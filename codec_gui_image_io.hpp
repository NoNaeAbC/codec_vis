#pragma once

#include "codec_gui_x265.hpp"

#include <filesystem>
#include <vector>

namespace codec_gui {

	std::vector<unsigned char> read_file_bytes(const std::filesystem::path& path);
	void dump_to_file(const std::filesystem::path& path, const std::vector<std::byte>& data);
	RawImage load_input_image(const std::filesystem::path& path);
	RawImage make_test_pattern();

} // namespace codec_gui
