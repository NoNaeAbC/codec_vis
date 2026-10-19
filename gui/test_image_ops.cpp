#include "../codec_gui_x265.hpp"
#include "image_ops.hpp"
#include "raw_image_conversion.hpp"
#include "test_check.hpp"

#include <cassert>
#include <cstddef>
#include <cstdint>

using namespace codec_gui;
using namespace codec_gui::gui;

namespace {

	RawImage make_gray(uint8_t value) {
		RawImage image;
		image.width                 = 2;
		image.height                = 2;
		image.format                = PixelFormat::Gray8;
		image.planes[0].strideBytes = 2;
		image.planes[0].bytes.assign(4, value);
		return image;
	}

	RawImage make_yuv420(uint8_t value) {
		RawImage image;
		image.width                 = 2;
		image.height                = 2;
		image.format                = PixelFormat::YUV420P8;
		image.planes[0].strideBytes = 2;
		image.planes[0].bytes.assign(4, value);
		image.planes[1].strideBytes = 1;
		image.planes[1].bytes.assign(1, 128);
		image.planes[2].strideBytes = 1;
		image.planes[2].bytes.assign(1, 128);
		return image;
	}

	RawImage make_gray10(uint16_t value) {
		RawImage image;
		image.width                 = 2;
		image.height                = 2;
		image.format                = PixelFormat::Gray10LE;
		image.planes[0].strideBytes = 4;
		image.planes[0].bytes.resize(8);
		for (int i = 0; i < 4; ++i) {
			image.planes[0].bytes[static_cast<std::size_t>(i) * 2]     = static_cast<uint8_t>(value & 0xffu);
			image.planes[0].bytes[static_cast<std::size_t>(i) * 2 + 1] = static_cast<uint8_t>(value >> 8u);
		}
		return image;
	}

} // namespace

