#include "raw_image_conversion.hpp"
#include "raw_image_utils.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <stdexcept>

namespace codec_gui::gui {
namespace {

uint8_t clamp_byte(double value) {
	return static_cast<uint8_t>(std::clamp<int>(static_cast<int>(std::lround(value)), 0, 255));
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
	const int depth = bit_depth(image.format);
	const double maximum = static_cast<double>((1u << depth) - 1u);
	const double midpoint = static_cast<double>(1u << (depth - 1));
	if (image.color.range == ColorRange::Full) {
		return YuvScale{0.0, midpoint, maximum, maximum};
	}
	const double scale = static_cast<double>(1u << (depth - 8));
	return YuvScale{16.0 * scale, 128.0 * scale, 219.0 * scale, 224.0 * scale};
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

double linear_to_transfer(double value, TransferCharacteristics transfer) {
	value = std::max(0.0, value);
	if (transfer == TransferCharacteristics::Linear) return std::clamp(value, 0.0, 1.0);
	if (transfer == TransferCharacteristics::PQ) {
		constexpr double m1 = 0.1593017578125;
		constexpr double m2 = 78.84375;
		constexpr double c1 = 0.8359375;
		constexpr double c2 = 18.8515625;
		constexpr double c3 = 18.6875;
		const double p = std::pow(value, m1);
		return std::pow((c1 + c2 * p) / (1.0 + c3 * p), m2);
	}
	if (transfer == TransferCharacteristics::HLG) {
		return value <= 1.0 / 12.0 ? std::sqrt(3.0 * value) : 0.17883277 * std::log(12.0 * value - 0.28466892) + 0.55991073;
	}
	if (transfer == TransferCharacteristics::SRGB) return linear_to_srgb(value);
	return value < 0.018 ? 4.5 * value : 1.099 * std::pow(value, 0.45) - 0.099;
}

struct Rgb {
	double r = 0.0;
	double g = 0.0;
	double b = 0.0;
};

Rgb multiply(const double matrix[9], Rgb value) {
	return {
		matrix[0] * value.r + matrix[1] * value.g + matrix[2] * value.b,
		matrix[3] * value.r + matrix[4] * value.g + matrix[5] * value.b,
		matrix[6] * value.r + matrix[7] * value.g + matrix[8] * value.b,
	};
}

const double* rgb_to_xyz_matrix(ColorPrimaries primaries) {
	static constexpr double BT709[9] = {0.4123908, 0.3575843, 0.1804808, 0.2126390, 0.7151687, 0.0721923, 0.0193308, 0.1191948, 0.9505322};
	static constexpr double BT2020[9] = {0.6369580, 0.1446169, 0.1688810, 0.2627002, 0.6779981, 0.0593017, 0.0000000, 0.0280727, 1.0609851};
	static constexpr double P3D65[9] = {0.4865709, 0.2656677, 0.1982173, 0.2289746, 0.6917385, 0.0792869, 0.0000000, 0.0451134, 1.0439444};
	switch (primaries) {
		case ColorPrimaries::BT709: return BT709;
		case ColorPrimaries::BT2020: return BT2020;
		case ColorPrimaries::DisplayP3: return P3D65;
		default: throw std::invalid_argument("unsupported or unspecified source color primaries");
	}
}

const double* xyz_to_rgb_matrix(ColorPrimaries primaries) {
	static constexpr double BT709[9] = {3.2409699, -1.5373832, -0.4986108, -0.9692436, 1.8759675, 0.0415551, 0.0556301, -0.2039770, 1.0569715};
	static constexpr double BT2020[9] = {1.7166512, -0.3556708, -0.2533663, -0.6666844, 1.6164812, 0.0157685, 0.0176399, -0.0427706, 0.9421031};
	static constexpr double P3D65[9] = {2.4934969, -0.9313836, -0.4027108, -0.8294890, 1.7626641, 0.0236247, 0.0358458, -0.0761724, 0.9568845};
	switch (primaries) {
		case ColorPrimaries::BT709: return BT709;
		case ColorPrimaries::BT2020: return BT2020;
		case ColorPrimaries::DisplayP3: return P3D65;
		default: throw std::invalid_argument("unsupported or unspecified target color primaries");
	}
}

bool is_hdr(TransferCharacteristics transfer) {
	return transfer == TransferCharacteristics::PQ || transfer == TransferCharacteristics::HLG;
}

bool wider_than_bt709(ColorPrimaries primaries) {
	return primaries == ColorPrimaries::BT2020 || primaries == ColorPrimaries::DisplayP3;
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
	if (image.color.transfer == TransferCharacteristics::Unspecified) {
		// Preserve the coded R'G'B' values. Guessing a transfer function here would
		// silently change an image whose interpretation is explicitly unknown.
		r *= 255.0;
		g *= 255.0;
		b *= 255.0;
	} else {
		r = linear_to_srgb(tonemap_linear(transfer_to_linear(r, image.color.transfer), image.color.transfer)) * 255.0;
		g = linear_to_srgb(tonemap_linear(transfer_to_linear(g, image.color.transfer), image.color.transfer)) * 255.0;
		b = linear_to_srgb(tonemap_linear(transfer_to_linear(b, image.color.transfer), image.color.transfer)) * 255.0;
	}
}

uint32_t source_sample(const RawImage& image, int plane, int x, int y) {
	const int sampleBytes = bytes_per_sample(image.format);
	const int depth = bit_depth(image.format);
	const uint32_t scale = 1u << (depth - 8);
	const uint32_t defaultValue = plane == 0 ? 16u * scale : 128u * scale;
	return sample_or_default(image.planes[plane], x, y, sampleBytes, defaultValue);
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
		case PixelFormat::RGBP8:
		case PixelFormat::RGBP14LE:
		case PixelFormat::RGBP16LE: {
			const int sampleBytes = bytes_per_sample(image.format);
			const double scale = 255.0 / max_sample_value(image.format);
			for (int y = 0; y < image.height; ++y) {
				for (int x = 0; x < image.width; ++x) {
					const uint8_t r = clamp_byte(sample_or_default(image.planes[0], x, y, sampleBytes, 0) * scale);
					const uint8_t g = clamp_byte(sample_or_default(image.planes[1], x, y, sampleBytes, 0) * scale);
					const uint8_t b = clamp_byte(sample_or_default(image.planes[2], x, y, sampleBytes, 0) * scale);
					store(x, y, r, g, b);
				}
			}
			break;
		}
		case PixelFormat::YUV420P8:
		case PixelFormat::YUV420P10LE:
		case PixelFormat::YUV420P12LE:
		case PixelFormat::YUV420P14LE:
		case PixelFormat::YUV422P8:
		case PixelFormat::YUV422P10LE:
		case PixelFormat::YUV422P12LE:
		case PixelFormat::YUV422P14LE: {
			const int sampleBytes = bytes_per_sample(image.format);
			const uint32_t depthScale = 1u << (bit_depth(image.format) - 8);
			const uint32_t neutralY = 16u * depthScale;
			const uint32_t neutralC = 128u * depthScale;
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
		case PixelFormat::YUV444P10LE:
		case PixelFormat::YUV444P12LE:
		case PixelFormat::YUV444P14LE:
		case PixelFormat::YUV444P16LE: {
			const int sampleBytes = bytes_per_sample(image.format);
			const uint32_t depthScale = 1u << (bit_depth(image.format) - 8);
			const uint32_t neutralY = 16u * depthScale;
			const uint32_t neutralC = 128u * depthScale;
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
		case PixelFormat::Gray10LE:
		case PixelFormat::Gray12LE:
		case PixelFormat::Gray14LE: {
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
	const int sourceDepth = bit_depth(image.format);
	const int targetDepth = bit_depth(targetFormat);
	const uint32_t sourceMaximum = (1u << sourceDepth) - 1u;
	const uint32_t targetMaximum = (1u << targetDepth) - 1u;
	const int bps = bytes_per_sample(targetFormat);
	const int planes = plane_count(targetFormat);
	for (int plane = 0; plane < planes; ++plane) {
		const int w = plane_width(out, plane);
		const int h = plane_height(out, plane);
		out.planes[plane].strideBytes = w * bps;
		out.planes[plane].bytes.resize(static_cast<std::size_t>(out.planes[plane].strideBytes) * static_cast<std::size_t>(h));
	}
	const int sourceBps = bytes_per_sample(image.format);
	auto scale_depth = [&](uint64_t sample) -> uint32_t {
		return static_cast<uint32_t>((sample * targetMaximum + sourceMaximum / 2u) / sourceMaximum);
	};
	for (int y = 0; y < out.height; ++y) {
		for (int x = 0; x < out.width; ++x) {
			put_sample(out.planes[0], x, y, bps, scale_depth(source_sample(image, 0, x, y)), targetMaximum);
		}
	}
	if (!is_gray(targetFormat)) {
		const int targetStepX = is_444(targetFormat) ? 1 : 2;
		const int targetStepY = is_420(targetFormat) ? 2 : 1;
		const int sourceStepX = is_444(image.format) || is_gray(image.format) ? 1 : 2;
		const int sourceStepY = is_420(image.format) ? 2 : 1;
		const uint32_t sourceNeutral = 1u << (sourceDepth - 1);
		for (int plane = 1; plane < 3; ++plane) {
			const int targetW = plane_width(out, plane);
			const int targetH = plane_height(out, plane);
			for (int cy = 0; cy < targetH; ++cy) {
				for (int cx = 0; cx < targetW; ++cx) {
					uint64_t sum = 0;
					uint32_t count = 0;
					for (int dy = 0; dy < targetStepY; ++dy) {
						for (int dx = 0; dx < targetStepX; ++dx) {
							const int lx = std::min(image.width - 1, cx * targetStepX + dx);
							const int ly = std::min(image.height - 1, cy * targetStepY + dy);
							sum += is_gray(image.format)
								? sourceNeutral
								: sample_or_default(image.planes[plane], lx / sourceStepX, ly / sourceStepY, sourceBps, sourceNeutral);
							++count;
						}
					}
					put_sample(out.planes[plane], cx, cy, bps, scale_depth((sum + count / 2u) / count), targetMaximum);
				}
			}
		}
	}
	return out;
}

RawImage transform_raw_image(const RawImage& image, PixelFormat targetFormat, const ColorTransformOptions& options) {
	if (!is_rgb(image.format) &&
	    image.color.primaries == options.target.primaries &&
	    image.color.transfer == options.target.transfer &&
	    image.color.matrix == options.target.matrix &&
	    image.color.range == options.target.range) {
		RawImage out = convert_raw_image_format(image, targetFormat);
		out.color.chroma420Location = options.target.chroma420Location;
		return out;
	}
	if (image.color.primaries == ColorPrimaries::Unspecified ||
	    image.color.transfer == TransferCharacteristics::Unspecified ||
	    image.color.matrix == MatrixCoefficients::Unspecified) {
		throw std::invalid_argument("color transform requires explicit source primaries, transfer, and matrix metadata");
	}
	if (options.target.primaries == ColorPrimaries::Unspecified ||
	    options.target.transfer == TransferCharacteristics::Unspecified ||
	    options.target.matrix == MatrixCoefficients::Unspecified) {
		throw std::invalid_argument("color transform requires explicit target primaries, transfer, and matrix");
	}
	if ((image.color.matrix == MatrixCoefficients::Identity && !is_rgb(image.format)) ||
	    options.target.matrix == MatrixCoefficients::Identity) {
		throw std::invalid_argument("identity matrix metadata requires an RGB planar pixel format");
	}
	const bool dynamicRangeReduction = is_hdr(image.color.transfer) && !is_hdr(options.target.transfer);
	const bool gamutReduction = wider_than_bt709(image.color.primaries) && options.target.primaries == ColorPrimaries::BT709;
	if ((dynamicRangeReduction || gamutReduction) && options.toneMap == ToneMapMode::None) {
		throw std::invalid_argument("HDR or wide-gamut output reduction requires an explicit tone/gamut mapping mode");
	}
	if (options.sourcePeakNits <= 0.0 || options.targetPeakNits <= 0.0) {
		throw std::invalid_argument("source and target peak luminance must be positive");
	}

	RawImage out;
	out.width = image.width;
	out.height = image.height;
	out.format = targetFormat;
	out.color = options.target;
	const int targetDepth = bit_depth(targetFormat);
	const int targetBps = bytes_per_sample(targetFormat);
	const uint32_t targetMaximum = (1u << targetDepth) - 1u;
	for (int plane = 0; plane < plane_count(targetFormat); ++plane) {
		out.planes[plane].strideBytes = plane_width(out, plane) * targetBps;
		out.planes[plane].bytes.resize(static_cast<std::size_t>(out.planes[plane].strideBytes) * plane_height(out, plane));
	}

	const std::size_t pixels = static_cast<std::size_t>(out.width) * out.height;
	std::vector<double> targetY(pixels);
	std::vector<double> targetCb(pixels);
	std::vector<double> targetCr(pixels);
	const YuvScale sourceScale = yuv_scale(image);
	RawImage targetScaleImage;
	targetScaleImage.format = targetFormat;
	targetScaleImage.color = options.target;
	const YuvScale targetScale = yuv_scale(targetScaleImage);
	double sourceKr = 0.0;
	double sourceKb = 0.0;
	double targetKr = 0.0;
	double targetKb = 0.0;
	matrix_coefficients(image.color.matrix, sourceKr, sourceKb);
	matrix_coefficients(options.target.matrix, targetKr, targetKb);
	const double sourceKg = 1.0 - sourceKr - sourceKb;
	const double targetKg = 1.0 - targetKr - targetKb;
	const int sourceBps = bytes_per_sample(image.format);
	const uint32_t neutral = 1u << (bit_depth(image.format) - 1);
	const double sourceWhiteNits = is_hdr(image.color.transfer) ? options.sourcePeakNits : 203.0;
	const double targetWhiteNits = is_hdr(options.target.transfer) ? options.targetPeakNits : 203.0;

	for (int y = 0; y < out.height; ++y) {
		for (int x = 0; x < out.width; ++x) {
			double rp = 0.0;
			double gp = 0.0;
			double bp = 0.0;
			if (is_rgb(image.format)) {
				const double maximum = max_sample_value(image.format);
				rp = sample_or_default(image.planes[0], x, y, sourceBps, 0) / maximum;
				gp = sample_or_default(image.planes[1], x, y, sourceBps, 0) / maximum;
				bp = sample_or_default(image.planes[2], x, y, sourceBps, 0) / maximum;
			} else if (is_gray(image.format)) {
				const uint32_t yy = source_sample(image, 0, x, y);
				rp = gp = bp = std::clamp((static_cast<double>(yy) - sourceScale.yOffset) / sourceScale.yScale, 0.0, 1.0);
			} else {
				const uint32_t yy = source_sample(image, 0, x, y);
				const int cx = (is_420(image.format) || is_422(image.format)) ? x / 2 : x;
				const int cy = is_420(image.format) ? y / 2 : y;
				const double yp = std::clamp((static_cast<double>(yy) - sourceScale.yOffset) / sourceScale.yScale, 0.0, 1.0);
				const double cb = (sample_or_default(image.planes[1], cx, cy, sourceBps, neutral) - sourceScale.cOffset) / sourceScale.cScale;
				const double cr = (sample_or_default(image.planes[2], cx, cy, sourceBps, neutral) - sourceScale.cOffset) / sourceScale.cScale;
				rp = yp + (2.0 - 2.0 * sourceKr) * cr;
				bp = yp + (2.0 - 2.0 * sourceKb) * cb;
				gp = (yp - sourceKr * rp - sourceKb * bp) / sourceKg;
			}
			Rgb linear{
				transfer_to_linear(rp, image.color.transfer) * sourceWhiteNits,
				transfer_to_linear(gp, image.color.transfer) * sourceWhiteNits,
				transfer_to_linear(bp, image.color.transfer) * sourceWhiteNits,
			};
			linear = multiply(xyz_to_rgb_matrix(options.target.primaries), multiply(rgb_to_xyz_matrix(image.color.primaries), linear));
			if (options.toneMap != ToneMapMode::None) {
				auto map = [&](double value) {
					const double normalized = std::max(0.0, value / options.targetPeakNits);
					return options.targetPeakNits * (options.toneMap == ToneMapMode::Clip ? std::min(1.0, normalized) : normalized / (1.0 + normalized));
				};
				linear = {map(linear.r), map(linear.g), map(linear.b)};
			}
			const Rgb signal{
				linear_to_transfer(linear.r / targetWhiteNits, options.target.transfer),
				linear_to_transfer(linear.g / targetWhiteNits, options.target.transfer),
				linear_to_transfer(linear.b / targetWhiteNits, options.target.transfer),
			};
			const double yp = targetKr * signal.r + targetKg * signal.g + targetKb * signal.b;
			const std::size_t index = static_cast<std::size_t>(y) * out.width + x;
			targetY[index] = targetScale.yOffset + yp * targetScale.yScale;
			targetCb[index] = targetScale.cOffset + ((signal.b - yp) / (2.0 - 2.0 * targetKb)) * targetScale.cScale;
			targetCr[index] = targetScale.cOffset + ((signal.r - yp) / (2.0 - 2.0 * targetKr)) * targetScale.cScale;
		}
	}
	auto target_sample = [&](double value) {
		return static_cast<uint32_t>(std::clamp<long>(std::lround(value), 0, targetMaximum));
	};
	for (int y = 0; y < out.height; ++y) {
		for (int x = 0; x < out.width; ++x) {
			put_sample(out.planes[0], x, y, targetBps, target_sample(targetY[static_cast<std::size_t>(y) * out.width + x]), targetMaximum);
		}
	}
	if (!is_gray(targetFormat)) {
		const int stepX = is_444(targetFormat) ? 1 : 2;
		const int stepY = is_420(targetFormat) ? 2 : 1;
		for (int cy = 0; cy < plane_height(out, 1); ++cy) {
			for (int cx = 0; cx < plane_width(out, 1); ++cx) {
				double cb = 0.0;
				double cr = 0.0;
				int count = 0;
				for (int dy = 0; dy < stepY; ++dy) for (int dx = 0; dx < stepX; ++dx) {
					const int x = std::min(out.width - 1, cx * stepX + dx);
					const int y = std::min(out.height - 1, cy * stepY + dy);
					const std::size_t index = static_cast<std::size_t>(y) * out.width + x;
					cb += targetCb[index];
					cr += targetCr[index];
					++count;
				}
				put_sample(out.planes[1], cx, cy, targetBps, target_sample(cb / count), targetMaximum);
				put_sample(out.planes[2], cx, cy, targetBps, target_sample(cr / count), targetMaximum);
			}
		}
	}
	return out;
}

} // namespace codec_gui::gui
