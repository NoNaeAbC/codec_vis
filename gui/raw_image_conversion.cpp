#include "raw_image_conversion.hpp"
#include "raw_image_utils.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>

namespace codec_gui::gui {
namespace {

uint8_t clamp_byte(double value) {
	return static_cast<uint8_t>(std::clamp<int>(static_cast<int>(std::lround(value)), 0, 255));
}

uint32_t clamp_sample(double value, int bitDepth) {
	const uint32_t maxValue = bitDepth == 8 ? 255u : 1023u;
	return std::min<uint32_t>(maxValue, static_cast<uint32_t>(std::max<int>(0, static_cast<int>(std::lround(value)))));
}

uint32_t sample_or_default(const ImagePlane& plane, int x, int y, int sampleBytes, uint32_t defaultValue) {
	if (x < 0 || y < 0 || plane.strideBytes <= 0) {
		return defaultValue;
	}
	const std::size_t byteIndex =
		static_cast<std::size_t>(y) * static_cast<std::size_t>(plane.strideBytes) +
		static_cast<std::size_t>(x) * static_cast<std::size_t>(sampleBytes);
	if (byteIndex + static_cast<std::size_t>(sampleBytes) > plane.bytes.size()) {
		return defaultValue;
	}
	return sample_at(plane, x, y, sampleBytes);
}

struct YuvScale {
	double yOffset = 16.0;
	double cOffset = 128.0;
	double yScale = 219.0;
	double cScale = 224.0;
};

YuvScale yuv_scale(const RawImage& image) {
	const bool high = is_10_bit(image.format);
	if (image.color.range == ColorRange::Full) {
		return high ? YuvScale{0.0, 512.0, 1023.0, 1023.0} : YuvScale{0.0, 128.0, 255.0, 255.0};
	}
	return high ? YuvScale{64.0, 512.0, 876.0, 896.0} : YuvScale{16.0, 128.0, 219.0, 224.0};
}

void matrix_coefficients(const MatrixCoefficients matrix, double& kr, double& kb) {
	if (matrix == MatrixCoefficients::BT2020NonConstant) {
		kr = 0.2627;
		kb = 0.0593;
		return;
	}
	kr = 0.2126;
	kb = 0.0722;
}

double transfer_to_linear(double value, TransferCharacteristics transfer) {
	value = std::clamp(value, 0.0, 1.0);
	if (transfer == TransferCharacteristics::Linear) {
		return value;
	}
	if (transfer == TransferCharacteristics::PQ) {
		constexpr double m1 = 0.1593017578125;
		constexpr double m2 = 78.84375;
		constexpr double c1 = 0.8359375;
		constexpr double c2 = 18.8515625;
		constexpr double c3 = 18.6875;
		const double vp = std::pow(value, 1.0 / m2);
		return std::pow(std::max(vp - c1, 0.0) / (c2 - c3 * vp), 1.0 / m1);
	}
	if (transfer == TransferCharacteristics::HLG) {
		return value <= 0.5 ? (value * value) / 3.0 : (std::exp((value - 0.55991073) / 0.17883277) + 0.28466892) / 12.0;
	}
	if (transfer == TransferCharacteristics::SRGB) {
		return value <= 0.04045 ? value / 12.92 : std::pow((value + 0.055) / 1.055, 2.4);
	}
	return value < 0.081 ? value / 4.5 : std::pow((value + 0.099) / 1.099, 1.0 / 0.45);
}

double linear_to_srgb(double value) {
	value = std::clamp(value, 0.0, 1.0);
	return value <= 0.0031308 ? value * 12.92 : 1.055 * std::pow(value, 1.0 / 2.4) - 0.055;
}

double tonemap_linear(double value, TransferCharacteristics transfer) {
	value = std::max(0.0, value);
	if (transfer == TransferCharacteristics::PQ || transfer == TransferCharacteristics::HLG) {
		return value / (1.0 + value);
	}
	return value;
}

void yuv_sample_to_rgb(
	const RawImage& image,
	uint32_t yy,
	uint32_t cb,
	uint32_t cr,
	double& r,
	double& g,
	double& b
) {
	const YuvScale scale = yuv_scale(image);
	const double yFull = std::clamp((static_cast<double>(yy) - scale.yOffset) / scale.yScale, 0.0, 1.0);
	const double cbFull = (static_cast<double>(cb) - scale.cOffset) / scale.cScale;
	const double crFull = (static_cast<double>(cr) - scale.cOffset) / scale.cScale;
	double kr = 0.2126;
	double kb = 0.0722;
	matrix_coefficients(image.color.matrix, kr, kb);
	const double kg = 1.0 - kr - kb;
	r = yFull + (2.0 - 2.0 * kr) * crFull;
	b = yFull + (2.0 - 2.0 * kb) * cbFull;
	g = (yFull - kr * r - kb * b) / kg;
	r = linear_to_srgb(tonemap_linear(transfer_to_linear(r, image.color.transfer), image.color.transfer)) * 255.0;
	g = linear_to_srgb(tonemap_linear(transfer_to_linear(g, image.color.transfer), image.color.transfer)) * 255.0;
	b = linear_to_srgb(tonemap_linear(transfer_to_linear(b, image.color.transfer), image.color.transfer)) * 255.0;
}

struct YCbCr {
	uint32_t y = 0;
	uint32_t cb = 0;
	uint32_t cr = 0;
};

YCbCr rgb_to_yuv_sample(double r, double g, double b, const ColorDescription& color, int bitDepth) {
	r /= 255.0;
	g /= 255.0;
	b /= 255.0;
	double kr = 0.2126;
	double kb = 0.0722;
	matrix_coefficients(color.matrix, kr, kb);
	const double kg = 1.0 - kr - kb;
	const double yFull = kr * r + kg * g + kb * b;
	const double cbFull = (b - yFull) / (2.0 - 2.0 * kb);
	const double crFull = (r - yFull) / (2.0 - 2.0 * kr);
	const bool high = bitDepth == 10;
	const bool full = color.range == ColorRange::Full;
	const double yOffset = full ? 0.0 : (high ? 64.0 : 16.0);
	const double cOffset = high ? 512.0 : 128.0;
	const double yScale = full ? (high ? 1023.0 : 255.0) : (high ? 876.0 : 219.0);
	const double cScale = full ? (high ? 1023.0 : 255.0) : (high ? 896.0 : 224.0);
	return {
		clamp_sample(yOffset + yFull * yScale, bitDepth),
		clamp_sample(cOffset + cbFull * cScale, bitDepth),
		clamp_sample(cOffset + crFull * cScale, bitDepth),
	};
}

uint32_t source_sample(const RawImage& image, int plane, int x, int y) {
	const int sampleBytes = bytes_per_sample(image.format);
	const uint32_t defaultValue = plane == 0 ? (sampleBytes == 1 ? 16u : 64u) : (sampleBytes == 1 ? 128u : 512u);
	return sample_or_default(image.planes[plane], x, y, sampleBytes, defaultValue);
}

void pixel_rgb(const RawImage& image, int x, int y, double& r, double& g, double& b) {
	if (is_gray(image.format)) {
		const int sampleBytes = bytes_per_sample(image.format);
		const double maxSample = static_cast<double>(max_sample_value(image.format));
		const double v = static_cast<double>(sample_or_default(image.planes[0], x, y, sampleBytes, 0)) * 255.0 / maxSample;
		r = g = b = v;
		return;
	}
		const int cx = (is_420(image.format) || is_422(image.format)) ? x / 2 : x;
		const int cy = is_420(image.format) ? y / 2 : y;
	yuv_sample_to_rgb(image, source_sample(image, 0, x, y), source_sample(image, 1, cx, cy), source_sample(image, 2, cx, cy), r, g, b);
}

} // namespace

std::vector<uint8_t> raw_image_to_rgba8(const RawImage& image) {
	if (image.width <= 0 || image.height <= 0) {
		return {};
	}

	std::vector<uint8_t> rgba(static_cast<std::size_t>(image.width) * static_cast<std::size_t>(image.height) * 4);
	auto store = [&](int x, int y, uint8_t r, uint8_t g, uint8_t b) {
		const std::size_t i = (static_cast<std::size_t>(y) * static_cast<std::size_t>(image.width) + static_cast<std::size_t>(x)) * 4;
		rgba[i + 0] = r;
		rgba[i + 1] = g;
		rgba[i + 2] = b;
		rgba[i + 3] = 255;
	};

	switch (image.format) {
		case PixelFormat::YUV420P8:
		case PixelFormat::YUV420P10LE:
		case PixelFormat::YUV422P8:
		case PixelFormat::YUV422P10LE: {
			const int sampleBytes = bytes_per_sample(image.format);
			const uint32_t neutralY = sampleBytes == 1 ? 16 : 64;
			const uint32_t neutralC = sampleBytes == 1 ? 128 : 512;
			for (int y = 0; y < image.height; ++y) {
				for (int x = 0; x < image.width; ++x) {
					const uint32_t yy = sample_or_default(image.planes[0], x, y, sampleBytes, neutralY);
					const uint32_t cb = sample_or_default(image.planes[1], x / 2, is_420(image.format) ? y / 2 : y, sampleBytes, neutralC);
					const uint32_t cr = sample_or_default(image.planes[2], x / 2, is_420(image.format) ? y / 2 : y, sampleBytes, neutralC);
					double r = 0.0;
					double g = 0.0;
					double b = 0.0;
					yuv_sample_to_rgb(image, yy, cb, cr, r, g, b);
					store(x, y, clamp_byte(r), clamp_byte(g), clamp_byte(b));
				}
			}
			break;
		}
		case PixelFormat::YUV444P8:
		case PixelFormat::YUV444P10LE: {
			const int sampleBytes = bytes_per_sample(image.format);
			const uint32_t neutralY = sampleBytes == 1 ? 16 : 64;
			const uint32_t neutralC = sampleBytes == 1 ? 128 : 512;
			for (int y = 0; y < image.height; ++y) {
				for (int x = 0; x < image.width; ++x) {
					const uint32_t yy = sample_or_default(image.planes[0], x, y, sampleBytes, neutralY);
					const uint32_t cb = sample_or_default(image.planes[1], x, y, sampleBytes, neutralC);
					const uint32_t cr = sample_or_default(image.planes[2], x, y, sampleBytes, neutralC);
					double r = 0.0;
					double g = 0.0;
					double b = 0.0;
					yuv_sample_to_rgb(image, yy, cb, cr, r, g, b);
					store(x, y, clamp_byte(r), clamp_byte(g), clamp_byte(b));
				}
			}
			break;
		}
		case PixelFormat::Gray8:
		case PixelFormat::Gray10LE: {
			const int sampleBytes = bytes_per_sample(image.format);
			const double maxSample = static_cast<double>(max_sample_value(image.format));
			for (int y = 0; y < image.height; ++y) {
				for (int x = 0; x < image.width; ++x) {
					const uint8_t v = clamp_byte(static_cast<double>(sample_or_default(image.planes[0], x, y, sampleBytes, 0)) * 255.0 / maxSample);
					store(x, y, v, v, v);
				}
			}
			break;
		}
	}
	return rgba;
}

RawImage convert_raw_image_format(const RawImage& image, PixelFormat targetFormat) {
	if (image.format == targetFormat) {
		return image;
	}
	RawImage out;
	out.width = image.width;
	out.height = image.height;
	out.format = targetFormat;
	out.color = image.color;
	const int targetDepth = bit_depth(targetFormat);
	const int bps = bytes_per_sample(targetFormat);
	const int planes = plane_count(targetFormat);
	for (int plane = 0; plane < planes; ++plane) {
		const int w = plane_width(out, plane);
		const int h = plane_height(out, plane);
		out.planes[plane].strideBytes = w * bps;
		out.planes[plane].bytes.resize(static_cast<std::size_t>(out.planes[plane].strideBytes) * static_cast<std::size_t>(h));
	}
	for (int y = 0; y < out.height; ++y) {
		for (int x = 0; x < out.width; ++x) {
			double r = 0.0;
			double g = 0.0;
			double b = 0.0;
			pixel_rgb(image, x, y, r, g, b);
			const YCbCr ycc = rgb_to_yuv_sample(r, g, b, out.color, targetDepth);
			put_sample(out.planes[0], x, y, bps, ycc.y);
				if (is_444(targetFormat)) {
					put_sample(out.planes[1], x, y, bps, ycc.cb);
					put_sample(out.planes[2], x, y, bps, ycc.cr);
				}
			}
		}
	if (is_422(targetFormat)) {
		for (int y = 0; y < out.height; ++y) {
			for (int x = 0; x < out.width / 2; ++x) {
				double cb = 0.0;
				double cr = 0.0;
				for (int dx = 0; dx < 2; ++dx) {
					double r = 0.0;
					double g = 0.0;
					double b = 0.0;
					pixel_rgb(image, std::min(out.width - 1, x * 2 + dx), y, r, g, b);
					const YCbCr ycc = rgb_to_yuv_sample(r, g, b, out.color, targetDepth);
					cb += ycc.cb;
					cr += ycc.cr;
				}
				put_sample(out.planes[1], x, y, bps, clamp_sample(cb * 0.5, targetDepth));
				put_sample(out.planes[2], x, y, bps, clamp_sample(cr * 0.5, targetDepth));
			}
		}
	}
	if (is_420(targetFormat)) {
		for (int y = 0; y < out.height / 2; ++y) {
			for (int x = 0; x < out.width / 2; ++x) {
				double cb = 0.0;
				double cr = 0.0;
				for (int dy = 0; dy < 2; ++dy) {
					for (int dx = 0; dx < 2; ++dx) {
						double r = 0.0;
						double g = 0.0;
						double b = 0.0;
						pixel_rgb(image, std::min(out.width - 1, x * 2 + dx), std::min(out.height - 1, y * 2 + dy), r, g, b);
						const YCbCr ycc = rgb_to_yuv_sample(r, g, b, out.color, targetDepth);
						cb += ycc.cb;
						cr += ycc.cr;
					}
				}
				put_sample(out.planes[1], x, y, bps, clamp_sample(cb * 0.25, targetDepth));
				put_sample(out.planes[2], x, y, bps, clamp_sample(cr * 0.25, targetDepth));
			}
		}
	}
	return out;
}

} // namespace codec_gui::gui
