#pragma once
#include "app_state.hpp"
namespace codec_gui::gui {
	struct CommandButton {
		float       x;
		float       w;
		const char *label;
		const char *name;
	};

	inline constexpr CommandButton CommandButtons[] = {
			{.x = 140, .w = 78, .label = "Import", .name = "import"},
			{.x = 224, .w = 78, .label = "Encode", .name = "encode"},
			{.x = 308, .w = 78, .label = "Cancel", .name = "cancel"},
			{.x = 392, .w = 62, .label = "Save", .name = "save"},
			{.x = 462, .w = 78, .label = "Single", .name = "single"},
			{.x = 546, .w = 64, .label = "Side", .name = "side"},
			{.x = 616, .w = 70, .label = "Split", .name = "split"},
			{.x = 692, .w = 70, .label = "Blink", .name = "blink"},
			{.x = 768, .w = 60, .label = "Diff", .name = "diff"},
			{.x = 834, .w = 60, .label = "Grid", .name = "grid"},
			{.x = 900, .w = 52, .label = "Fit", .name = "fit"},
			{.x = 958, .w = 64, .label = "100%", .name = "100"},
			{.x = 1028, .w = 110, .label = "Delete", .name = "delete"},
			{.x = 1144, .w = 144, .label = "Keep results", .name = "scratch"},
			{.x = 1294, .w = 100, .label = "Metrics", .name = "metrics"},
	};

	inline Rect command_button_rect(Rect bar, const CommandButton &button) {
		return {bar.x + button.x, bar.y, button.w, bar.h};
	}
} // namespace codec_gui::gui
