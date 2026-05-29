#include "image_ops.hpp"
#include "raw_image_utils.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>

namespace codec_gui::gui {
namespace {

uint32_t luma_sample_10bit(const RawImage& image, int x, int y) {
	const int bps = bytes_per_sample(image.format);
	const uint32_t value = sample_at(image.planes[0], x, y, bps);
	return bps == 1 ? value * 4u : std::min<uint32_t>(1023u, value);
}

DerivedImageResult compute_luma_difference(const RawImage& a, const RawImage& b, double gain) {
	if (!plane_available(a, 0) || !plane_available(b, 0)) {
		return {nullptr, "plane buffer is incomplete"};
	}
	const bool highBitDepth = is_10_bit(a.format) || is_10_bit(b.format);
	const int bps = highBitDepth ? 2 : 1;
	auto out = std::make_shared<RawImage>();
	out->width = a.width;
	out->height = a.height;
	out->format = highBitDepth ? PixelFormat::Gray10LE : PixelFormat::Gray8;
	out->planes[0].strideBytes = out->width * bps;
	out->planes[0].bytes.resize(static_cast<std::size_t>(out->planes[0].strideBytes) * static_cast<std::size_t>(out->height));
	for (int y = 0; y < out->height; ++y) {
		for (int x = 0; x < out->width; ++x) {
			const int diff10 = std::abs(static_cast<int>(luma_sample_10bit(a, x, y)) - static_cast<int>(luma_sample_10bit(b, x, y)));
			const double scaled = static_cast<double>(diff10) * gain;
			const uint32_t output = highBitDepth
				? static_cast<uint32_t>(std::lround(scaled))
				: static_cast<uint32_t>(std::lround(scaled / 4.0));
			put_sample(out->planes[0], x, y, bps, output);
		}
	}
	return {out, {}};
}

} // namespace

DerivedImageResult compute_absolute_difference(const RawImage& a, const RawImage& b, double gain) {
	if (a.width != b.width || a.height != b.height) {
		return {nullptr, "image dimensions differ"};
	}
	gain = std::max(0.0, gain);
	if (a.format != b.format) {
		return compute_luma_difference(a, b, gain);
	}
	const int planes = plane_count(a.format);
	const int bps = bytes_per_sample(a.format);
	auto out = std::make_shared<RawImage>();
	out->width = a.width;
	out->height = a.height;
	out->format = a.format;
	for (int plane = 0; plane < planes; ++plane) {
		if (!plane_available(a, plane) || !plane_available(b, plane)) {
			return {nullptr, "plane buffer is incomplete"};
		}
		const int w = plane_width(a, plane);
		const int h = plane_height(a, plane);
		out->planes[plane].strideBytes = w * bps;
		out->planes[plane].bytes.resize(static_cast<std::size_t>(out->planes[plane].strideBytes) * static_cast<std::size_t>(h));
		for (int y = 0; y < h; ++y) {
			for (int x = 0; x < w; ++x) {
				const int diff = std::abs(static_cast<int>(sample_at(a.planes[plane], x, y, bps)) - static_cast<int>(sample_at(b.planes[plane], x, y, bps)));
				put_sample(out->planes[plane], x, y, bps, static_cast<uint32_t>(std::lround(static_cast<double>(diff) * gain)));
			}
		}
	}
	return {out, {}};
}

} // namespace codec_gui::gui
