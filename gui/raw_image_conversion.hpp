#pragma once

#include "../codec_gui_x265.hpp"

#include <cstdint>
#include <vector>

namespace codec_gui::gui {

enum class ToneMapMode { None, Clip, Reinhard };

struct ColorTransformOptions {
	ColorDescription target;
	ToneMapMode toneMap = ToneMapMode::None;
	double sourcePeakNits = 1000.0;
	double targetPeakNits = 203.0;
};

[[nodiscard]] std::vector<uint8_t> raw_image_to_rgba8(const RawImage& image);
[[nodiscard]] RawImage convert_raw_image_format(const RawImage& image, PixelFormat targetFormat);
[[nodiscard]] RawImage transform_raw_image(
	const RawImage& image,
	PixelFormat targetFormat,
	const ColorTransformOptions& options
);

} // namespace codec_gui::gui
