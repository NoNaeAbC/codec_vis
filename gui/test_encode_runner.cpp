#include "test_check.hpp"
#include "../codec_gui_x265.hpp"
#include "app_state.hpp"
#include "encode_runner.hpp"
#include "encoder_backends.hpp"
#include "raw_image_conversion.hpp"
#include <cmath>

#include <cassert>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <thread>
#include <vector>

using namespace codec_gui;
using namespace codec_gui::gui;

namespace {

EncodedImage fake_encode(const RawImage&, std::span<const EncoderParam>) {
	std::this_thread::sleep_for(std::chrono::milliseconds(20));
	EncodedImage out;
	out.hevcAnnexB = {std::byte{0x01}, std::byte{0x02}, std::byte{0x03}, std::byte{0x04}};
	return out;
}

DecodeResult fake_decode(const EncodedImage&) {
	return {.image=nullptr, .error="fake decoder unavailable"};
}

DecodeResult slow_decode(const EncodedImage&) {
	std::this_thread::sleep_for(std::chrono::milliseconds(120));
	return {.image=nullptr, .error={}};
}

CapabilityResult fake_caps() {
	CapabilityResult result;
	result.snapshot.implementation = "fake";
	result.snapshot.available = true;
	return result;
}

std::vector<ParamSummary> fake_summary(std::span<const EncoderParam>) {
	return {};
}

std::shared_ptr<const RawImage> make_image() {
	auto image = std::make_shared<RawImage>();
	image->width = 16;
	image->height = 16;
	image->format = PixelFormat::YUV420P8;
	image->color={.primaries=ColorPrimaries::BT709,.transfer=TransferCharacteristics::SRGB,.matrix=MatrixCoefficients::BT709,.range=ColorRange::Limited};
	image->planes[0].strideBytes = 16;
	image->planes[1].strideBytes = 8;
	image->planes[2].strideBytes = 8;
	image->planes[0].bytes.resize(static_cast<std::size_t>(16 * 16), 16);
	image->planes[1].bytes.resize(static_cast<std::size_t>(8 * 8), 128);
	image->planes[2].bytes.resize(static_cast<std::size_t>(8 * 8), 128);
	return image;
}

RawImage make_odd_image() {
	RawImage image;
	image.width = 15;
	image.height = 13;
	image.format = PixelFormat::YUV420P8;
	image.nominalPeakNits = 1000.0;
	image.planes[0].strideBytes = image.width;
	image.planes[1].strideBytes = (image.width + 1) / 2;
	image.planes[2].strideBytes = (image.width + 1) / 2;
	image.planes[0].bytes.resize(static_cast<std::size_t>(image.width * image.height));
	image.planes[1].bytes.resize(static_cast<std::size_t>(image.planes[1].strideBytes * ((image.height + 1) / 2)), 128);
	image.planes[2].bytes.resize(static_cast<std::size_t>(image.planes[2].strideBytes * ((image.height + 1) / 2)), 128);
	for (int y = 0; y < image.height; ++y) {
		for (int x = 0; x < image.width; ++x) {
			image.planes[0].bytes[static_cast<std::size_t>(y * image.width + x)] = static_cast<uint8_t>(x + y * image.width);
		}
	}
	return image;
}

} // namespace

