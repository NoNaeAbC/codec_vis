#include "encode_runner.hpp"
#include "raw_image_utils.hpp"
#include "test_check.hpp"
#include <condition_variable>
#include <iostream>
#include <mutex>
using namespace codec_gui;
using namespace codec_gui::gui;
#define CHECK CODEC_VIS_CHECK
static std::shared_ptr<RawImage> source_image() {
	auto raw   = std::make_shared<RawImage>();
	raw->width = raw->height = 8;
	raw->format              = PixelFormat::YUV420P8;
	raw->color               = {ColorPrimaries::BT709,
								TransferCharacteristics::BT709,
								MatrixCoefficients::BT709,
								ColorRange::Limited,
								{}};
	for (int p = 0; p < 3; ++p) {
		raw->planes[p].strideBytes = plane_width(*raw, p);
		raw->planes[p].bytes.resize(plane_width(*raw, p) * plane_height(*raw, p), p ? 128 : 64);
	}
	return raw;
}
static DecodeResult decoded(const EncodedImage &) { return {.image = source_image(), .error = {}}; }
int                 main() {
	auto           source = source_image();
	EncoderBackend backend;
	backend.id                 = {9};
	backend.name               = "Controlled";
	backend.codec              = "Controlled";
	backend.decodePreview      = decoded;
	backend.summarizeParams    = [](std::span<const EncoderParam>) { return std::vector<ParamSummary>{}; };
	const RawImage *observed   = nullptr;
	int64_t         observedQp = 0;
	backend.encode             = [&](const RawImage &input, std::span<const EncoderParam> params) {
		observed = &input;
		for (const auto &p: params)
			if (p.name == "qp") observedQp = std::get<int64_t>(p.value);
		EncodedImage e;
		e.hevcAnnexB = {std::byte{1}};
		return e;
	};
	EncodePreparationCache cache;
	auto first = run_backend_encode(backend, source, {}, cache); // No conversion: borrow immutable source.
	CHECK(observed == source.get());
	CHECK(!first.metadata.preparationCacheHit);
	std::vector<EncoderParam> p{{"qp", int64_t(2)}, {"bit-depth", std::string("10")}};
	auto                      converted = run_backend_encode(backend, source, p, cache);
	auto                     *prepared  = observed;
	CHECK(prepared != source.get() && prepared->format == PixelFormat::YUV420P10LE);
	p[0].value   = int64_t(43);
	auto changed = run_backend_encode(backend, source, p, cache);
	CHECK(changed.metadata.preparationCacheHit && observed == prepared && observedQp == 43);
	p.push_back({"range", std::string("full")});
	auto recolored = run_backend_encode(backend, source, p, cache);
	CHECK(!recolored.metadata.preparationCacheHit && observed != prepared);
	auto replacement = source_image();
	CHECK(!run_backend_encode(backend, replacement, p, cache).metadata.preparationCacheHit);

	AppState    state;
	BackendInfo info;
	info.id                         = backend.id;
	info.name                       = backend.name;
	state.backends                  = {info};
	state.selection.selectedBackend = backend.id;
	Action loaded;
	loaded.kind               = ActionKind::SourceLoaded;
	loaded.sourceLoaded.image = source;
	state                     = update(std::move(state), loaded).state;
	auto sourceId             = state.selection.selectedImage;
	auto request              = [&](int64_t qp) {
		Action a;
		a.kind    = ActionKind::RequestLivePreview;
		a.image   = sourceId;
		a.backend = backend.id;
		a.params  = {{"qp", qp}};
		auto r    = update(std::move(state), a);
		state     = std::move(r.state);
		CHECK(r.commands.size() == 1);
		return r.commands[0];
	};
	auto   a = request(1), b = request(2);
	Action done;
	done.kind                    = ActionKind::LivePreviewCompleted;
	done.image                   = sourceId;
	done.backend                 = backend.id;
	done.revision                = a.revision;
	done.params                  = a.params;
	done.encodeCompleted.preview = source_image();
	state                        = update(std::move(state), done).state;
	CHECK(state.livePreview.displayed == a.revision &&
		  state.livePreview.busy); // Active work remains visible while dragging.
	auto previewId               = state.livePreview.image;
	done.revision                = b.revision;
	done.params                  = b.params;
	done.encodeCompleted.preview = source_image();
	state                        = update(std::move(state), done).state;
	CHECK(state.livePreview.image == previewId && !state.livePreview.busy && state.images.size() == 2 &&
		  state.encodeRuns.empty());
	auto revision = find_image(state, previewId)->previewRevision;
	done.revision = a.revision;
	state         = update(std::move(state), done).state;
	CHECK(find_image(state, previewId)->previewRevision == revision);
	Action off;
	off.kind      = ActionKind::SetLivePreviewEnabled;
	off.value     = 0;
	state         = update(std::move(state), off).state;
	done.revision = b.revision;
	state         = update(std::move(state), done).state;
	CHECK(find_image(state, previewId)->previewRevision == revision);
	off.value                            = 1;
	state                                = update(std::move(state), off).state;
	find_image(state, sourceId)->decoded = source_image();
	auto refreshed                       = request(2);
	CHECK(state.livePreview.minimumGeneration == refreshed.revision);
	done.revision = b.revision;
	state         = update(std::move(state), done).state;
	CHECK(find_image(state, previewId)->previewRevision == revision); // Old source revision cannot reappear.

	// Hold the active codec call while 200 edits arrive. Only the newest pending
	// edit may run afterwards. No timing-sensitive sleeps or discarded samples.
	std::mutex              mutex;
	std::condition_variable cv;
	bool                    entered = false, release = false;
	std::vector<int64_t>    encoded;
	backend.encode = [&](const RawImage &, std::span<const EncoderParam> params) {
		auto             qp = std::get<int64_t>(params[0].value);
		std::unique_lock lock(mutex);
		encoded.push_back(qp);
		if (encoded.size() == 1) {
			entered = true;
			cv.notify_all();
			cv.wait(lock, [&] { return release; });
		}
		EncodedImage e;
		e.hevcAnnexB = {std::byte{1}};
		return e;
	};
	EncodeRunner runner({&backend, 1});
	auto         initial = request(10);
	runner.submit(initial, state);
	{
		std::unique_lock lock(mutex);
		CHECK(cv.wait_for(lock, std::chrono::seconds(2), [&] { return entered; }));
	}
	Command last;
	for (int i = 11; i <= 210; ++i) {
		last = request(i);
		runner.submit(last, state);
	}
	{
		std::scoped_lock lock(mutex);
		release = true;
	}
	cv.notify_all();
	const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
	bool       finished = false;
	while (std::chrono::steady_clock::now() < deadline && !finished) {
		for (const auto &event: runner.take_actions()) {
			if (event.kind == ActionKind::LivePreviewCompleted && event.revision == last.revision) finished = true;
			state = update(std::move(state), event).state;
		}
		std::this_thread::yield();
	}
	CHECK(finished);
	{
		std::scoped_lock lock(mutex);
		CHECK(encoded == std::vector<int64_t>({10, 210}));
	}
	CHECK(state.images.size() == 2 && state.encodeRuns.empty() && !state.livePreview.busy);
	CHECK(std::get<int64_t>(state.livePreview.shownParams[0].value) == 210);
	{
		bool normalEntered = false, normalReleased = false;
		backend.encode = [&](const RawImage &, std::span<const EncoderParam> params) {
			if (std::get<int64_t>(params[0].value) == -1) {
				std::unique_lock lock(mutex);
				normalEntered = true;
				cv.notify_all();
				cv.wait(lock, [&] { return normalReleased; });
			}
			EncodedImage e;
			e.hevcAnnexB = {std::byte{1}};
			return e;
		};
		EncodeRunner independent({&backend, 1});
		Action       normal;
		normal.kind       = ActionKind::StartEncodeRun;
		normal.image      = sourceId;
		normal.backend    = backend.id;
		normal.params     = {{"qp", int64_t(-1)}};
		auto normalUpdate = update(std::move(state), normal);
		state             = std::move(normalUpdate.state);
		independent.submit(normalUpdate.commands.front(), state);
		{
			std::unique_lock lock(mutex);
			CHECK(cv.wait_for(lock, std::chrono::seconds(2), [&] { return normalEntered; }));
		}
		auto live = request(211);
		independent.submit(live, state);
		bool liveDone = false;
		auto limit    = std::chrono::steady_clock::now() + std::chrono::seconds(2);
		while (std::chrono::steady_clock::now() < limit && !liveDone) {
			for (const auto &a: independent.take_actions())
				if (a.kind == ActionKind::LivePreviewCompleted && a.revision == live.revision) liveDone = true;
			std::this_thread::yield();
		}
		CHECK(liveDone); // A retained encode is still held in its codec call.
		{
			std::scoped_lock lock(mutex);
			CHECK(!normalReleased);
			normalReleased = true;
		}
		cv.notify_all();
	}
	std::cout << "Live feedback: immutable input cache/invalidation, progressive display, bounded latest-edit queue, "
				 "final value, stale completion, pause guards and independence from retained encodes passed\n";
}
