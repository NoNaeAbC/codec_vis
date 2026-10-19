#include "test_check.hpp"
#include "app_state.hpp"
#include "platform_portal.hpp"

#include <cassert>
#include <filesystem>

using namespace codec_gui::gui;

int main() {
	const std::filesystem::path root = std::filesystem::temp_directory_path();
	CODEC_VIS_CHECK(decode_portal_file_uri("file://" + (root / "frog.png").string()) == root / "frog.png");
	CODEC_VIS_CHECK(decode_portal_file_uri("file://" + (root / "frog%20image%23one.png").string()) == root / "frog image#one.png");
	CODEC_VIS_CHECK(decode_portal_file_uri("file://localhost" + (root / "frog.png").string()) == root / "frog.png");
	CODEC_VIS_CHECK(decode_portal_file_uri((root / "plain-path.png").string()) == root / "plain-path.png");
	CODEC_VIS_CHECK(decode_portal_file_uri("file://" + (root / "bad%zzescape.png").string()) == root / "bad%zzescape.png");

	Action const canceledOpen = action_from_open_portal_response(1, {});
	CODEC_VIS_CHECK(canceledOpen.kind == ActionKind::OpenSourceCanceled);
	Action const emptyOpen = action_from_open_portal_response(0, {});
	CODEC_VIS_CHECK(emptyOpen.kind == ActionKind::OpenSourceCanceled);
	Action const chosen = action_from_open_portal_response(0, {"file://" + (root / "frog%20image.jpg").string()});
	CODEC_VIS_CHECK(chosen.kind == ActionKind::SourcePathChosen);
	CODEC_VIS_CHECK(chosen.path == root / "frog image.jpg");

	Action const canceledSave = action_from_save_portal_response(ImageId{9}, 2, {});
	CODEC_VIS_CHECK(canceledSave.kind == ActionKind::SaveCanceled);
	CODEC_VIS_CHECK(canceledSave.image == ImageId{9});
	CODEC_VIS_CHECK(canceledSave.text.empty());
	Action const save = action_from_save_portal_response(ImageId{9}, 0, {"file://" + (root / "result.ivf").string()});
	CODEC_VIS_CHECK(save.kind == ActionKind::SaveEncodedResult);
	CODEC_VIS_CHECK(save.image == ImageId{9});
	CODEC_VIS_CHECK(save.path == root / "result.ivf");
	CODEC_VIS_CHECK(save.value == 1.0);
	return 0;
}
