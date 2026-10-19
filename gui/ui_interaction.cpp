#include "decoder_ui.hpp"
#include "command_controls.hpp"
#include "ui_interaction.hpp"

#include "../codec_gui_x265.hpp"
#include "app_state.hpp"
#include "image_list_model.hpp"
#include "encoder_param_state.hpp"
#include "layout.hpp"
#include "ui_widgets.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

namespace codec_gui::gui {
namespace {

bool contains(Rect rect, Point point) {
	return point.x >= rect.x && point.y >= rect.y && point.x < rect.x + rect.w && point.y < rect.y + rect.h;
}

Rect backend_row_rect(Rect inspector, float y) {
	return Rect{.x=inspector.x + 12.0f, .y=y, .w=inspector.w - 24.0f, .h=22.0f};
}

float backend_selector_end_y(const AppState& state, Rect inspector) {
	return inspector.y + 42.0f + 22.0f + static_cast<float>(state.backends.size()) * 24.0f + 8.0f;
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

const EncodeRun* run_by_id(const AppState& state, EncodeRunId id) {
	const auto it = std::ranges::find_if(state.encodeRuns, [&](const EncodeRun& run) {
		return run.id == id;
	});
	return it == state.encodeRuns.end() ? nullptr : &*it;
}

const ImageObject* image_by_id(const AppState& state, ImageId id) {
	const auto it = std::ranges::find_if(state.images, [&](const ImageObject& image) {
		return image.id == id;
	});
	return it == state.images.end() ? nullptr : &*it;
}

const EncoderConfig* config_for_backend(const AppState& state, BackendId backend) {
	const auto it = std::ranges::find_if(state.encoderConfigs, [&](const EncoderConfig& config) {
		return config.backend == backend;
	});
	return it == state.encoderConfigs.end() ? nullptr : &*it;
}

ParamValue current_value(const AppState& state, BackendId backend, const EncoderParamInfo& param) {
	if (const EncoderConfig* config = config_for_backend(state, backend)) {
		const auto it = std::ranges::find_if(config->params, [&](const EncoderParam& existing) {
			return existing.name == param.name;
		});
		if (it != config->params.end()) {
			return it->value;
		}
	}
	return std::visit(
		[](const auto& value) -> ParamValue {
			using T = std::decay_t<decltype(value)>;
			if constexpr (std::is_same_v<T, std::monostate>) {
				return std::string{};
			} else {
				return value;
			}
		},
		param.defaultValue
	);
}

Action set_param_action(BackendId backend, const EncoderParamInfo& param, ParamValue value) {
	Action action;
	action.kind = ActionKind::SetEncoderParam;
	action.backend = backend;
	action.param.name = param.name;
	action.param.value = std::move(value);
	return action;
}

double control_ratio(Rect control, Point point) {
	if (control.w <= 0.0f) {
		return 0.0;
	}
	return std::clamp(static_cast<double>((point.x - control.x) / control.w), 0.0, 1.0);
}

int64_t slider_int_value(const EncoderParamInfo& param, Rect control, Point point) {
	const IntRange range = param.intRange.value_or(IntRange{.min=0, .max=100, .step=1});
	const int64_t step = range.step <= 0 ? 1 : range.step;
	const double raw = static_cast<double>(range.min) + control_ratio(control, point) * static_cast<double>(range.max - range.min);
	const auto steps = static_cast<int64_t>(std::llround((raw - static_cast<double>(range.min)) / static_cast<double>(step)));
	const int64_t snapped = range.min + steps * step;
	return std::clamp(snapped, range.min, range.max);
}

double slider_float_value(const EncoderParamInfo& param, Rect control, Point point) {
	const FloatRange range = param.floatRange.value_or(FloatRange{.min=0.0, .max=1.0, .step=0.01});
	const double step = range.step <= 0.0 ? 0.01 : range.step;
	const double raw = range.min + control_ratio(control, point) * (range.max - range.min);
	const double snapped = range.min + std::round((raw - range.min) / step) * step;
	return std::clamp(snapped, range.min, range.max);
}

std::vector<EncoderParam> config_params_or_defaults(const AppState& state, const BackendInfo& backend) {
	std::vector<EncoderParam> params;
	for (const EncoderParamInfo& info : backend.params) {
		if (!info.relevantForStillImage || info.name == "implementation" || !parameter_is_enabled(state, backend, info)) {
			continue;
		}
		EncoderParam param;
		param.name = info.name;
		param.value = current_value(state, backend.id, info);
		if (const std::string* text = std::get_if<std::string>(&param.value); text != nullptr && text->empty()) {
			continue;
		}
		params.push_back(std::move(param));
	}
	return params;
}

float selected_run_details_end_y(const AppState& state, Rect inspector, float y) {
	const EncodeRun* run = run_by_id(state, state.selection.selectedRun);
	if (run == nullptr || y + 120.0f >= inspector.y + inspector.h) {
		return y;
	}
	y += 22.0f;
	y += 20.0f;
	y += 20.0f;
	if (!run->params.empty() && y + 18.0f < inspector.y + inspector.h) {
		y += 20.0f;
	}
	if (!run->error.empty() && y + 18.0f < inspector.y + inspector.h) {
		y += 20.0f;
	}
	if (run->finishedSeconds > run->startedSeconds && y + 18.0f < inspector.y + inspector.h) {
		y += 20.0f;
	}
	if (run->producedImage) {
		const ImageObject* image = image_by_id(state, *run->producedImage);
		if (image != nullptr && image->encoded && y + 18.0f < inspector.y + inspector.h) {
			y += 20.0f;
			if (image->encoded->encodeSeconds > 0.0 && y + 18.0f < inspector.y + inspector.h) {
				y += 20.0f;
			}
			if (!image->encoded->outputPath.empty() && y + 18.0f < inspector.y + inspector.h) {
				y += 20.0f;
			}
			if (!image->encoded->metricError.empty() && y + 18.0f < inspector.y + inspector.h) {
				y += 20.0f;
			}
			if (!image->encoded->previewError.empty() && y + 18.0f < inspector.y + inspector.h) {
				y += 20.0f;
			}
		}
	}
	return y + 10.0f;
}

std::vector<Action> queue_panel_action(const AppState& state, Rect inspector, float y, Point point) {
	if (state.encodeRuns.empty() || y + 62.0f > inspector.y + inspector.h) {
		return {};
	}
	y += 22.0f;
	std::size_t shown = 0;
	for (auto it = state.encodeRuns.rbegin(); it != state.encodeRuns.rend() && shown < 4; ++it) {
		const EncodeRun& run = *it;
		if (y + 36.0f > inspector.y + inspector.h) {
			break;
		}
		const Rect row{.x=inspector.x + 12.0f, .y=y, .w=inspector.w - 24.0f, .h=36.0f};
		if (contains(row, point)) {
			Action action;
			action.kind = ActionKind::SelectEncodeRun;
			action.run = run.id;
			return {action};
		}
		y += 40.0f;
		++shown;
	}
	return {};
}

Action key_action(std::string text) {
	Action action;
	action.kind = ActionKind::KeyPressed;
	action.text = std::move(text);
	return action;
}

std::vector<Action> command_bar_action(const AppState& state, const LayoutResult& layout, Point point) {
	for (const CommandButton& button : CommandButtons) {
		Rect const rect=command_button_rect(layout.commandBar,button);
		if (!contains(rect, point)) {
			continue;
		}
		if (!command_is_enabled(state, button.name)) {
			return {};
		}
		if (std::string(button.name) == "metrics") {
			Action action;action.kind=ActionKind::RequestMetrics;action.image=state.selection.selectedImage;return {action};
		}
		if (std::string(button.name) == "import") {
			Action action;
			action.kind = ActionKind::RequestOpenSource;
			return {action};
		}
		if (std::string(button.name) == "encode") {
			const BackendInfo* backend = selected_backend(state);
			const ImageId source = selected_encode_source(state);
			if (backend == nullptr || !valid(source)) {
				return {};
			}
			Action action;
			action.kind = ActionKind::StartEncodeRun;
			action.image = source;
			action.backend = backend->id;
			action.params = config_params_or_defaults(state, *backend);
			return {std::move(action)};
		}
		if (std::string(button.name) == "cancel") {
			const EncodeRunId run = state.selection.selectedRun;
			if (!valid(run)) {
				return {};
			}
			Action action;
			action.kind = ActionKind::CancelEncodeRun;
			action.run = run;
			return {action};
		}
		if (std::string(button.name) == "save") {
			const ImageId image = state.selection.selectedImage;
			const ImageObject* selected = find_image(state, image);
			if (selected == nullptr || !selected->encoded) {
				return {};
			}
			Action action;
			action.kind = ActionKind::SaveEncodedResult;
			action.image = image;
			return {std::move(action)};
		}
		if (std::string(button.name) == "delete") {
			if (!valid(state.selection.selectedImage)) return {};
			Action action;
			action.kind = ActionKind::RemoveImage;
			action.image = state.selection.selectedImage;
			return {action};
		}
		if (std::string(button.name) == "scratch") {
			Action action;
			action.kind = ActionKind::ToggleScratchResults;
			return {action};
		}
		if (std::string(button.name) == "single") {
			Action action;
			action.kind = ActionKind::SetViewMode;
			action.viewMode = ViewModeKind::Single;
			return {action};
		}
		if (std::string(button.name) == "side") {
			Action action;
			action.kind = ActionKind::SetViewMode;
			action.viewMode = ViewModeKind::SideBySide;
			return {action};
		}
		if (std::string(button.name) == "split") {
			Action action;
			action.kind = ActionKind::SetViewMode;
			action.viewMode = ViewModeKind::Split;
			return {action};
		}
		if (std::string(button.name) == "blink") {
			Action action;
			action.kind = ActionKind::SetViewMode;
			action.viewMode = ViewModeKind::Blink;
			return {action};
		}
		if (std::string(button.name) == "diff") {
			Action action;
			action.kind = ActionKind::SetViewMode;
			action.viewMode = ViewModeKind::Difference;
			return {action};
		}
		if (std::string(button.name) == "grid") {
			Action action;
			action.kind = ActionKind::SetViewMode;
			action.viewMode = ViewModeKind::Grid;
			return {action};
		}
		if (std::string(button.name) == "fit") {
			return {key_action("fit")};
		}
		if (std::string(button.name) == "100") {
			return {key_action("100%")};
		}
	}
	return {};
}

std::vector<Action> image_list_action(const AppState& state, const LayoutResult& layout, Point point) {
	if (!contains(layout.imageList, point)) {
		return {};
	}
	Rect const header{.x=layout.imageList.x, .y=layout.imageList.y, .w=layout.imageList.w, .h=ImageListHeaderHeight};
	if (contains(header, point)) {
		Action action;
		action.kind = ActionKind::SetImageListSort;
		action.sortKey = next_sort_key(state.imageList.sortKey);
		return {action};
	}
	float y = layout.imageList.y + ImageListHeaderHeight + 4.0f - state.imageList.scrollOffset;
	for (const ImageObject* imagePtr : ordered_images(state)) {
		const ImageObject& image = *imagePtr;
		Rect const row = image_list_row_rect(layout.imageList, y);
		if (contains(row, point)) {
			const std::size_t paneButtons = std::min<std::size_t>(state.panes.size(), 4);
			if (valid(state.selection.selectedImage) && state.selection.selectedImage != image.id &&
			    contains(image_list_difference_button_rect(row, paneButtons), point)) {
				Action action;
				action.kind = ActionKind::CreateDifferenceImage;
				action.image = state.selection.selectedImage;
				action.otherImage = image.id;
				return {action};
			}
			for (std::size_t i = 0; i < paneButtons; ++i) {
				if (contains(image_list_pane_button_rect(row, i), point)) {
					Action select;
					select.kind = ActionKind::SelectImage;
					select.image = image.id;
					Action assign;
					assign.kind = ActionKind::AssignImageToPane;
					assign.pane = state.panes[i].id;
					assign.image = image.id;
					return {select, assign};
				}
			}
			std::vector<Action> actions;
			Action select;
			select.kind = ActionKind::SelectImage;
			select.image = image.id;
			actions.push_back(select);
			if (valid(state.selection.activePane)) {
				Action assign;
				assign.kind = ActionKind::AssignImageToPane;
				assign.pane = state.selection.activePane;
				assign.image = image.id;
				actions.push_back(assign);
			}
			return actions;
		}
		y += ImageListRowStep;
	}
	return {};
}

std::vector<Action> inspector_action(const AppState& state, const LayoutResult& layout, Point point) {
	if (layout.inspector.w<160 || !contains(layout.inspector, point)) {
		return {};
	}
    for(const auto& control:decoder_controls(state,layout.inspector)) {
        if((control.id.starts_with("inspector:") || control.rect.y>=layout.inspector.y+40) && contains(control.rect,point))return control.enabled?std::vector<Action>{control.action}:std::vector<Action>{};
    }
    if(state.decoderInspector || point.y<layout.inspector.y+40)return {};
	float y = layout.inspector.y + 42.0f + 22.0f - state.layout.inspectorScrollOffset;
	for (const BackendInfo& candidate : state.backends) {
		if (contains(backend_row_rect(layout.inspector, y), point)) {
			Action action;
			action.kind = ActionKind::SelectBackend;
			action.backend = candidate.id;
			return {action};
		}
		y += 24.0f;
	}
	const BackendInfo* backend = selected_backend(state);
	if (backend == nullptr || !backend->capabilities.available) {
		return {};
	}
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
		if (contains(control, point)) {
			if (param.name == "implementation" || !parameter_is_enabled(state, *backend, param)) {
				return {};
			}
			const ParamValue value = current_value(state, backend->id, param);
			if (param.kind == ParamKind::Bool) {
				bool current = false;
				if (const bool* boolValue = std::get_if<bool>(&value)) {
					current = *boolValue;
				}
				return {set_param_action(backend->id, param, !current)};
			}
			if (param.kind == ParamKind::Enum && !param.enumValues.empty()) {
				const std::size_t index = std::min<std::size_t>(
					param.enumValues.size() - 1,
					static_cast<std::size_t>(control_ratio(control, point) * static_cast<double>(param.enumValues.size()))
				);
				return {set_param_action(backend->id, param, param.enumValues[index].value)};
			}
			if (param.kind == ParamKind::Int) {
				if (param.intRange && !param.directNumericInput) {
					return {set_param_action(backend->id, param, slider_int_value(param, control, point))};
				}
				int64_t current = 0;
				if (const int64_t* intValue = std::get_if<int64_t>(&value)) {
					current = *intValue;
				}
				return {set_param_action(backend->id, param, current)};
			}
			if (param.kind == ParamKind::Float) {
				if (param.floatRange && !param.directNumericInput) {
					return {set_param_action(backend->id, param, slider_float_value(param, control, point))};
				}
				double current = 0.0;
				if (const double* doubleValue = std::get_if<double>(&value)) {
					current = *doubleValue;
				}
				return {set_param_action(backend->id, param, current)};
			}
			if (param.kind == ParamKind::String) {
				std::string current;
				if (const std::string* stringValue = std::get_if<std::string>(&value)) {
					current = *stringValue;
				}
				return {set_param_action(backend->id, param, current)};
			}
		}
		y += 24;
	}
	std::vector<Action> queueActions = queue_panel_action(state, layout.inspector, selected_run_details_end_y(state, layout.inspector, y), point);
	if (!queueActions.empty()) {
		return queueActions;
	}
	return {};
}

} // namespace

std::vector<Action> actions_for_pointer_press(const AppState& state, const LayoutResult& layout, Point point) {
	if (!state.errors.empty() && contains(layout.statusBar, point)) {
		Action dismiss;
		dismiss.kind = ActionKind::ErrorDismissed;
		dismiss.value = static_cast<double>(state.errors.front().id);
		return {dismiss};
	}
	if (contains(layout.commandBar, point)) {
		return command_bar_action(state, layout, point);
	}
	if (contains(layout.imageList, point)) {
		return image_list_action(state, layout, point);
	}
	if (contains(layout.inspector, point)) {
		return inspector_action(state, layout, point);
	}
	if (state.images.empty() && contains(layout.viewer, point)) {
		constexpr float width = 150.0f;
		constexpr float height = 34.0f;
		const Rect importRect{
			.x=layout.viewer.x + (layout.viewer.w - width) * 0.5f,
			.y=layout.viewer.y + layout.viewer.h * 0.5f + 28.0f,
			.w=width,
			.h=height,
		};
		if (contains(importRect, point) && command_is_enabled(state, "import")) {
			Action action;
			action.kind = ActionKind::RequestOpenSource;
			return {action};
		}
	}
	return {};
}

std::vector<Action> actions_for_pointer_move(const AppState& state, const LayoutResult& layout, Point point) {
    if(state.decoderInspector)return {};
	const BackendInfo* backend = selected_backend(state);
	if (backend == nullptr || state.interaction.activePointerCapture.empty()) return {};
	float y = backend_selector_end_y(state, layout.inspector) + 22.0f - state.layout.inspectorScrollOffset;
	std::string currentGroup;
	for (const EncoderParamInfo& param : backend->params) {
		if (!param.relevantForStillImage) continue;
		if (param.group != currentGroup) {
			currentGroup = param.group;
			y += 24.0f;
		}
		const std::string widgetId = "param:" + std::to_string(backend->id.value) + ":" + param.name;
		if (widgetId == state.interaction.activePointerCapture && parameter_is_enabled(state, *backend, param)) {
			const Rect control{.x=layout.inspector.x + layout.inspector.w * 0.50f, .y=y - 2.0f, .w=layout.inspector.w * 0.44f, .h=20.0f};
			if (param.kind == ParamKind::Int && param.intRange && !param.directNumericInput) return {set_param_action(backend->id, param, slider_int_value(param, control, point))};
			if (param.kind == ParamKind::Float && param.floatRange && !param.directNumericInput) return {set_param_action(backend->id, param, slider_float_value(param, control, point))};
			return {};
		}
		y += 24.0f;
	}
	return {};
}

} // namespace codec_gui::gui
