#include "signal_math.hpp"
#include "raw_image_conversion.hpp"
#include "raw_image_utils.hpp"
#include "test_check.hpp"

#include <cstdio>

using namespace codec_gui;
using namespace codec_gui::gui;

namespace {

// Check the actual metric ingress as well as scalar arithmetic. Neutral YUV
// avoids a lossy RGB/YUV matrix roundtrip; BT.2020 avoids a gamut transform.
void check_ingress(int depth, TransferCharacteristics transfer, ColorRange range, bool rgb = false) {
	RawImage image;
	image.width = 1 << depth;
	image.height = 1;
	image.format = rgb ? (depth == 8 ? PixelFormat::RGBP8 : depth == 14 ? PixelFormat::RGBP14LE : PixelFormat::RGBP16LE)
		: pixel_format_for(depth, true);
	image.color = {.primaries=ColorPrimaries::BT2020, .transfer=transfer,
		.matrix=rgb ? MatrixCoefficients::Identity : MatrixCoefficients::BT2020NonConstant,
		.range=range, .chroma420Location=std::nullopt};
	const int bps = bytes_per_sample(image.format);
	const uint32_t maximum = (1u << depth) - 1;
	for (auto& p : image.planes) {
		p.strideBytes = image.width * bps;
		p.bytes.resize(p.strideBytes);
	}
	for (uint32_t q = 0; q <= maximum; ++q) {
		put_sample(image.planes[0], q, 0, bps, q, maximum);
		for (int p = 1; p < 3; ++p)
			put_sample(image.planes[p], q, 0, bps, rgb ? ((q + p) & maximum) : 1u << (depth - 1), maximum);
	}
	if (!rgb && transfer == TransferCharacteristics::Linear && range == ColorRange::Full) {
		const auto widened = convert_raw_image_format(image, PixelFormat::YUV444P16LE);
		const auto restored = convert_raw_image_format(widened, image.format);
		CODEC_VIS_CHECK(restored.planes[0].bytes == image.planes[0].bytes);
	}
	const auto linear = raw_image_to_linear_bt2020(image);
	const double offset = range == ColorRange::Full ? 0 : 16 * (1u << (depth - 8));
	const double scale = range == ColorRange::Full ? maximum : 219 * (1u << (depth - 8));
	for (uint32_t q = 0; q <= maximum; ++q) {
		for (int p = 0; p < 3; ++p) {
			const double value = linear.planes[p][q];
			const double signal = transfer == TransferCharacteristics::SRGB ? srgb_inverse_eotf(value) : value;
			const uint32_t expected = rgb ? ((q + p) & maximum) : q;
			CODEC_VIS_CHECK(quantize_sample(signal * scale + offset, maximum) == expected);
		}
	}
}

void check_depth(int depth) {
	const uint32_t maximum = (1u << depth) - 1;
	uint32_t halfSignalErrors = 0, halfLinearErrors = 0;
	for (uint32_t q = 0; q <= maximum; ++q) {
		const double signal = dequantize_unorm(q, maximum);
		CODEC_VIS_CHECK(quantize_unorm(signal, maximum) == q);
		CODEC_VIS_CHECK(quantize_unorm(static_cast<float>(signal), maximum) == q);
		const double linear = srgb_eotf(signal);
		CODEC_VIS_CHECK(quantize_unorm(srgb_inverse_eotf(linear), maximum) == q);
		CODEC_VIS_CHECK(quantize_unorm(srgb_inverse_eotf(static_cast<float>(linear)), maximum) == q);
		// Also check storing the encoded signal at FP32 before the EOTF.
		CODEC_VIS_CHECK(quantize_unorm(srgb_inverse_eotf(static_cast<float>(srgb_eotf(static_cast<float>(signal)))), maximum) == q);
#if defined(__FLT16_MANT_DIG__)
		halfSignalErrors += quantize_unorm(static_cast<double>(static_cast<_Float16>(signal)), maximum) != q;
		halfLinearErrors += quantize_unorm(srgb_inverse_eotf(static_cast<double>(static_cast<_Float16>(linear))), maximum) != q;
#endif
	}
	std::printf("%2d-bit: exhaustive FP32/FP64 UNORM and sRGB roundtrips passed (%u codes); FP16 failures signal=%u, linear=%u\n",
		depth, maximum + 1, halfSignalErrors, halfLinearErrors);
#if defined(__FLT16_MANT_DIG__)
	if (depth <= 10) CODEC_VIS_CHECK(halfSignalErrors == 0 && halfLinearErrors == 0);
	else CODEC_VIS_CHECK(halfSignalErrors > 0 && halfLinearErrors > 0);
#endif
	for (auto transfer : {TransferCharacteristics::Linear, TransferCharacteristics::SRGB})
		for (auto range : {ColorRange::Full, ColorRange::Limited}) {
			check_ingress(depth, transfer, range);
			if (depth == 8 || depth == 14 || depth == 16) check_ingress(depth, transfer, range, true);
		}
}

} // namespace

