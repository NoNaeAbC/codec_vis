#include "layout.hpp"
#include "app_state.hpp"

#include <algorithm>

namespace codec_gui::gui {

LayoutResult compute_layout(const LayoutState& layout, int framebufferWidth, int framebufferHeight, float outputScale) {
	const float scale = std::max(outputScale, 1.0f);
	const float commandH = layout.commandBarHeight * scale;
	const float statusH = layout.statusBarHeight * scale;
	const float collapsedW = 48.0f * scale;
	const float minViewerW = 480.0f * scale;

	LayoutResult out;
	out.commandBar = {.x=0, .y=0, .w=static_cast<float>(framebufferWidth), .h=commandH};
	out.statusBar = {.x=0, .y=static_cast<float>(framebufferHeight) - statusH, .w=static_cast<float>(framebufferWidth), .h=statusH};

	Rect const content{.x=0, .y=commandH, .w=static_cast<float>(framebufferWidth), .h=std::max(0.0f, static_cast<float>(framebufferHeight) - commandH - statusH)};
	float imageListW = layout.imageListCollapsed ? collapsedW : layout.imageListWidth * scale;
	float inspectorW = layout.inspectorCollapsed ? collapsedW : layout.inspectorWidth * scale;
	out.imageListCollapsed = layout.imageListCollapsed;
	out.inspectorCollapsed = layout.inspectorCollapsed;

	if (content.w - imageListW - inspectorW < minViewerW) {
		inspectorW = collapsedW;
		out.inspectorCollapsed = true;
	}
	if (content.w - imageListW - inspectorW < minViewerW) {
		imageListW = collapsedW;
		out.imageListCollapsed = true;
	}
	if (content.w - imageListW - inspectorW < minViewerW) {
		inspectorW = 0;
		imageListW = 0;
		out.inspectorCollapsed = true;
		out.imageListCollapsed = true;
	}

	out.imageList = {.x=content.x, .y=content.y, .w=imageListW, .h=content.h};
	out.inspector = {.x=content.x + content.w - inspectorW, .y=content.y, .w=inspectorW, .h=content.h};
	out.viewer = {.x=content.x + imageListW, .y=content.y, .w=std::max(0.0f, content.w - imageListW - inspectorW), .h=content.h};
	return out;
}

} // namespace codec_gui::gui
