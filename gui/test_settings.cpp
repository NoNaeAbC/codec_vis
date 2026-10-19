#include "test_check.hpp"
#include "app_state.hpp"
#include "settings.hpp"

#include <cassert>
#include <cstdint>
#include <filesystem>
#include <string>

using namespace codec_gui;
using namespace codec_gui::gui;

int main() {
	const std::filesystem::path root = std::filesystem::temp_directory_path();
	AppState state;
	state.storage.lastImportDirectory = root / "import dir";
	state.storage.lastExportDirectory = root / "export";
	state.layout.imageListWidth = 312.0f;
	state.layout.inspectorWidth = 420.0f;
	state.layout.inspectorCollapsed = true;
	state.viewMode.kind = ViewModeKind::Split;
	state.viewMode.splitPosition = 0.375;
	state.viewMode.blinkIntervalSeconds = 0.25;
	state.viewMode.differenceGain = 4.0;
	state.selection.selectedBackend = BackendId{5};
	state.debug.enabled = true;

	Pane pane;
	pane.id = PaneId{7};
	pane.image = ImageId{3};
	pane.transform.scale = 2.0;
	pane.transform.centerX = 11.0;
	pane.transform.centerY = 12.0;
	pane.linkGroup = 1;
	state.panes.push_back(pane);
	state.viewMode.paneOrder.push_back(pane.id);

	EncoderConfig config;
	config.backend = BackendId{5};
	config.params.push_back({.name="qp", .value=int64_t{31}});
	config.params.push_back({.name="tool", .value=true});
	config.params.push_back({.name="mode", .value=std::string{"a\tb"}});
	state.encoderConfigs.push_back(config);

	const std::string text = serialize_settings(state);
	CODEC_VIS_CHECK(text.find("codec_vis_settings\t1") != std::string::npos);

	AppState loaded = apply_serialized_settings(AppState{}, text);
	CODEC_VIS_CHECK(loaded.storage.lastImportDirectory == state.storage.lastImportDirectory);
	CODEC_VIS_CHECK(loaded.storage.lastExportDirectory == state.storage.lastExportDirectory);
	CODEC_VIS_CHECK(loaded.layout.imageListWidth == state.layout.imageListWidth);
	CODEC_VIS_CHECK(loaded.layout.inspectorCollapsed);
	CODEC_VIS_CHECK(loaded.viewMode.kind == ViewModeKind::Split);
	CODEC_VIS_CHECK(loaded.selection.selectedBackend == BackendId{5});
	CODEC_VIS_CHECK(!loaded.debug.enabled);
	CODEC_VIS_CHECK(loaded.viewMode.paneOrder.size() == 1);
	CODEC_VIS_CHECK(loaded.panes.size() == 1);
	CODEC_VIS_CHECK(loaded.panes.front().image == ImageId{3});
	CODEC_VIS_CHECK(loaded.panes.front().linkGroup == 1u);
	CODEC_VIS_CHECK(loaded.encoderConfigs.size() == 1);
	CODEC_VIS_CHECK(loaded.encoderConfigs.front().backend == BackendId{5});
	CODEC_VIS_CHECK(std::get<int64_t>(loaded.encoderConfigs.front().params[0].value) == 31);
	CODEC_VIS_CHECK(std::get<bool>(loaded.encoderConfigs.front().params[1].value));
	CODEC_VIS_CHECK(std::get<std::string>(loaded.encoderConfigs.front().params[2].value) == "a\tb");

	AppState nonempty;
	Pane existing;
	existing.id = PaneId{7};
	nonempty.panes.push_back(existing);
	nonempty.nextId = 7;
	AppState merged = apply_serialized_settings(nonempty, text);
	CODEC_VIS_CHECK(merged.panes.size() == 2);
	CODEC_VIS_CHECK(merged.panes.front().id == PaneId{7});
	CODEC_VIS_CHECK(merged.panes.back().id != PaneId{7});
	CODEC_VIS_CHECK(merged.viewMode.paneOrder.size() == 1);
	CODEC_VIS_CHECK(merged.viewMode.paneOrder.front() == merged.panes.back().id);

	return 0;
}