int main() {
	for (int depth : {8, 10, 12, 14, 16}) check_depth(depth);
	CODEC_VIS_CHECK(dequantize_unorm(0, 255) == 0);
	CODEC_VIS_CHECK(dequantize_unorm(255, 255) == 1);
	for (int depth : {8, 10, 12, 14, 16}) {
		const uint32_t maximum = (1u << depth) - 1;
		CODEC_VIS_CHECK(quantize_unorm(-1, maximum) == 0);
		CODEC_VIS_CHECK(quantize_unorm(2, maximum) == maximum);
		CODEC_VIS_CHECK(quantize_unorm(std::nextafter(1.0, 0.0), maximum) == maximum);
		CODEC_VIS_CHECK(quantize_unorm(std::nextafter(0.0, 1.0), maximum) == 0);
		// Check decision boundaries in code units, with exactly representable
		// half-integers. Values below a tie round down; ties round up.
		for (uint32_t q = 0; q < maximum; ++q) {
			const double tie = q + 0.5;
			CODEC_VIS_CHECK(quantize_sample(std::nextafter(tie, 0.0), maximum) == q);
			CODEC_VIS_CHECK(quantize_sample(tie, maximum) == q + 1);
			CODEC_VIS_CHECK(quantize_sample(std::nextafter(tie, double(maximum)), maximum) == q + 1);
		}
	}
	// Published sRGB branch values and reference midtone, independent of inverse.
	CODEC_VIS_CHECK(srgb_eotf(0) == 0 && srgb_eotf(1) == 1);
	CODEC_VIS_CHECK(std::abs(srgb_eotf(0.04045) - 0.0031308049535603713) < 1e-17);
	CODEC_VIS_CHECK(std::abs(srgb_eotf(0.5) - 0.21404114048223255) < 1e-15);
	for (double v : {0.0, 0.04045, std::nextafter(0.04045, 1.0), 0.5, 1.0, 2.0, -0.25}) {
		// The standard rounded thresholds have a tiny discontinuity; do not
		// demand floating-point bit equality across that boundary.
		CODEC_VIS_CHECK(std::abs(srgb_inverse_eotf(srgb_eotf(v)) - v) < 3e-8);
	}
	CODEC_VIS_CHECK(srgb_eotf(-0.25) < 0 && srgb_eotf(2) > 1);
	// Demonstrate that floor(x*256) and nearest(x*255) are different
	// quantizers, despite both recovering all 256 endpoint-normalized codes.
	CODEC_VIS_CHECK(quantize_unorm(0.003, 255) == 1);
	CODEC_VIS_CHECK(static_cast<uint32_t>(0.003 * 256) == 0);
	for (uint32_t q = 0; q < 256; ++q) {
		const double x = dequantize_unorm(q, 255);
		CODEC_VIS_CHECK(static_cast<uint32_t>(std::min(x, std::nextafter(1.0, 0.0)) * 256) == q);
	}
}
