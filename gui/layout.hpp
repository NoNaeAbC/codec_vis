#pragma once

#include "app_state.hpp"

namespace codec_gui::gui {

	struct LayoutResult {
		Rect commandBar;
		Rect statusBar;
		Rect imageList;
		Rect inspector;
		Rect viewer;
		bool imageListCollapsed = false;
		bool inspectorCollapsed = false;
	};

	[[nodiscard]] LayoutResult compute_layout(
		const LayoutState& layout,
		int framebufferWidth,
		int framebufferHeight,
		float outputScale
	);

} // namespace codec_gui::gui
