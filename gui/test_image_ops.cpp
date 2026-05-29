#include "image_ops.hpp"

#include <cassert>
#include <cstdint>

using namespace codec_gui;
using namespace codec_gui::gui;

namespace {

RawImage make_gray(uint8_t value) {
	RawImage image;
	image.width = 2;
	image.height = 2;
	image.format = PixelFormat::Gray8;
	image.planes[0].strideBytes = 2;
	image.planes[0].bytes.assign(4, value);
	return image;
}

RawImage make_yuv420(uint8_t value) {
	RawImage image;
	image.width = 2;
	image.height = 2;
	image.format = PixelFormat::YUV420P8;
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
	image.width = 2;
	image.height = 2;
	image.format = PixelFormat::Gray10LE;
	image.planes[0].strideBytes = 4;
	image.planes[0].bytes.resize(8);
	for (int i = 0; i < 4; ++i) {
		image.planes[0].bytes[static_cast<std::size_t>(i) * 2] = static_cast<uint8_t>(value & 0xffu);
		image.planes[0].bytes[static_cast<std::size_t>(i) * 2 + 1] = static_cast<uint8_t>(value >> 8u);
	}
	return image;
}

} // namespace

int main() {
	{
		DerivedImageResult result = compute_absolute_difference(make_gray(10), make_gray(20), 2.0);
		assert(result.image);
		assert(result.image->planes[0].bytes.size() == 4);
		assert(result.image->planes[0].bytes[0] == 20);
	}
	{
		DerivedImageResult result = compute_absolute_difference(make_gray(10), make_gray(20), 100.0);
		assert(result.image);
		assert(result.image->planes[0].bytes[0] == 255);
	}
	{
		RawImage a = make_gray(0);
		RawImage b = make_gray(0);
		b.height = 3;
		DerivedImageResult result = compute_absolute_difference(a, b, 1.0);
		assert(!result.image);
		assert(!result.error.empty());
	}
	{
		DerivedImageResult result = compute_absolute_difference(make_yuv420(10), make_gray(20), 2.0);
		assert(result.image);
		assert(result.image->format == PixelFormat::Gray8);
		assert(result.image->planes[0].bytes[0] == 20);
	}
	{
		DerivedImageResult result = compute_absolute_difference(make_gray(10), make_gray10(80), 1.0);
		assert(result.image);
		assert(result.image->format == PixelFormat::Gray10LE);
		assert(result.image->planes[0].bytes[0] == 40);
		assert(result.image->planes[0].bytes[1] == 0);
	}
	return 0;
}
