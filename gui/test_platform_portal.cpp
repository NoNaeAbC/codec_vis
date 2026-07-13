#include "platform_portal.hpp"

#include <cassert>
#include <filesystem>

using namespace codec_gui::gui;

int main() {
	const std::filesystem::path root = std::filesystem::temp_directory_path();
	assert(decode_portal_file_uri("file://" + (root / "frog.png").string()) == root / "frog.png");
	assert(decode_portal_file_uri("file://" + (root / "frog%20image%23one.png").string()) == root / "frog image#one.png");
	assert(decode_portal_file_uri("file://localhost" + (root / "frog.png").string()) == root / "frog.png");
	assert(decode_portal_file_uri((root / "plain-path.png").string()) == root / "plain-path.png");
	assert(decode_portal_file_uri("file://" + (root / "bad%zzescape.png").string()) == root / "bad%zzescape.png");

	Action canceledOpen = action_from_open_portal_response(1, {});
	assert(canceledOpen.kind == ActionKind::OpenSourceCanceled);
	Action emptyOpen = action_from_open_portal_response(0, {});
	assert(emptyOpen.kind == ActionKind::OpenSourceCanceled);
	Action chosen = action_from_open_portal_response(0, {"file://" + (root / "frog%20image.jpg").string()});
	assert(chosen.kind == ActionKind::SourcePathChosen);
	assert(chosen.path == root / "frog image.jpg");

	Action canceledSave = action_from_save_portal_response(ImageId{9}, 2, {});
	assert(canceledSave.kind == ActionKind::SaveCanceled);
	assert(canceledSave.image == ImageId{9});
	assert(canceledSave.text.empty());
	Action save = action_from_save_portal_response(ImageId{9}, 0, {"file://" + (root / "result.ivf").string()});
	assert(save.kind == ActionKind::SaveEncodedResult);
	assert(save.image == ImageId{9});
	assert(save.path == root / "result.ivf");
	assert(save.value == 1.0);
	return 0;
}
