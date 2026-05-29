#pragma once

#include "../codec_gui_x265.hpp"

#include <cstdint>
#include <vector>

namespace codec_gui::gui {

[[nodiscard]] std::vector<uint8_t> raw_image_to_rgba8(const RawImage& image);
[[nodiscard]] RawImage convert_raw_image_format(const RawImage& image, PixelFormat targetFormat);

} // namespace codec_gui::gui
