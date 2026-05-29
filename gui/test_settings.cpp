#include "settings.hpp"

#include <cassert>
#include <cstdint>
#include <filesystem>
#include <string>
#include <variant>

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
	config.params.push_back({"qp", int64_t{31}});
	config.params.push_back({"tool", true});
	config.params.push_back({"mode", std::string{"a\tb"}});
	state.encoderConfigs.push_back(config);

	const std::string text = serialize_settings(state);
	assert(text.find("codec_vis_settings\t1") != std::string::npos);

	AppState loaded = apply_serialized_settings(AppState{}, text);
	assert(loaded.storage.lastImportDirectory == state.storage.lastImportDirectory);
	assert(loaded.storage.lastExportDirectory == state.storage.lastExportDirectory);
	assert(loaded.layout.imageListWidth == state.layout.imageListWidth);
	assert(loaded.layout.inspectorCollapsed);
	assert(loaded.viewMode.kind == ViewModeKind::Split);
	assert(loaded.selection.selectedBackend == BackendId{5});
	assert(!loaded.debug.enabled);
	assert(loaded.viewMode.paneOrder.size() == 1);
	assert(loaded.panes.size() == 1);
	assert(loaded.panes.front().image == ImageId{3});
	assert(loaded.panes.front().linkGroup == 1u);
	assert(loaded.encoderConfigs.size() == 1);
	assert(loaded.encoderConfigs.front().backend == BackendId{5});
	assert(std::get<int64_t>(loaded.encoderConfigs.front().params[0].value) == 31);
	assert(std::get<bool>(loaded.encoderConfigs.front().params[1].value));
	assert(std::get<std::string>(loaded.encoderConfigs.front().params[2].value) == "a\tb");

	AppState nonempty;
	Pane existing;
	existing.id = PaneId{7};
	nonempty.panes.push_back(existing);
	nonempty.nextId = 7;
	AppState merged = apply_serialized_settings(nonempty, text);
	assert(merged.panes.size() == 2);
	assert(merged.panes.front().id == PaneId{7});
	assert(merged.panes.back().id != PaneId{7});
	assert(merged.viewMode.paneOrder.size() == 1);
	assert(merged.viewMode.paneOrder.front() == merged.panes.back().id);

	return 0;
}