int main() {
	{
		DerivedImageResult const result = compute_absolute_difference(make_gray(10), make_gray(20), 2.0);
		CODEC_VIS_CHECK(result.image);
		CODEC_VIS_CHECK(result.image->color.primaries == ColorPrimaries::BT2020);
		CODEC_VIS_CHECK(raw_image_to_linear_bt2020(*result.image).width == result.image->width);
		CODEC_VIS_CHECK(result.image->planes[0].bytes.size() == 4);
		CODEC_VIS_CHECK(result.image->planes[0].bytes[0] == 20);
	}
	{
		DerivedImageResult const result = compute_absolute_difference(make_gray(10), make_gray(20), 100.0);
		CODEC_VIS_CHECK(result.image);
		CODEC_VIS_CHECK(result.image->planes[0].bytes[0] == 255);
	}
	{
		RawImage const a                = make_gray(0);
		RawImage       b                = make_gray(0);
		b.height                        = 3;
		DerivedImageResult const result = compute_absolute_difference(a, b, 1.0);
		CODEC_VIS_CHECK(!result.image);
		CODEC_VIS_CHECK(!result.error.empty());
	}
	{
		DerivedImageResult const result = compute_absolute_difference(make_yuv420(10), make_gray(20), 2.0);
		CODEC_VIS_CHECK(result.image);
		CODEC_VIS_CHECK(result.image->format == PixelFormat::Gray8);
		CODEC_VIS_CHECK(result.image->planes[0].bytes[0] == 20);
	}
	{
		DerivedImageResult const result = compute_absolute_difference(make_gray(10), make_gray10(80), 1.0);
		CODEC_VIS_CHECK(result.image);
		CODEC_VIS_CHECK(result.image->format == PixelFormat::Gray10LE);
		CODEC_VIS_CHECK(result.image->planes[0].bytes[0] == 40);
		CODEC_VIS_CHECK(result.image->planes[0].bytes[1] == 0);
	}
	{
		RawImage rgb;
		rgb.width = rgb.height = 2;
		rgb.format             = PixelFormat::RGBP8;
		rgb.color              = {.primaries = ColorPrimaries::BT709,
								  .transfer  = TransferCharacteristics::SRGB,
								  .matrix    = MatrixCoefficients::Identity,
								  .range     = ColorRange::Full};
		for (int p = 0; p < 3; ++p) {
			rgb.planes[p].strideBytes = 2;
			rgb.planes[p].bytes.assign(4, p == 0 ? 255 : 0);
		}
		ColorTransformOptions options;
		options.target        = rgb.color;
		options.target.matrix = MatrixCoefficients::BT709;
		options.target.range  = ColorRange::Limited;
		RawImage   yuv        = transform_raw_image(rgb, PixelFormat::YUV444P8, options);
		const auto diff       = compute_absolute_difference(rgb, yuv, 1.0);
		CODEC_VIS_CHECK(diff.image);
		for (uint8_t sample: diff.image->planes[0].bytes) CODEC_VIS_CHECK(sample == 0);
		yuv.color.transfer    = TransferCharacteristics::PQ;
		const auto wrongColor = compute_absolute_difference(rgb, yuv, 1.0);
		CODEC_VIS_CHECK(!wrongColor.image && !wrongColor.error.empty());
	}
	{
		RawImage a = make_yuv420(10);
		RawImage b = a;
		b.planes[1].bytes.clear();
		CODEC_VIS_CHECK(!compute_absolute_difference(a, b, 1.0).image);
		b = a;
		b.planes[1].bytes[0] += 4;
		const auto diff = compute_absolute_difference(a, b, 1.0);
		CODEC_VIS_CHECK(diff.image && diff.image->planes[0].bytes[0] == 4);
	}
	{
		RawImage a;
		a.width = a.height = 2;
		a.format           = PixelFormat::YUV444P16LE;
		for (auto &plane: a.planes) {
			plane.strideBytes = 4;
			plane.bytes.assign(8, 0);
		}
		RawImage b           = a;
		b.planes[0].bytes[0] = 1;
		const auto diff      = compute_absolute_difference(a, b, 1.0);
		CODEC_VIS_CHECK(diff.image && diff.image->format == PixelFormat::RGBP16LE);
		for (const auto &plane: diff.image->planes) CODEC_VIS_CHECK(plane.bytes.size() == 8 && plane.bytes[0] == 1);
	}
	{
		const auto diff = compute_absolute_difference(make_gray(235), make_gray10(940), 1.0);
		CODEC_VIS_CHECK(diff.image);
		for (uint8_t sample: diff.image->planes[0].bytes) CODEC_VIS_CHECK(sample == 0);
	}
	{
		auto a = make_gray(0), b = make_gray(255);
		a.color              = {.primaries = ColorPrimaries::DisplayP3,
								.transfer  = TransferCharacteristics::SRGB,
								.matrix    = MatrixCoefficients::Identity,
								.range     = ColorRange::Full};
		b.color              = {.primaries = ColorPrimaries::BT2020,
								.transfer  = TransferCharacteristics::Linear,
								.matrix    = MatrixCoefficients::Identity,
								.range     = ColorRange::Full};
		const auto identical = compute_linear_difference(a, a, 8);
		CODEC_VIS_CHECK(identical.image);
		for (auto byte: identical.image->planes[0].bytes) CODEC_VIS_CHECK(byte == 0);
		const auto diff = compute_linear_difference(a, b, 1);
		CODEC_VIS_CHECK(diff.image && diff.image->format == PixelFormat::RGBP16LE);
		CODEC_VIS_CHECK(diff.image->planes[0].bytes[0] == 255 && diff.image->planes[0].bytes[1] == 255);
		CODEC_VIS_CHECK(raw_image_to_linear_bt2020(*diff.image).width == a.width);
		b.width += 1;
		CODEC_VIS_CHECK(!compute_linear_difference(a, b, 1).image);
	}

	return 0;
}
