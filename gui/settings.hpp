#pragma once

#include "app_state.hpp"

#include <string>

namespace codec_gui::gui {

	[[nodiscard]] std::string serialize_settings(const AppState& state);
	[[nodiscard]] AppState apply_serialized_settings(AppState state, std::string_view text);

} // namespace codec_gui::gui
