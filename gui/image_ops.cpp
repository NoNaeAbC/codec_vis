#include "image_ops.hpp"
#include "../codec_gui_x265.hpp"
#include "raw_image_conversion.hpp"
#include "raw_image_utils.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <memory>

namespace codec_gui::gui {
	namespace {

		uint32_t sample_at_depth(const RawImage &image, int plane, int x, int y, int targetDepth) {
			const uint32_t value       = sample_at(image.planes[plane], x, y, bytes_per_sample(image.format));
			const int      sourceDepth = bit_depth(image.format);
			return sourceDepth < targetDepth ? value << (targetDepth - sourceDepth)
											 : value >> (sourceDepth - targetDepth);
		}

		PixelFormat gray_format(int depth) {
			if (depth == 16) return PixelFormat::RGBP16LE;
			if (depth == 14) return PixelFormat::Gray14LE;
			if (depth == 12) return PixelFormat::Gray12LE;
			if (depth == 10) return PixelFormat::Gray10LE;
			return PixelFormat::Gray8;
		}

		DerivedImageResult compute_difference_heatmap(const RawImage &a, const RawImage &b, double gain) {
			if (!plane_available(a, 0) || !plane_available(b, 0)) {
				return {.image = nullptr, .error = "plane buffer is incomplete"};
			}
			const int      targetDepth = std::max(bit_depth(a.format), bit_depth(b.format));
			const int      bps         = targetDepth == 8 ? 1 : 2;
			const uint32_t maximum     = (1u << targetDepth) - 1u;
			auto           out         = std::make_shared<RawImage>();
			out->width                 = a.width;
			out->height                = a.height;
			out->format                = gray_format(targetDepth);
			// A neutral diagnostic heatmap has an explicit linear working-space interpretation.
			out->color.primaries       = ColorPrimaries::BT2020;
			out->color.transfer        = TransferCharacteristics::Linear;
			out->color.matrix          = MatrixCoefficients::Identity;
			out->color.range           = ColorRange::Full;
			out->planes[0].strideBytes = out->width * bps;
			out->planes[0].bytes.resize(static_cast<std::size_t>(out->planes[0].strideBytes) *
										static_cast<std::size_t>(out->height));
			for (int y = 0; y < out->height; ++y) {
				for (int x = 0; x < out->width; ++x) {
					auto difference =
							static_cast<uint32_t>(std::abs(static_cast<int>(sample_at_depth(a, 0, x, y, targetDepth)) -
														   static_cast<int>(sample_at_depth(b, 0, x, y, targetDepth))));
					if (!is_gray(a.format) && !is_gray(b.format) && plane_available(a, 1) && plane_available(a, 2) &&
						plane_available(b, 1) && plane_available(b, 2)) {
						const int ax = (is_420(a.format) || is_422(a.format)) ? x / 2 : x;
						const int ay = is_420(a.format) ? y / 2 : y;
						const int bx = (is_420(b.format) || is_422(b.format)) ? x / 2 : x;
						const int by = is_420(b.format) ? y / 2 : y;
						for (int plane = 1; plane < 3; ++plane) {
							difference = std::max(
									difference,
									static_cast<uint32_t>(std::abs(
											static_cast<int>(sample_at_depth(a, plane, ax, ay, targetDepth)) -
											static_cast<int>(sample_at_depth(b, plane, bx, by, targetDepth)))));
						}
					}
					const uint32_t output = static_cast<uint32_t>(std::clamp(
							std::lround(static_cast<double>(difference) * gain), 0l, static_cast<long>(maximum)));
					put_sample(out->planes[0], x, y, bps, output, maximum);
				}
			}
			// There is no Gray16 format; a 16-bit neutral RGB heatmap retains precision.
			if (targetDepth == 16) {
				out->planes[1] = out->planes[0];
				out->planes[2] = out->planes[0];
			}
			return {.image = out, .error = {}};
		}

	} // namespace

	DerivedImageResult compute_absolute_difference(const RawImage &a, const RawImage &b, double gain) {
		try {
			const ComparisonImages images = normalize_comparison(a, b);
			gain                          = std::max(0.0, gain);
			return compute_difference_heatmap(images.reference, images.candidate, gain);
		} catch (const std::exception &e) { return {.image = nullptr, .error = e.what()}; }
	}

	DerivedImageResult compute_linear_difference(const RawImage &a, const RawImage &b, double gain) {
		try {
			if (a.width != b.width || a.height != b.height)
				throw std::invalid_argument("difference inputs must have matching dimensions");
			if (!std::isfinite(gain) || gain < 0)
				throw std::invalid_argument("difference gain must be finite and nonnegative");
			auto first = raw_image_to_linear_bt2020(a), second = raw_image_to_linear_bt2020(b);
			auto out    = std::make_shared<RawImage>();
			out->width  = a.width;
			out->height = a.height;
			out->format = PixelFormat::RGBP16LE;
			out->color  = {.primaries = ColorPrimaries::BT2020,
						   .transfer  = TransferCharacteristics::Linear,
						   .matrix    = MatrixCoefficients::Identity,
						   .range     = ColorRange::Full};
			for (auto &plane: out->planes) {
				plane.strideBytes = a.width * 2;
				plane.bytes.resize(std::size_t(a.width) * a.height * 2);
			}
			for (std::size_t i = 0; i < first.planes[0].size(); ++i) {
				double error = 0;
				for (int p = 0; p < 3; ++p)
					error = std::max(error, std::abs(double(first.planes[p][i]) - second.planes[p][i]));
				// This is the final visual heatmap, not a score or working-space intermediate.
				auto code = uint32_t(std::min(65535., std::floor(std::clamp(error * gain, 0., 1.) * 65536)));
				for (auto &plane: out->planes) {
					plane.bytes[i * 2]     = uint8_t(code);
					plane.bytes[i * 2 + 1] = uint8_t(code >> 8);
				}
			}
			return {.image = out, .error = {}};
		} catch (const std::exception &e) { return {.image = nullptr, .error = e.what()}; }
	}

} // namespace codec_gui::gui
