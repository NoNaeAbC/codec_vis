#include "metrics.hpp"

#include <cassert>
#include <cmath>
#include <cstddef>
#include <cstdint>

using namespace codec_gui;
using namespace codec_gui::gui;

namespace {

RawImage make_gray(uint8_t value) {
	RawImage image;
	image.width = 4;
	image.height = 4;
	image.format = PixelFormat::Gray8;
	image.planes[0].strideBytes = 4;
	image.planes[0].bytes.assign(16, value);
	return image;
}

RawImage make_420(uint8_t y, uint8_t u, uint8_t v) {
	RawImage image;
	image.width = 4;
	image.height = 4;
	image.format = PixelFormat::YUV420P8;
	image.planes[0].strideBytes = 4;
	image.planes[1].strideBytes = 2;
	image.planes[2].strideBytes = 2;
	image.planes[0].bytes.assign(16, y);
	image.planes[1].bytes.assign(4, u);
	image.planes[2].bytes.assign(4, v);
	return image;
}

RawImage make_420_pattern(uint8_t offset) {
	RawImage image;
	image.width = 256;
	image.height = 256;
	image.format = PixelFormat::YUV420P8;
	image.planes[0].strideBytes = 256;
	image.planes[1].strideBytes = 128;
	image.planes[2].strideBytes = 128;
	image.planes[0].bytes.resize(256 * 256);
	image.planes[1].bytes.assign(128 * 128, 96);
	image.planes[2].bytes.assign(128 * 128, 160);
	for (int y = 0; y < image.height; ++y) {
		for (int x = 0; x < image.width; ++x) {
			image.planes[0].bytes[static_cast<std::size_t>(y * image.planes[0].strideBytes + x)] =
				static_cast<uint8_t>((x * 3 + y * 5 + offset) & 0xff);
		}
	}
	return image;
}

} // namespace

int main() {
	{
		MetricResult result = compute_psnr(make_gray(12), make_gray(12));
		assert(result.psnrY);
		assert(std::isinf(*result.psnrY));
		assert(result.psnrAll);
		assert(std::isinf(*result.psnrAll));
	}
	{
		MetricResult result = compute_psnr(make_gray(0), make_gray(1));
		assert(result.psnrY);
		assert(std::abs(*result.psnrY - 48.1308036) < 0.0001);
	}
	{
		MetricResult result = compute_psnr(make_420(10, 20, 30), make_420(11, 20, 30));
		assert(result.psnrY);
		assert(result.psnrAll);
		assert(*result.psnrAll > *result.psnrY);
	}
	{
		RawImage a = make_gray(0);
		RawImage b = make_gray(0);
		b.width = 8;
		MetricResult result = compute_psnr(a, b);
		assert(!result.psnrY);
		assert(!result.unavailableReason.empty());
	}
	{
		MetricResult result = compute_quality_metrics(make_420_pattern(0), make_420_pattern(1));
		assert(result.psnrY);
		assert(result.metrics.size() >= 6);
		EncodedMetadata metadata;
		metadata.metrics = result.metrics;
		assert(metric_by_id(metadata, "psnr-y") != nullptr);
		assert(primary_metric(metadata) != nullptr);
	}
	return 0;
}
