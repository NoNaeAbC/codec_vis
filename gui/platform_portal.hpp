#pragma once

#include "app_state.hpp"

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace codec_gui::gui {

	[[nodiscard]] std::filesystem::path decode_portal_file_uri(std::string_view uri);
	[[nodiscard]] Action action_from_open_portal_response(uint32_t response, const std::vector<std::string>& uris);
	[[nodiscard]] Action action_from_save_portal_response(ImageId image, uint32_t response, const std::vector<std::string>& uris);
	[[nodiscard]] Action show_open_file_portal();
	[[nodiscard]] Action show_save_file_portal(ImageId image, const std::filesystem::path& suggestedPath);

} // namespace codec_gui::gui