int main() {
	EncoderBackend backend;
	backend.id = BackendId{77};
	backend.name = "fake";
	backend.codec = "FAKE";
	backend.kind = BackendKind::Software;
	backend.queryCapabilities = fake_caps;
	backend.encode = fake_encode;
	backend.decodePreview = fake_decode;
	backend.summarizeParams = fake_summary;

	EncoderBackend av1Backend = backend;
	av1Backend.codec = "AV1";
	av1Backend.inputGeometry = InputGeometryPolicy::PadToChromaAlignment;
	av1Backend.encode = [](const RawImage& image, std::span<const EncoderParam>) {
		CODEC_VIS_CHECK(image.width == 16);
		CODEC_VIS_CHECK(image.height == 14);
		CODEC_VIS_CHECK(image.planes[0].strideBytes == 16);
		CODEC_VIS_CHECK(image.planes[0].bytes[15] == image.planes[0].bytes[14]);
		CODEC_VIS_CHECK(image.planes[0].bytes[static_cast<std::size_t>(13 * 16)] == image.planes[0].bytes[static_cast<std::size_t>(12 * 16)]);
		return fake_encode(image, {});
	};
	const EncodeResult oddResult = run_backend_encode(av1Backend, make_odd_image(), {});
	CODEC_VIS_CHECK(oddResult.comparisonReference->width == 16);
	CODEC_VIS_CHECK(oddResult.comparisonReference->height == 14);
	CODEC_VIS_CHECK(oddResult.comparisonReference->nominalPeakNits == 1000.0);

	std::vector<EncoderBackend> backends{backend};

	AppState state;
	ImageObject image;
	image.id = next_image_id(state);
	image.type = ImageObjectType::Source;
	image.displayName = "source";
	image.width = 16;
	image.height = 16;
	image.decoded = make_image();
	state.images.push_back(image);
	EncodeRun run;
	run.id = next_run_id(state);
	run.source = image.id;
	run.backend = backend.id;
	state.encodeRuns.push_back(run);

	Command command;
	command.kind = CommandKind::RunEncode;
	command.run = run.id;
	command.image = image.id;
	command.backend = backend.id;

	EncodeRunner runner(backends);
	runner.submit(command, state);

	bool sawStarted = false;
	bool sawCompleted = false;
	for (int i = 0; i < 100 && !sawCompleted; ++i) {
		for (const Action& action : runner.take_actions()) {
			if (action.kind == ActionKind::EncodeRunStarted) {
				sawStarted = true;
				CODEC_VIS_CHECK(action.run == run.id);
				CODEC_VIS_CHECK(action.value >= 0.0);
			}
			if (action.kind == ActionKind::EncodeRunCompleted) {
				sawCompleted = true;
				CODEC_VIS_CHECK(action.encodeCompleted.run == run.id);
				CODEC_VIS_CHECK(action.encodeCompleted.metadata.byteSize == 4);
				CODEC_VIS_CHECK(action.encodeCompleted.metadata.backendName == "fake");
				CODEC_VIS_CHECK(action.encodeCompleted.metadata.encoderInput);
				CODEC_VIS_CHECK(action.encodeCompleted.metadata.encoderInput->planes[0].bytes == image.decoded->planes[0].bytes);
				CODEC_VIS_CHECK(action.encodeCompleted.metadata.previewError == "fake decoder unavailable");
				CODEC_VIS_CHECK(!action.encodeCompleted.metadata.metricError.empty());
				CODEC_VIS_CHECK(action.value > 0.0);
			}
		}
		std::this_thread::sleep_for(std::chrono::milliseconds(5));
	}
	CODEC_VIS_CHECK(sawStarted);
	CODEC_VIS_CHECK(sawCompleted);

	EncoderBackend slowBackend = backend;
	slowBackend.decodePreview = slow_decode;
	std::vector<EncoderBackend> slowBackends{slowBackend};

	AppState twoRunState;
	ImageObject twoRunImage;
	twoRunImage.id = next_image_id(twoRunState);
	twoRunImage.type = ImageObjectType::Source;
	twoRunImage.displayName = "source";
	twoRunImage.width = 16;
	twoRunImage.height = 16;
	twoRunImage.decoded = make_image();
	twoRunState.images.push_back(twoRunImage);
	EncodeRun first;
	first.id = next_run_id(twoRunState);
	first.source = twoRunImage.id;
	first.backend = slowBackend.id;
	twoRunState.encodeRuns.push_back(first);
	EncodeRun second;
	second.id = next_run_id(twoRunState);
	second.source = twoRunImage.id;
	second.backend = slowBackend.id;
	twoRunState.encodeRuns.push_back(second);

	Command firstCommand = command;
	firstCommand.run = first.id;
	firstCommand.image = twoRunImage.id;
	firstCommand.backend = slowBackend.id;
	Command secondCommand = firstCommand;
	secondCommand.run = second.id;

	EncodeRunner splitRunner(slowBackends);
	splitRunner.submit(firstCommand, twoRunState);
	splitRunner.submit(secondCommand, twoRunState);

	bool sawSecondStartedBeforeAnyCompletion = false;
	bool sawAnyCompletionBeforeSecondStarted = false;
	int completedCount = 0;
	for (int i = 0; i < 200 && completedCount < 2; ++i) {
		for (const Action& action : splitRunner.take_actions()) {
			if (action.kind == ActionKind::EncodeRunCompleted) {
				++completedCount;
				if (!sawSecondStartedBeforeAnyCompletion) {
					sawAnyCompletionBeforeSecondStarted = true;
				}
			}
			if (action.kind == ActionKind::EncodeRunStarted && action.run == second.id && completedCount == 0) {
				sawSecondStartedBeforeAnyCompletion = true;
			}
		}
		std::this_thread::sleep_for(std::chrono::milliseconds(5));
	}
	CODEC_VIS_CHECK(sawSecondStartedBeforeAnyCompletion);
	CODEC_VIS_CHECK(!sawAnyCompletionBeforeSecondStarted);
	CODEC_VIS_CHECK(completedCount == 2);

	{
		// Decoder metadata survives unchanged; metrics wait for a request.
		EncoderBackend mismatched = backend;
		mismatched.decodePreview = [](const EncodedImage&) {
			auto decoded = std::make_shared<RawImage>(*make_image());
			decoded->color.transfer = TransferCharacteristics::PQ;
			return DecodeResult{.image=decoded, .error={}};
		};
		EncodeRunner checked(std::span<const EncoderBackend>(&mismatched, 1));
		checked.submit(command, state);
		bool completed = false;
		for (int i = 0; i < 200 && !completed; ++i) {
			for (const auto& action : checked.take_actions()) {
				CODEC_VIS_CHECK(action.kind != ActionKind::EncodeRunFailed);
				if (action.kind != ActionKind::EncodeRunCompleted) continue;
				completed = true;
				CODEC_VIS_CHECK(action.encodeCompleted.preview->color.transfer == TransferCharacteristics::PQ);
				CODEC_VIS_CHECK(action.encodeCompleted.metadata.codedColor.transfer == TransferCharacteristics::PQ);
				CODEC_VIS_CHECK(!action.encodeCompleted.metadata.psnrY);
				CODEC_VIS_CHECK(action.encodeCompleted.metadata.metricError.empty());
				CODEC_VIS_CHECK(action.encodeCompleted.metadata.metricState==MetricState::NotRequested);
			}
			std::this_thread::sleep_for(std::chrono::milliseconds(5));
		}
		CODEC_VIS_CHECK(completed);
	}
	{
		// Quantization before encoding must remain visible in end-to-end quality.
		AppState precise = state;
		auto input = std::make_shared<RawImage>(convert_raw_image_format(*make_image(), PixelFormat::YUV420P10LE));
		for (std::size_t i = 0; i < input->planes[0].bytes.size(); i += 2) {
			input->planes[0].bytes[i] = 65;
			input->planes[0].bytes[i + 1] = 0;
		}
		precise.images[0].decoded = input;
		EncoderBackend quantized = backend;
		quantized.decodePreview = [](const EncodedImage&) { return DecodeResult{.image=make_image(), .error={}}; };
		EncodeRunner checked(std::span<const EncoderBackend>(&quantized, 1));
		Command converted = command;
		converted.params = {{.name="bit-depth", .value=std::string{"8"}}};
		checked.submit(converted, precise);
		bool completed = false;
		for (int i = 0; i < 200 && !completed; ++i) {
			for (const auto& action : checked.take_actions()) {
				CODEC_VIS_CHECK(action.kind != ActionKind::EncodeRunFailed);
				if (action.kind != ActionKind::EncodeRunCompleted) continue;
				completed = true;
				CODEC_VIS_CHECK(!action.encodeCompleted.metadata.psnrY);
				CODEC_VIS_CHECK(action.encodeCompleted.metadata.metrics.empty());
				const auto metric=compute_psnr(*input,*action.encodeCompleted.preview);
				CODEC_VIS_CHECK(metric.psnrY && std::isfinite(*metric.psnrY));
				CODEC_VIS_CHECK(action.encodeCompleted.metadata.metricReference == "source (end-to-end)");
			}
			std::this_thread::sleep_for(std::chrono::milliseconds(5));
		}
		CODEC_VIS_CHECK(completed);
	}
	{
		// Metric work must not delay an independent decoded result. Hold the
		// first evaluator at a deterministic gate while encoding and decoding.
		std::atomic<bool> entered{false},releaseMetric{false};
		EncoderBackend decodable=backend;
		decodable.decodePreview=[](const EncodedImage&){return DecodeResult{.image=make_image(),.error={}};};
		AppState measuring=state;
		ImageObject candidate=measuring.images.front();
		candidate.id=next_image_id(measuring);candidate.type=ImageObjectType::EncodedResult;
		candidate.parents={measuring.images.front().id};candidate.encoded.emplace();
		measuring.images.push_back(candidate);
		EncodeRunner checked(std::span<const EncoderBackend>(&decodable,1),[&](std::string_view id,const LinearBt2020Image& a,const LinearBt2020Image& b){
			entered=true;while(!releaseMetric)std::this_thread::sleep_for(std::chrono::milliseconds(1));
			return compute_metric(id,a,b);
		});
		Command metric;metric.kind=CommandKind::ComputeMetric;metric.image=candidate.id;metric.otherImage=candidate.parents.front();
		checked.submit_metrics(metric,measuring);
		for(int i=0;i<1000 && !entered;++i)std::this_thread::sleep_for(std::chrono::milliseconds(1));
		CODEC_VIS_CHECK(entered);
		checked.submit(command,measuring);
		bool decoded=false;
		for(int i=0;i<1000 && !decoded;++i) {
			for(const auto& a:checked.take_actions()) {
				CODEC_VIS_CHECK(a.kind!=ActionKind::MetricComputed);
				if(a.kind==ActionKind::EncodeRunCompleted) {
					decoded=true;CODEC_VIS_CHECK(a.encodeCompleted.preview);
					CODEC_VIS_CHECK(a.encodeCompleted.metadata.metrics.empty());
					CODEC_VIS_CHECK(a.encodeCompleted.metadata.metricSeconds==0);
				}
			}
			std::this_thread::sleep_for(std::chrono::milliseconds(1));
		}
		releaseMetric=true;CODEC_VIS_CHECK(decoded);
		bool done=false;std::size_t computed=0;
		for(int i=0;i<2000 && !done;++i) {
			for(const auto& a:checked.take_actions()) {
				CODEC_VIS_CHECK(a.kind!=ActionKind::MetricsFailed);
				if(a.kind==ActionKind::MetricComputed){++computed;CODEC_VIS_CHECK(a.image==candidate.id && a.metric.value);}
				done|=a.kind==ActionKind::MetricsCompleted;
			}
			std::this_thread::sleep_for(std::chrono::milliseconds(1));
		}
		CODEC_VIS_CHECK(done && computed==metric_catalog().size());
        // A subsequent chosen-metric request must never evaluate the rest.
        metric.metricIds={"ssim"};checked.submit_metrics(metric,measuring);
        done=false;computed=0;
        for(int i=0;i<2000&&!done;++i){
            for(const auto& a:checked.take_actions()){
                CODEC_VIS_CHECK(a.kind!=ActionKind::MetricsFailed);
                if(a.kind==ActionKind::MetricComputed){++computed;CODEC_VIS_CHECK(a.metric.id=="ssim");}
                done|=a.kind==ActionKind::MetricsCompleted;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        CODEC_VIS_CHECK(done&&computed==1);

	}
	{
		EncodeRunner canceled(backends);
		canceled.request_cancel(command.run);
		canceled.submit(command, state);
		bool acknowledged = false;
		for (int i = 0; i < 200 && !acknowledged; ++i) {
			for (const auto& action : canceled.take_actions()) {
				CODEC_VIS_CHECK(action.kind != ActionKind::EncodeRunStarted);
				CODEC_VIS_CHECK(action.kind != ActionKind::EncodeRunCompleted);
				acknowledged |= action.kind == ActionKind::EncodeRunCanceled;
			}
			std::this_thread::sleep_for(std::chrono::milliseconds(5));
		}
		CODEC_VIS_CHECK(acknowledged);
	}
	{
		// Closing with queued work must finish only the active native call.
		std::atomic<int> calls{0};
		std::atomic<bool> release{false};
		EncoderBackend blocked = backend;
		blocked.encode = [&](const RawImage&, std::span<const EncoderParam>) {
			++calls;
			while (!release.load()) std::this_thread::sleep_for(std::chrono::milliseconds(1));
			return EncodedImage{};
		};
		auto closing = std::make_unique<EncodeRunner>(std::span<const EncoderBackend>(&blocked, 1));
		for (int i = 0; i < 100; ++i) closing->submit(command, state);
		for (int i = 0; i < 1000 && calls.load() == 0; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(1));
		CODEC_VIS_CHECK(calls.load() == 1);
		std::thread shutdown([&] { closing.reset(); });
		std::this_thread::sleep_for(std::chrono::milliseconds(30));
		release = true;
		shutdown.join();
		CODEC_VIS_CHECK(calls.load() == 1);
	}
	return 0;
}
