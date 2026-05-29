#include "ui_widgets.hpp"

#include "image_list_model.hpp"
#include "viewer_model.hpp"

#include <algorithm>
#include <string>

namespace codec_gui::gui {
namespace {

bool contains(Rect rect, Point point) {
	return point.x >= rect.x && point.y >= rect.y && point.x < rect.x + rect.w && point.y < rect.y + rect.h;
}

const BackendInfo* selected_backend(const AppState& state) {
	if (valid(state.selection.selectedBackend)) {
		const auto it = std::find_if(state.backends.begin(), state.backends.end(), [&](const BackendInfo& backend) {
			return backend.id == state.selection.selectedBackend;
		});
		if (it != state.backends.end()) {
			return &*it;
		}
	}
	return state.backends.empty() ? nullptr : &state.backends.front();
}

Rect backend_row_rect(Rect inspector, float y) {
	return Rect{inspector.x + 12.0f, y, inspector.w - 24.0f, 22.0f};
}

float backend_selector_end_y(const AppState& state, Rect inspector) {
	return inspector.y + 42.0f + 22.0f + static_cast<float>(state.backends.size()) * 24.0f + 8.0f;
}

WidgetKind widget_kind_for_param(const EncoderParamInfo& param) {
	switch (param.kind) {
		case ParamKind::Bool:
			return WidgetKind::Toggle;
		case ParamKind::Int:
			return param.intRange ? WidgetKind::SliderInt : WidgetKind::NumericInput;
		case ParamKind::Float:
			return param.floatRange ? WidgetKind::SliderFloat : WidgetKind::NumericInput;
		case ParamKind::Enum:
			return WidgetKind::Dropdown;
		case ParamKind::String:
			return WidgetKind::TextInput;
	}
	return WidgetKind::Button;
}

void push_widget(std::vector<WidgetInfo>& out, const AppState& state, std::string id, WidgetKind kind, Rect rect, bool enabled = true) {
	WidgetInfo widget;
	widget.id = std::move(id);
	widget.kind = kind;
	widget.rect = rect;
	widget.enabled = enabled;
	widget.hovered = contains(rect, state.interaction.lastPointer);
	widget.active = state.interaction.activePointerCapture == widget.id;
	widget.focused = state.interaction.focusedWidget == widget.id;
	out.push_back(std::move(widget));
}

} // namespace

std::vector<WidgetInfo> collect_widgets(const AppState& state, const LayoutResult& layout) {
	std::vector<WidgetInfo> out;

	struct Button {
		float x;
		float w;
		const char* name;
	};
	const Button buttons[] = {
		{140, 78, "import"},
		{224, 78, "encode"},
		{308, 78, "cancel"},
		{392, 62, "save"},
		{462, 78, "single"},
		{546, 64, "side"},
		{616, 70, "split"},
		{692, 70, "blink"},
		{768, 60, "diff"},
		{834, 60, "grid"},
		{900, 52, "fit"},
		{958, 64, "100"},
	};
	for (const Button& button : buttons) {
		push_widget(out, state, std::string{"command:"} + button.name, WidgetKind::Button, Rect{layout.commandBar.x + button.x, layout.commandBar.y, button.w, layout.commandBar.h});
	}

	push_widget(out, state, "image-list:sort", WidgetKind::TableList, Rect{layout.imageList.x, layout.imageList.y, layout.imageList.w, 40}, layout.imageList.w > 0.0f);
	float y = layout.imageList.y + 42;
	for (const ImageObject* image : ordered_images(state)) {
		if (y + 54 > layout.imageList.y + layout.imageList.h) {
			break;
		}
		push_widget(out, state, "image-row:" + std::to_string(image->id.value), WidgetKind::TableList, Rect{layout.imageList.x + 6, y - 2, layout.imageList.w - 12, 50}, layout.imageList.w > 0.0f);
		y += 56;
	}

	const std::vector<PaneRect> paneRects = compute_pane_rects(state.viewMode, state.panes, layout.viewer);
	for (const PaneRect& pane : paneRects) {
		push_widget(out, state, "pane:" + std::to_string(pane.pane.value), WidgetKind::PaneView, pane.rect, true);
	}
	if (state.viewMode.kind == ViewModeKind::Split) {
		const float split = layout.viewer.x + layout.viewer.w * static_cast<float>(std::clamp(state.viewMode.splitPosition, 0.0, 1.0));
		push_widget(out, state, "splitter:viewer", WidgetKind::Splitter, Rect{split - 6.0f, layout.viewer.y, 12.0f, layout.viewer.h}, true);
	}

	const BackendInfo* backend = selected_backend(state);
	y = layout.inspector.y + 42.0f + 22.0f;
	for (const BackendInfo& candidate : state.backends) {
		push_widget(out, state, "backend:" + std::to_string(candidate.id.value), WidgetKind::Button, backend_row_rect(layout.inspector, y), true);
		y += 24.0f;
	}
	if (backend != nullptr) {
		y = backend_selector_end_y(state, layout.inspector) + 22.0f;
		std::string currentGroup;
		int renderedParams = 0;
		for (const EncoderParamInfo& param : backend->params) {
			if (!param.relevantForStillImage || y + 44 > layout.inspector.y + layout.inspector.h - 120) {
				continue;
			}
			if (param.group != currentGroup) {
				currentGroup = param.group;
				y += 24;
			}
			Rect control{layout.inspector.x + layout.inspector.w * 0.50f, y - 2, layout.inspector.w * 0.44f, 20};
			push_widget(out, state, "param:" + std::to_string(backend->id.value) + ":" + param.name, widget_kind_for_param(param), control, backend->capabilities.available && param.name != "implementation");
			y += 24;
			++renderedParams;
			if (renderedParams >= 16) {
				break;
			}
		}
	}

	for (const AppError& error : state.errors) {
		push_widget(out, state, "error:" + std::to_string(error.id), WidgetKind::InlineError, layout.statusBar, true);
		break;
	}

	return out;
}

} // namespace codec_gui::gui
