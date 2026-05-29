#include "layout.hpp"

#include <algorithm>

namespace codec_gui::gui {

LayoutResult compute_layout(const LayoutState& layout, int framebufferWidth, int framebufferHeight, float outputScale) {
	const float scale = std::max(outputScale, 1.0f);
	const float commandH = layout.commandBarHeight * scale;
	const float statusH = layout.statusBarHeight * scale;
	const float collapsedW = 48.0f * scale;
	const float minViewerW = 480.0f * scale;

	LayoutResult out;
	out.commandBar = {0, 0, static_cast<float>(framebufferWidth), commandH};
	out.statusBar = {0, static_cast<float>(framebufferHeight) - statusH, static_cast<float>(framebufferWidth), statusH};

	Rect content{0, commandH, static_cast<float>(framebufferWidth), std::max(0.0f, static_cast<float>(framebufferHeight) - commandH - statusH)};
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

	out.imageList = {content.x, content.y, imageListW, content.h};
	out.inspector = {content.x + content.w - inspectorW, content.y, inspectorW, content.h};
	out.viewer = {content.x + imageListW, content.y, std::max(0.0f, content.w - imageListW - inspectorW), content.h};
	return out;
}

} // namespace codec_gui::gui
