#pragma once

#include "app_state.hpp"
#include "layout.hpp"

#include <string>
#include <vector>

namespace codec_gui::gui {

	enum class WidgetKind {
		Button,
		Toggle,
		SliderInt,
		SliderFloat,
		NumericInput,
		Dropdown,
		TextInput,
		TableList,
		Splitter,
		PaneView,
		ProgressRow,
		InlineError,
	};

	struct WidgetInfo {
		std::string id;
		WidgetKind kind = WidgetKind::Button;
		Rect rect;
		bool enabled = true;
		bool hovered = false;
		bool active = false;
		bool focused = false;
	};

	[[nodiscard]] std::vector<WidgetInfo> collect_widgets(const AppState& state, const LayoutResult& layout);

} // namespace codec_gui::gui
