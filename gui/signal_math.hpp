#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace codec_gui::gui {

// Endpoint-normalized integer samples: black = 0, white = maximum.
// This pair uses nearest rounding, not the floor(x * (maximum + 1))
// bin quantizer. Mixing those conventions changes decision boundaries.
[[nodiscard]] inline double dequantize_unorm(uint32_t code, uint32_t maximum) {
	return static_cast<double>(code) / maximum;
}

[[nodiscard]] inline uint32_t quantize_sample(double value, uint32_t maximum) {
	// round() also handles the representable value immediately below a tie;
	// floor(value + 0.5) can round that addition up to the tie first.
	return static_cast<uint32_t>(std::round(std::clamp(value, 0.0, static_cast<double>(maximum))));
}

[[nodiscard]] inline uint32_t quantize_unorm(double value, uint32_t maximum) {
	return quantize_sample(std::clamp(value, 0.0, 1.0) * maximum, maximum);
}

// Extended, sign-preserving sRGB transfer functions. Neither function clips
// negative or HDR values; callers that write bounded formats clamp separately.
[[nodiscard]] inline double srgb_eotf(double signal) {
	const double a = std::abs(signal);
	return std::copysign(a <= 0.04045 ? a / 12.92 : std::pow((a + 0.055) / 1.055, 2.4), signal);
}

[[nodiscard]] inline double srgb_inverse_eotf(double linear) {
	const double a = std::abs(linear);
	return std::copysign(a <= 0.0031308 ? a * 12.92 : 1.055 * std::pow(a, 1.0 / 2.4) - 0.055, linear);
}

} // namespace codec_gui::gui
