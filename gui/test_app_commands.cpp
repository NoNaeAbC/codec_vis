#include "test_check.hpp"
#include "metrics.hpp"
#include <algorithm>
#include "../codec_gui_x265.hpp"
#include "app_commands.hpp"

#include "app_state.hpp"
#include "encoder_backends.hpp"

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <ios>
#include <iterator>
#include <memory>
#include <span>
#include <utility>
#include <vector>

using namespace codec_gui;
using namespace codec_gui::gui;

namespace {

std::shared_ptr<const RawImage> make_image(uint8_t value) {
	auto image = std::make_shared<RawImage>();
	image->width = 4;
	image->height = 4;
	image->format = PixelFormat::YUV420P8;
	image->color={.primaries=ColorPrimaries::BT709,.transfer=TransferCharacteristics::SRGB,.matrix=MatrixCoefficients::BT709,.range=ColorRange::Limited};
	image->planes[0].strideBytes = 4;
	image->planes[1].strideBytes = 2;
	image->planes[2].strideBytes = 2;
	image->planes[0].bytes.resize(16, value);
	image->planes[1].bytes.resize(4, 128);
	image->planes[2].bytes.resize(4, 128);
	return image;
}

EncodedImage fake_encode(const RawImage&, std::span<const EncoderParam>) {
	EncodedImage encoded;
	encoded.hevcAnnexB = {std::byte{0x11}, std::byte{0x22}, std::byte{0x33}};
	return encoded;
}

DecodeResult fake_decode(const EncodedImage&) {
	return {.image=make_image(10), .error={}};
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

} // namespace

int main(int argc,char** argv) {
	const bool metricsOnly=argc==2 && std::string_view(argv[1])=="--metrics-only";
	const std::filesystem::path root = std::filesystem::temp_directory_path();
	if(!metricsOnly) {
	{
		Command command;
		command.kind = CommandKind::SaveBytesToFile;
		command.path = root / "codec_vis_command_save_test.bin";
		command.bytes = {std::byte{0x01}, std::byte{0x02}, std::byte{0x03}};
		command.value = 1.0;
		std::vector<Action> actions = execute_command(command, AppState{}, CommandContext{});
		CODEC_VIS_CHECK(actions.size() == 1);
		CODEC_VIS_CHECK(actions.front().kind == ActionKind::SaveCompleted);
		CODEC_VIS_CHECK(std::filesystem::file_size(command.path) == 3);
	}

	{
		Command command;
		command.kind = CommandKind::SaveBytesToFile;
		command.path = root / "codec_vis_command_save_test.bin";
		command.bytes = {std::byte{0x04}};
		command.value = 0.0;
		std::vector<Action> actions = execute_command(command, AppState{}, CommandContext{});
		CODEC_VIS_CHECK(actions.size() == 1);
		CODEC_VIS_CHECK(actions.front().kind == ActionKind::SaveFailed);
	}

	{
		Command command;
		command.kind = CommandKind::QueryBackendCapabilities;
		command.backend = BackendId{999};
		std::vector<Action> actions = execute_command(command, AppState{}, CommandContext{});
		CODEC_VIS_CHECK(actions.size() == 1);
		CODEC_VIS_CHECK(actions.front().kind == ActionKind::BackendCapabilitiesFailed);
	}

	}
	{
		AppState state;
		Action loaded;
		loaded.kind = ActionKind::SourceLoaded;
		loaded.sourceLoaded.path = "source.png";
		loaded.sourceLoaded.image = make_image(10);
		state = update(std::move(state), loaded).state;

		BackendInfo info;
		info.id = BackendId{42};
		info.name = "fake";
		info.codec = "FAKE";
		info.capabilities.implementation = "fake";
		state.backends.push_back(info);
		state.selection.selectedBackend = info.id;

		Action start;
		start.kind = ActionKind::StartEncodeRun;
		start.image = state.images.front().id;
		start.backend = info.id;
		UpdateResult queued = update(std::move(state), start);
		CODEC_VIS_CHECK(queued.commands.size() == 1);
		CODEC_VIS_CHECK(queued.commands.front().kind == CommandKind::RunEncode);

		EncoderBackend backend;
		backend.id = info.id;
		backend.name = "fake";
		backend.codec = "FAKE";
		backend.kind = BackendKind::Software;
		backend.queryCapabilities = fake_caps;
		backend.encode = fake_encode;
		backend.decodePreview = fake_decode;
		backend.summarizeParams = fake_summary;
		std::vector<EncoderBackend> backends{backend};
		std::vector<Action> const encodeActions = execute_command(queued.commands.front(), queued.state, CommandContext{backends});
		CODEC_VIS_CHECK(encodeActions.size() == 2);
		state = std::move(queued.state);
		for (const Action& action : encodeActions) {
			state = update(std::move(state), action).state;
		}
		CODEC_VIS_CHECK(state.encodeRuns.front().state == EncodeRunState::Completed);
		CODEC_VIS_CHECK(state.images.size() == 2);
		CODEC_VIS_CHECK(state.images.back().type == ImageObjectType::EncodedResult);
		CODEC_VIS_CHECK(state.images.back().encoded->byteSize == 3);
		CODEC_VIS_CHECK(state.images.back().encoded->bytes[1] == std::byte{0x22});
		CODEC_VIS_CHECK(!state.images.back().encoded->psnrY);
		CODEC_VIS_CHECK(state.images.back().decoded);
		CODEC_VIS_CHECK(state.images.back().encoded->metricState==MetricState::NotRequested);
		Action request;request.kind=ActionKind::RequestMetrics;request.image=state.images.back().id;
		auto metricsQueued=update(state,request);
		CODEC_VIS_CHECK(metricsQueued.state.images.back().encoded->metricState==MetricState::Queued);
		const auto metricCommand=std::ranges::find_if(metricsQueued.commands,[](const auto& c){return c.kind==CommandKind::ComputeMetric;});
		CODEC_VIS_CHECK(metricCommand!=metricsQueued.commands.end());
		const auto duplicate=update(metricsQueued.state,request);
		CODEC_VIS_CHECK(std::ranges::none_of(duplicate.commands,[](const auto& c){return c.kind==CommandKind::ComputeMetric;}));
		const auto metricActions=execute_command(*metricCommand,metricsQueued.state,CommandContext{backends});
		for(const auto& a:metricActions) metricsQueued.state=update(std::move(metricsQueued.state),a).state;
		CODEC_VIS_CHECK(metricsQueued.state.images.back().encoded->metricState==MetricState::Ready);
		CODEC_VIS_CHECK(metricsQueued.state.images.back().encoded->psnrY);
		CODEC_VIS_CHECK(metricsQueued.state.images.back().encoded->metrics.size()==metric_catalog().size());
		// Completion after deleting/replacing a result must not resurrect it.
		Action remove;remove.kind=ActionKind::RemoveImage;remove.image=request.image;
		auto removed=update(metricsQueued.state,remove).state;
		for(const auto& a:metricActions) removed=update(std::move(removed),a).state;
		CODEC_VIS_CHECK(find_image(removed,request.image)==nullptr);
		backends.front().decodePreview = [](const EncodedImage&) {
			auto decoded = std::make_shared<RawImage>(*make_image(10));
			decoded->color.transfer = TransferCharacteristics::PQ;
			return DecodeResult{.image=decoded, .error={}};
		};
		const auto mismatched = execute_command(queued.commands.front(), state, CommandContext{backends});
		CODEC_VIS_CHECK(mismatched.size() == 2);
		CODEC_VIS_CHECK(mismatched.back().encodeCompleted.preview->color.transfer == TransferCharacteristics::PQ);
		CODEC_VIS_CHECK(!mismatched.back().encodeCompleted.metadata.psnrY);
		CODEC_VIS_CHECK(mismatched.back().encodeCompleted.metadata.metricError.empty());

		Action assign;
		assign.kind = ActionKind::AssignImageToPane;
		assign.pane = state.panes.front().id;
		assign.image = state.images.back().id;
		state = update(std::move(state), assign).state;
		CODEC_VIS_CHECK(state.panes.front().image == state.images.back().id);

		if(metricsOnly) return 0;
		Command save;
		save.kind = CommandKind::SaveBytesToFile;
		save.image = state.images.back().id;
		save.path = root / "codec_vis_e2e_encoded.bin";
		save.bytes = state.images.back().encoded->bytes;
		save.value = 1.0;
		std::vector<Action> saveActions = execute_command(save, state, CommandContext{});
		CODEC_VIS_CHECK(saveActions.size() == 1);
		CODEC_VIS_CHECK(saveActions.front().kind == ActionKind::SaveCompleted);
		std::ifstream saved(save.path, std::ios::binary);
		std::vector<char> bytes((std::istreambuf_iterator<char>(saved)), std::istreambuf_iterator<char>());
		CODEC_VIS_CHECK(bytes.size() == 3);
		CODEC_VIS_CHECK(static_cast<unsigned char>(bytes[0]) == 0x11);
		CODEC_VIS_CHECK(static_cast<unsigned char>(bytes[1]) == 0x22);
		CODEC_VIS_CHECK(static_cast<unsigned char>(bytes[2]) == 0x33);
	}

	{
		AppState state;
		Action action;
		action.kind = ActionKind::SourcePathChosen;
		action.path = root / "does-not-exist.codec-vis";
		UpdateResult result = update(std::move(state), action);
		CODEC_VIS_CHECK(result.commands.size() == 1);
		CODEC_VIS_CHECK(result.commands.front().kind == CommandKind::LoadSourceImage);
		std::vector<Action> actions = execute_command(result.commands.front(), result.state, CommandContext{});
		CODEC_VIS_CHECK(actions.size() == 1);
		CODEC_VIS_CHECK(actions.front().kind == ActionKind::SourceLoadFailed);
	}

	{
		AppState state;
		ImageObject first;
		first.id = ImageId{1};
		first.displayName = "first";
		first.width = 4;
		first.height = 4;
		first.decoded = make_image(10);
		ImageObject second = first;
		second.id = ImageId{2};
		second.displayName = "second";
		second.decoded = make_image(30);
		state.images = {first, second};

		Command command;
		command.kind = CommandKind::ComputeDerivedImage;
		command.image = first.id;
		command.otherImage = second.id;
		command.value = 2.0;
		std::vector<Action> actions = execute_command(command, state, CommandContext{});
		CODEC_VIS_CHECK(actions.size() == 1);
		CODEC_VIS_CHECK(actions.front().kind == ActionKind::DerivedImageComputed);
		CODEC_VIS_CHECK(actions.front().derivedImage.image);
		CODEC_VIS_CHECK(actions.front().derivedImage.image->planes[0].bytes.front() == 40);
		CODEC_VIS_CHECK(actions.front().derivedImage.displayName == "Difference: first vs second");
	}

	{
		AppState state;
		ImageObject source;
		source.id = ImageId{1};
		source.type = ImageObjectType::Source;
		source.displayName = "frog.png";
		state.images.push_back(source);
		ImageObject encoded;
		encoded.id = ImageId{2};
		encoded.type = ImageObjectType::EncodedResult;
		encoded.displayName = "encoded";
		encoded.parents.push_back(source.id);
		EncodedMetadata metadata;
		metadata.codecName = "AV1";
		metadata.backendName = "VAAPI";
		metadata.bytes = {std::byte{0x01}};
		metadata.byteSize = 1;
		encoded.encoded = metadata;
		state.images.push_back(encoded);

		Action save;
		save.kind = ActionKind::SaveEncodedResult;
		save.image = encoded.id;
		UpdateResult result = update(std::move(state), save);
		CODEC_VIS_CHECK(result.commands.size() == 1);
		CODEC_VIS_CHECK(result.commands.front().kind == CommandKind::ShowSaveFilePortal);
		CODEC_VIS_CHECK(result.commands.front().image == encoded.id);
		CODEC_VIS_CHECK(result.commands.front().path.extension() == ".ivf");
	}

	return 0;
}
