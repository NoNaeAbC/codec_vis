#include "encode_runner.hpp"

#include <cassert>
#include <chrono>
#include <cstddef>
#include <memory>
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
	return {nullptr, "fake decoder unavailable"};
}

DecodeResult slow_decode(const EncodedImage&) {
	std::this_thread::sleep_for(std::chrono::milliseconds(120));
	return {nullptr, {}};
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
	image->planes[0].strideBytes = 16;
	image->planes[1].strideBytes = 8;
	image->planes[2].strideBytes = 8;
	image->planes[0].bytes.resize(16 * 16, 16);
	image->planes[1].bytes.resize(8 * 8, 128);
	image->planes[2].bytes.resize(8 * 8, 128);
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
				assert(action.run == run.id);
				assert(action.value >= 0.0);
			}
			if (action.kind == ActionKind::EncodeRunCompleted) {
				sawCompleted = true;
				assert(action.encodeCompleted.run == run.id);
				assert(action.encodeCompleted.metadata.byteSize == 4);
				assert(action.encodeCompleted.metadata.backendName == "fake");
				assert(action.encodeCompleted.metadata.previewError == "fake decoder unavailable");
				assert(!action.encodeCompleted.metadata.metricError.empty());
				assert(action.value > 0.0);
			}
		}
		std::this_thread::sleep_for(std::chrono::milliseconds(5));
	}
	assert(sawStarted);
	assert(sawCompleted);

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
	assert(sawSecondStartedBeforeAnyCompletion);
	assert(!sawAnyCompletionBeforeSecondStarted);
	assert(completedCount == 2);

	return 0;
}
