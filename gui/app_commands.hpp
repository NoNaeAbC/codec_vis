#pragma once

#include "app_state.hpp"
#include "encoder_backends.hpp"

#include <span>
#include <vector>

namespace codec_gui::gui {

	struct CommandContext {
		std::span<const EncoderBackend> backends;
	};

	[[nodiscard]] std::vector<Action> execute_command(
		const Command& command,
		const AppState& state,
		CommandContext context
	);

} // namespace codec_gui::gui
