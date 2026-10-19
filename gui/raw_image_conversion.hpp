#pragma once

#include "../codec_gui_x265.hpp"

#include <cstdint>
#include <array>
#include <vector>

namespace codec_gui::gui {

// Metric working space: scene/display-linear BT.2020 D65, with 1 = 203
// cd/m². FP32 storage preserves integer-code roundtrips through 16-bit sRGB;
// narrowing to FP16 loses codes at 12 bits and above. Without gamut clipping,
// tone mapping, or integer intermediates. Negative and >1 values are retained.
struct LinearBt2020Image {
	int width = 0, height = 0;
	std::array<std::vector<float>, 3> planes;
};

[[nodiscard]] LinearBt2020Image raw_image_to_linear_bt2020(const RawImage& image);
// Shared by the FP64 reference conversion and the Vulkan FP32 preview kernel.
[[nodiscard]] std::array<double,9> linear_bt2020_matrix(ColorPrimaries primaries);

enum class ToneMapMode { None, Clip, Reinhard };

struct ColorTransformOptions {
	ColorDescription target;
	ToneMapMode toneMap = ToneMapMode::None;
	double sourcePeakNits = 1000.0;
	double targetPeakNits = 203.0;
};

struct ComparisonImages {
	RawImage reference;
	RawImage candidate;
};

// Normalize representation only; never guess a gamut/transfer conversion or
// tone map. Throws when the images cannot be interpreted on the same basis.
[[nodiscard]] ComparisonImages normalize_comparison(const RawImage& reference, const RawImage& candidate);

[[nodiscard]] std::vector<uint8_t> raw_image_to_rgba8(const RawImage& image);
[[nodiscard]] RawImage convert_raw_image_format(const RawImage& image, PixelFormat targetFormat);
[[nodiscard]] RawImage transform_raw_image(
	const RawImage& image,
	PixelFormat targetFormat,
	const ColorTransformOptions& options
);

} // namespace codec_gui::gui
