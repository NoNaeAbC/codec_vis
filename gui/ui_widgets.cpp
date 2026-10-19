#include "decoder_ui.hpp"
#include "command_controls.hpp"
#include "ui_widgets.hpp"

#include "../codec_gui_x265.hpp"
#include "app_state.hpp"
#include "image_list_model.hpp"
#include "encoder_param_state.hpp"
#include "layout.hpp"
#include "viewer_model.hpp"

#include <algorithm>
#include <string>
#include <utility>
#include <vector>

namespace codec_gui::gui {
namespace {

bool contains(Rect rect, Point point) {
	return point.x >= rect.x && point.y >= rect.y && point.x < rect.x + rect.w && point.y < rect.y + rect.h;
}

bool intersects(Rect a, Rect b) {
	return a.x < b.x + b.w && a.x + a.w > b.x && a.y < b.y + b.h && a.y + a.h > b.y;
}

const BackendInfo* selected_backend(const AppState& state) {
	if (valid(state.selection.selectedBackend)) {
		const auto it = std::ranges::find_if(state.backends, [&](const BackendInfo& backend) {
			return backend.id == state.selection.selectedBackend;
		});
		if (it != state.backends.end()) {
			return &*it;
		}
	}
	return state.backends.empty() ? nullptr : &state.backends.front();
}

Rect backend_row_rect(Rect inspector, float y) {
	return Rect{.x=inspector.x + 12.0f, .y=y, .w=inspector.w - 24.0f, .h=22.0f};
}

float backend_selector_end_y(const AppState& state, Rect inspector) {
	return inspector.y + 42.0f + 22.0f + static_cast<float>(state.backends.size()) * 24.0f + 8.0f;
}

WidgetKind widget_kind_for_param(const EncoderParamInfo& param) {
	switch (param.kind) {
		case ParamKind::Bool:
			return WidgetKind::Toggle;
		case ParamKind::Int:
			return param.intRange && !param.directNumericInput ? WidgetKind::SliderInt : WidgetKind::NumericInput;
		case ParamKind::Float:
			return param.floatRange && !param.directNumericInput ? WidgetKind::SliderFloat : WidgetKind::NumericInput;
		case ParamKind::Enum:
			return WidgetKind::SelectionPane;
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

	for (const CommandButton& button : CommandButtons) {
		push_widget(
			out,
			state,
			std::string{"command:"} + button.name,
			WidgetKind::Button,
			command_button_rect(layout.commandBar,button),
			command_is_enabled(state, button.name)
		);
	}

	push_widget(out, state, "image-list:sort", WidgetKind::TableList, Rect{.x=layout.imageList.x, .y=layout.imageList.y, .w=layout.imageList.w, .h=ImageListHeaderHeight}, layout.imageList.w > 0.0f);
	float y = layout.imageList.y + ImageListHeaderHeight + 4.0f - state.imageList.scrollOffset;
	const Rect imageListContent{.x=layout.imageList.x, .y=layout.imageList.y + ImageListHeaderHeight, .w=layout.imageList.w, .h=std::max(0.0f, layout.imageList.h - ImageListHeaderHeight)};
	for (const ImageObject* image : ordered_images(state)) {
		const Rect row = image_list_row_rect(layout.imageList, y);
		if (intersects(row, imageListContent)) {
			push_widget(out, state, "image-row:" + std::to_string(image->id.value), WidgetKind::TableList, row, layout.imageList.w > 0.0f);
		}
		y += ImageListRowStep;
	}

	const std::vector<PaneRect> paneRects = compute_pane_rects(state.viewMode, state.panes, layout.viewer);
	for (const PaneRect& pane : paneRects) {
		push_widget(out, state, "pane:" + std::to_string(pane.pane.value), WidgetKind::PaneView, pane.rect, true);
	}
	if (state.viewMode.kind == ViewModeKind::Split) {
		const float split = layout.viewer.x + layout.viewer.w * static_cast<float>(std::clamp(state.viewMode.splitPosition, 0.0, 1.0));
		push_widget(out, state, "splitter:viewer", WidgetKind::Splitter, Rect{.x=split - 6.0f, .y=layout.viewer.y, .w=12.0f, .h=layout.viewer.h}, true);
	}

    for(const auto& control:decoder_controls(state,layout.inspector)) {
        const Rect clip{layout.inspector.x,layout.inspector.y+40,layout.inspector.w,layout.inspector.h-40};
        if(control.id.starts_with("inspector:") || (control.rect.y>=clip.y && intersects(control.rect,clip)))push_widget(out,state,control.id,WidgetKind::Button,control.rect,control.enabled);
    }
    if(!state.decoderInspector && layout.inspector.w>=160) {
	const BackendInfo* backend = selected_backend(state);
	const Rect inspectorContent{.x=layout.inspector.x, .y=layout.inspector.y + 40.0f, .w=layout.inspector.w, .h=std::max(0.0f, layout.inspector.h - 40.0f)};
	y = layout.inspector.y + 42.0f + 22.0f - state.layout.inspectorScrollOffset;
	for (const BackendInfo& candidate : state.backends) {
		const Rect row = backend_row_rect(layout.inspector, y);
		if (intersects(row, inspectorContent)) {
			push_widget(out, state, "backend:" + std::to_string(candidate.id.value), WidgetKind::Button, row, true);
		}
		y += 24.0f;
	}
	if (backend != nullptr) {
		y = backend_selector_end_y(state, layout.inspector) + 22.0f;
		y -= state.layout.inspectorScrollOffset;
		std::string currentGroup;
		for (const EncoderParamInfo& param : backend->params) {
			if (!param.relevantForStillImage) {
				continue;
			}
			if (param.group != currentGroup) {
				currentGroup = param.group;
				y += 24;
			}
			Rect const control{.x=layout.inspector.x + layout.inspector.w * 0.50f, .y=y - 2, .w=layout.inspector.w * 0.44f, .h=20};
			if (intersects(control, inspectorContent)) {
				push_widget(out, state, "param:" + std::to_string(backend->id.value) + ":" + param.name, widget_kind_for_param(param), control, backend->capabilities.available && param.name != "implementation" && parameter_is_enabled(state, *backend, param));
			}
			y += 24;
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
