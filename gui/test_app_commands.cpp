#include "app_commands.hpp"

#include "../codec_gui_image_io.hpp"

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <span>
#include <vector>

using namespace codec_gui;
using namespace codec_gui::gui;

namespace {

std::shared_ptr<const RawImage> make_image(uint8_t value) {
	auto image = std::make_shared<RawImage>();
	image->width = 4;
	image->height = 4;
	image->format = PixelFormat::YUV420P8;
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
	return {make_image(10), {}};
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

int main() {
	const std::filesystem::path root = std::filesystem::temp_directory_path();
	{
		Command command;
		command.kind = CommandKind::SaveBytesToFile;
		command.path = root / "codec_vis_command_save_test.bin";
		command.bytes = {std::byte{0x01}, std::byte{0x02}, std::byte{0x03}};
		command.value = 1.0;
		std::vector<Action> actions = execute_command(command, AppState{}, CommandContext{});
		assert(actions.size() == 1);
		assert(actions.front().kind == ActionKind::SaveCompleted);
		assert(std::filesystem::file_size(command.path) == 3);
	}

	{
		Command command;
		command.kind = CommandKind::SaveBytesToFile;
		command.path = root / "codec_vis_command_save_test.bin";
		command.bytes = {std::byte{0x04}};
		command.value = 0.0;
		std::vector<Action> actions = execute_command(command, AppState{}, CommandContext{});
		assert(actions.size() == 1);
		assert(actions.front().kind == ActionKind::SaveFailed);
	}

	{
		Command command;
		command.kind = CommandKind::QueryBackendCapabilities;
		command.backend = BackendId{999};
		std::vector<Action> actions = execute_command(command, AppState{}, CommandContext{});
		assert(actions.size() == 1);
		assert(actions.front().kind == ActionKind::BackendCapabilitiesFailed);
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
		assert(queued.commands.size() == 1);
		assert(queued.commands.front().kind == CommandKind::RunEncode);

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
		std::vector<Action> encodeActions = execute_command(queued.commands.front(), queued.state, CommandContext{backends});
		assert(encodeActions.size() == 2);
		state = std::move(queued.state);
		for (const Action& action : encodeActions) {
			state = update(std::move(state), action).state;
		}
		assert(state.encodeRuns.front().state == EncodeRunState::Completed);
		assert(state.images.size() == 2);
		assert(state.images.back().type == ImageObjectType::EncodedResult);
		assert(state.images.back().encoded->byteSize == 3);
		assert(state.images.back().encoded->bytes[1] == std::byte{0x22});
		assert(state.images.back().encoded->psnrY);

		Action assign;
		assign.kind = ActionKind::AssignImageToPane;
		assign.pane = state.panes.front().id;
		assign.image = state.images.back().id;
		state = update(std::move(state), assign).state;
		assert(state.panes.front().image == state.images.back().id);

		Command save;
		save.kind = CommandKind::SaveBytesToFile;
		save.image = state.images.back().id;
		save.path = root / "codec_vis_e2e_encoded.bin";
		save.bytes = state.images.back().encoded->bytes;
		save.value = 1.0;
		std::vector<Action> saveActions = execute_command(save, state, CommandContext{});
		assert(saveActions.size() == 1);
		assert(saveActions.front().kind == ActionKind::SaveCompleted);
		std::ifstream saved(save.path, std::ios::binary);
		std::vector<char> bytes((std::istreambuf_iterator<char>(saved)), std::istreambuf_iterator<char>());
		assert(bytes.size() == 3);
		assert(static_cast<unsigned char>(bytes[0]) == 0x11);
		assert(static_cast<unsigned char>(bytes[1]) == 0x22);
		assert(static_cast<unsigned char>(bytes[2]) == 0x33);
	}

	{
		AppState state;
		Action action;
		action.kind = ActionKind::SourcePathChosen;
		action.path = root / "does-not-exist.codec-vis";
		UpdateResult result = update(std::move(state), action);
		assert(result.commands.size() == 1);
		assert(result.commands.front().kind == CommandKind::LoadSourceImage);
		std::vector<Action> actions = execute_command(result.commands.front(), result.state, CommandContext{});
		assert(actions.size() == 1);
		assert(actions.front().kind == ActionKind::SourceLoadFailed);
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
		assert(actions.size() == 1);
		assert(actions.front().kind == ActionKind::DerivedImageComputed);
		assert(actions.front().derivedImage.image);
		assert(actions.front().derivedImage.image->planes[0].bytes.front() == 40);
		assert(actions.front().derivedImage.displayName == "Difference: first vs second");
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
		assert(result.commands.size() == 1);
		assert(result.commands.front().kind == CommandKind::ShowSaveFilePortal);
		assert(result.commands.front().image == encoded.id);
		assert(result.commands.front().path.extension() == ".ivf");
	}

	return 0;
}
