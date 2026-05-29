#include "raw_image_conversion.hpp"

#include <cassert>
#include <cstdint>
#include <vector>

namespace codec_gui::gui {
namespace {

RawImage make_gray10_image() {
	RawImage image;
	image.width = 2;
	image.height = 1;
	image.format = PixelFormat::Gray10LE;
	image.planes[0].strideBytes = 4;
	image.planes[0].bytes = {
		0x00, 0x00,
		0xff, 0x03,
	};
	return image;
}

RawImage make_yuv420_neutral_image() {
	RawImage image;
	image.width = 2;
	image.height = 2;
	image.format = PixelFormat::YUV420P8;
	image.planes[0].strideBytes = 2;
	image.planes[1].strideBytes = 1;
	image.planes[2].strideBytes = 1;
	image.planes[0].bytes = {16, 235, 81, 145};
	image.planes[1].bytes = {128};
	image.planes[2].bytes = {128};
	return image;
}

} // namespace
} // namespace codec_gui::gui

int main() {
	using namespace codec_gui;
	using namespace codec_gui::gui;

	const std::vector<uint8_t> gray = raw_image_to_rgba8(make_gray10_image());
	assert(gray.size() == 8);
	assert(gray[0] == 0);
	assert(gray[1] == 0);
	assert(gray[2] == 0);
	assert(gray[3] == 255);
	assert(gray[4] == 255);
	assert(gray[5] == 255);
	assert(gray[6] == 255);
	assert(gray[7] == 255);

	const std::vector<uint8_t> yuv = raw_image_to_rgba8(make_yuv420_neutral_image());
	assert(yuv.size() == 16);
	assert(yuv[0] == 0);
	assert(yuv[1] == 0);
	assert(yuv[2] == 0);
	assert(yuv[4] == 255);
	assert(yuv[5] == 255);
	assert(yuv[6] == 255);
	assert(yuv[8] > 70 && yuv[8] < 80);
	assert(yuv[9] > 70 && yuv[9] < 80);
	assert(yuv[10] > 70 && yuv[10] < 80);
	assert(yuv[12] > 145 && yuv[12] < 155);
	assert(yuv[13] > 145 && yuv[13] < 155);
	assert(yuv[14] > 145 && yuv[14] < 155);

	return 0;
}
