#pragma once

#include "app_state.hpp"
#include "layout.hpp"

#include <vector>

namespace codec_gui::gui {

	[[nodiscard]] std::vector<Action> actions_for_pointer_press(
		const AppState& state,
		const LayoutResult& layout,
		Point point
	);

} // namespace codec_gui::gui
