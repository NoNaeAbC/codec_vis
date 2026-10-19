#pragma once

#include "codec_gui_x265.hpp"

#include <filesystem>
#include <vector>
#include <span>
#include <string_view>

namespace codec_gui {

	std::vector<unsigned char> read_file_bytes(const std::filesystem::path& path);
	void dump_to_file(const std::filesystem::path& path, const std::vector<std::byte>& data);
	RawImage load_input_image(const std::filesystem::path& path);
	// The file loader and tests share this exact decoder/format dispatch path.
	RawImage decode_input_image(std::span<const unsigned char> bytes, std::string_view filenameHint = {},
	                           std::optional<double>* codecSeconds = nullptr);
	inline std::vector<std::string> supported_input_extensions() {
		return {".heif",".heic",".hif",".avif",".vvic",".png",".jpg",".jpeg",".jxl",".nef",".webp",".jls",".jp2",".j2k",".j2c",".jpc",
			".jxr",".wdp",".hdp",".jxs",".ivf",".h264",".264",".avc",".h265",".265",".hevc",".h266",".266",".vvc"};
	}
	RawImage make_test_pattern();

} // namespace codec_gui
