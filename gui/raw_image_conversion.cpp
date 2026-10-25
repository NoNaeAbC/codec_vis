#include "raw_image_conversion.hpp"
#include "../codec_gui_x265.hpp"
#include "raw_image_utils.hpp"
#include "signal_math.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <vector>

namespace codec_gui::gui {
namespace {

uint8_t clamp_byte(double value) {
	return static_cast<uint8_t>(quantize_sample(value, 255));
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
	const auto maximum = static_cast<double>((1u << depth) - 1u);
	const auto midpoint = static_cast<double>(1u << (depth - 1));
	if (image.color.range == ColorRange::Full) {
		return YuvScale{.yOffset=0.0, .cOffset=midpoint, .yScale=maximum, .cScale=maximum};
	}
	const auto scale = static_cast<double>(1u << (depth - 8));
	return YuvScale{.yOffset=16.0 * scale, .cOffset=128.0 * scale, .yScale=219.0 * scale, .cScale=224.0 * scale};
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

void primaries_luma_coefficients(const ColorPrimaries primaries, double& kr, double& kb) {
	if (primaries == ColorPrimaries::BT2020) {
		kr = 0.2627;
		kb = 0.0593;
	} else if (primaries == ColorPrimaries::DisplayP3) {
		kr = 0.2289746;
		kb = 0.0792869;
	} else {
		kr = 0.2126;
		kb = 0.0722;
	}
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
		return srgb_eotf(value);
	}
	if (transfer == TransferCharacteristics::Gamma22) return std::pow(value, 2.2);
	if (transfer == TransferCharacteristics::Gamma28) return std::pow(value, 2.8);
	return value < 0.081 ? value / 4.5 : std::pow((value + 0.099) / 1.099, 1.0 / 0.45);
}

double linear_to_srgb(double value) {
	value = std::clamp(value, 0.0, 1.0);
	return srgb_inverse_eotf(value);
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
	if (transfer == TransferCharacteristics::Gamma22) return std::pow(value, 1.0 / 2.2);
	if (transfer == TransferCharacteristics::Gamma28) return std::pow(value, 1.0 / 2.8);
	return value < 0.018 ? 4.5 * value : 1.099 * std::pow(value, 0.45) - 0.099;
}

struct Rgb {
	double r = 0.0;
	double g = 0.0;
	double b = 0.0;
};

Rgb multiply(const double matrix[9], Rgb value) {
	return {
		.r=matrix[0] * value.r + matrix[1] * value.g + matrix[2] * value.b,
		.g=matrix[3] * value.r + matrix[4] * value.g + matrix[5] * value.b,
		.b=matrix[6] * value.r + matrix[7] * value.g + matrix[8] * value.b,
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

constexpr double SDR_REFERENCE_WHITE_NITS = 203.0;
constexpr double PQ_MAX_NITS = 10000.0;

double hlg_system_gamma(double peakNits) {
	return std::clamp(1.2 + 0.42 * std::log10(std::max(peakNits, 1.0) / 1000.0), 1.0, 1.5);
}

Rgb signal_rgb_to_nits(
	Rgb signal,
	TransferCharacteristics transfer,
	double kr,
	double kb,
	double hlgPeakNits
) {
	if (transfer != TransferCharacteristics::HLG) {
		const double scale = transfer == TransferCharacteristics::PQ ? PQ_MAX_NITS : SDR_REFERENCE_WHITE_NITS;
		return {
			.r=transfer_to_linear(signal.r, transfer) * scale,
			.g=transfer_to_linear(signal.g, transfer) * scale,
			.b=transfer_to_linear(signal.b, transfer) * scale,
		};
	}
	const Rgb scene{
		.r=transfer_to_linear(signal.r, transfer),
		.g=transfer_to_linear(signal.g, transfer),
		.b=transfer_to_linear(signal.b, transfer),
	};
	const double kg = 1.0 - kr - kb;
	const double sceneLuma = std::max(0.0, kr * scene.r + kg * scene.g + kb * scene.b);
	if (sceneLuma == 0.0) return {};
	const double scale = hlgPeakNits * std::pow(sceneLuma, hlg_system_gamma(hlgPeakNits) - 1.0);
	return {.r=scene.r * scale, .g=scene.g * scale, .b=scene.b * scale};
}

Rgb nits_rgb_to_signal(
	Rgb nits,
	TransferCharacteristics transfer,
	double kr,
	double kb,
	double hlgPeakNits
) {
	if (transfer != TransferCharacteristics::HLG) {
		const double scale = transfer == TransferCharacteristics::PQ ? PQ_MAX_NITS : SDR_REFERENCE_WHITE_NITS;
		return {
			.r=linear_to_transfer(nits.r / scale, transfer),
			.g=linear_to_transfer(nits.g / scale, transfer),
			.b=linear_to_transfer(nits.b / scale, transfer),
		};
	}
	const double kg = 1.0 - kr - kb;
	const double displayLuma = std::max(0.0, kr * nits.r + kg * nits.g + kb * nits.b);
	if (displayLuma == 0.0) return {};
	const double sceneLuma = std::pow(displayLuma / hlgPeakNits, 1.0 / hlg_system_gamma(hlgPeakNits));
	const double scale = hlgPeakNits * std::pow(sceneLuma, hlg_system_gamma(hlgPeakNits) - 1.0);
	return {
		.r=linear_to_transfer(nits.r / scale, transfer),
		.g=linear_to_transfer(nits.g / scale, transfer),
		.b=linear_to_transfer(nits.b / scale, transfer),
	};
}

double rgb_sample_to_signal(const RawImage& image, uint32_t sample) {
	const int depth = bit_depth(image.format);
	if (image.color.range == ColorRange::Full) {
		return dequantize_unorm(sample, static_cast<uint32_t>(max_sample_value(image.format)));
	}
	const auto scale = static_cast<double>(1u << (depth - 8));
	return std::clamp((static_cast<double>(sample) - 16.0 * scale) / (219.0 * scale), 0.0, 1.0);
}

Rgb display_srgb(const RawImage& image, Rgb signal) {
	if (image.color.transfer == TransferCharacteristics::Unspecified ||
	    image.color.primaries == ColorPrimaries::Unspecified) {
		return signal;
	}
	double kr = 0.2126;
	double kb = 0.0722;
	primaries_luma_coefficients(image.color.primaries, kr, kb);
	const double hlgPeak = image.nominalPeakNits.value_or(1000.0);
	Rgb nits = signal_rgb_to_nits(signal, image.color.transfer, kr, kb, hlgPeak);
	nits = multiply(
		xyz_to_rgb_matrix(ColorPrimaries::BT709),
		multiply(rgb_to_xyz_matrix(image.color.primaries), nits)
	);
	return {
		.r=linear_to_srgb(nits.r / SDR_REFERENCE_WHITE_NITS),
		.g=linear_to_srgb(nits.g / SDR_REFERENCE_WHITE_NITS),
		.b=linear_to_srgb(nits.b / SDR_REFERENCE_WHITE_NITS),
	};
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
	const Rgb display = display_srgb(image, {.r=r, .g=g, .b=b});
	r = display.r * 255.0;
	g = display.g * 255.0;
	b = display.b * 255.0;
}

uint32_t source_sample(const RawImage& image, int plane, int x, int y) {
	const int sampleBytes = bytes_per_sample(image.format);
	const int depth = bit_depth(image.format);
	const uint32_t scale = 1u << (depth - 8);
	const uint32_t defaultValue = plane == 0 ? 16u * scale : 128u * scale;
	return sample_or_default(image.planes[plane], x, y, sampleBytes, defaultValue);
}

} // namespace

std::array<double,9> linear_bt2020_matrix(ColorPrimaries primaries) {
	// H.273 chromaticities; non-D65 whites are Bradford-adapted to D65.
	// Constants derived from the standards' xy coordinates (also tabulated
	// in FFmpeg libavutil/csp.c), without a display RGB conversion.
	std::array<double,9> primary{};
	switch(primaries) {
		case ColorPrimaries::BT709:
		case ColorPrimaries::BT2020:
		case ColorPrimaries::DisplayP3: std::copy_n(rgb_to_xyz_matrix(primaries),9,primary.begin());break;
		case ColorPrimaries::BT470M: primary={0.59898823694389935,0.16676361429603212,0.18470407581173995,0.29609802386054196,0.59329372296638228,0.11060825317307572,-0.00014058163411941716,0.063999778543494909,1.0251985538505031}; break;
		case ColorPrimaries::BT470BG: primary={0.43055381332990234,0.34154980353055336,0.17835231019121595,0.22200430999823090,0.70665476592528287,0.071340924076486373,0.020182209999839152,0.12955337375296852,0.93932216700707061}; break;
		case ColorPrimaries::BT601_525: primary={0.39352090365938974,0.36525807671760352,0.19167694667467830,0.21237636070506749,0.70105985692572281,0.086563782369209571,0.018739090650447113,0.11193392673603975,0.95838473337339147}; break;
		case ColorPrimaries::Film: primary={0.53430846685194056,0.23025656759971272,0.18589089260001812,0.25082149720897579,0.68585422692867282,0.063324275862351093,-0.00021457397181764477,0.062230784093153378,1.0270415406385429}; break;
		case ColorPrimaries::DCIP3: primary={0.45925165499198695,0.29579178750572255,0.19541248455396212,0.21515051246430858,0.70913363664987572,0.075715850885815658,0.00027200554373124446,0.046939514088582136,1.0418462311275651}; break;
		case ColorPrimaries::XYZ: primary={1,0,0,0,1,0,0,0,1};break;
		default: throw std::invalid_argument("metric input primaries are unspecified or unsupported");
	}
	const double* sourceMatrix = primary.data();
	const double* targetMatrix = xyz_to_rgb_matrix(ColorPrimaries::BT2020);
	if (primaries == ColorPrimaries::BT2020) return {1,0,0,0,1,0,0,0,1};
	// Compose once; no intermediate RGB gamut or quantized image exists.
	std::array<double,9> matrix{};
	for(int r=0;r<3;++r) for(int c=0;c<3;++c) for(int k=0;k<3;++k)
		matrix[r*3+c] += targetMatrix[r*3+k]*sourceMatrix[k*3+c];
	return matrix;
}

LinearBt2020Image raw_image_to_linear_bt2020(const RawImage& image) {
	if (image.width <= 0 || image.height <= 0) throw std::invalid_argument("invalid metric image dimensions");
	for (int p=0;p<plane_count(image.format);++p)
		if (!plane_available(image,p)) throw std::invalid_argument("metric input plane is incomplete");
	if (image.color.transfer == TransferCharacteristics::Unspecified)
		throw std::invalid_argument("metric input transfer is unspecified");
	const auto matrix=linear_bt2020_matrix(image.color.primaries);
	const bool nativePrimaries=image.color.primaries==ColorPrimaries::BT2020;
	auto eotf = [&](double v) {
		const double a=std::abs(v);
		double result=0;
		switch(image.color.transfer) {
			case TransferCharacteristics::Linear: return v;
			case TransferCharacteristics::SRGB: return srgb_eotf(v);
			case TransferCharacteristics::Gamma22: result=std::pow(a,2.2); break;
			case TransferCharacteristics::Gamma28: result=std::pow(a,2.8); break;
			case TransferCharacteristics::PQ: {
				const double p=std::pow(a,1.0/78.84375);
				const double denominator=18.8515625-18.6875*p;
				if (denominator<=0) throw std::invalid_argument("PQ signal outside the finite EOTF domain");
				result=std::pow(std::max(p-0.8359375,0.0)/denominator,1.0/0.1593017578125)*10000.0/203.0; break;
			}
			case TransferCharacteristics::HLG: result=a<=0.5?a*a/3.0:(std::exp((a-0.55991073)/0.17883277)+0.28466892)/12.0; break;
			case TransferCharacteristics::BT709:
			case TransferCharacteristics::BT601:
			case TransferCharacteristics::BT2020_10:
			case TransferCharacteristics::BT2020_12: result=a<0.081?a/4.5:std::pow((a+0.099)/1.099,1.0/0.45); break;
			default: throw std::invalid_argument("unsupported metric input transfer");
		}
		return std::copysign(result,v);
	};
	LinearBt2020Image out{.width=image.width,.height=image.height};
	for(auto& p:out.planes) p.resize(std::size_t(image.width)*image.height);
	const int bps=bytes_per_sample(image.format);
	const auto scale=yuv_scale(image);
	double kr=0,kb=0;
	if (!is_rgb(image.format) && !is_gray(image.format)) {
		if (image.color.matrix==MatrixCoefficients::BT601) {kr=0.299;kb=0.114;}
		else if(image.color.matrix==MatrixCoefficients::BT709 || image.color.matrix==MatrixCoefficients::BT2020NonConstant)
			matrix_coefficients(image.color.matrix,kr,kb);
		else throw std::invalid_argument("unsupported metric input YUV matrix");
	}
	const double maximum=max_sample_value(image.format);
	// Bilinear, location-aware chroma reconstruction in the original signal
	// domain. Avoid nearest-neighbour expansion or an 8-bit display conversion.
	auto chroma = [&](int p,int x,int y) {
		const double sx=(is_420(image.format)||is_422(image.format))?2.0:1.0;
		const double sy=is_420(image.format)?2.0:1.0;
		double ox=0,oy=is_420(image.format)?0.5:0;
		if(is_420(image.format)) {
			const auto loc=image.color.chroma420Location.value_or(Chroma420SampleLocation::LeftCenter);
			ox=(loc==Chroma420SampleLocation::Center || loc==Chroma420SampleLocation::TopCenter || loc==Chroma420SampleLocation::BottomCenter)?0.5:0;
			if(loc==Chroma420SampleLocation::TopLeft || loc==Chroma420SampleLocation::TopCenter) oy=0;
			if(loc==Chroma420SampleLocation::BottomLeft || loc==Chroma420SampleLocation::BottomCenter) oy=1;
		}
		const double fx=(x-ox)/sx,fy=(y-oy)/sy;
		const int ix=std::floor(fx),iy=std::floor(fy);
		auto at=[&](int xx,int yy) {return double(sample_at(image.planes[p],std::clamp(xx,0,plane_width(image,p)-1),std::clamp(yy,0,plane_height(image,p)-1),bps));};
		const double tx=fx-ix,ty=fy-iy;
		return (1-ty)*((1-tx)*at(ix,iy)+tx*at(ix+1,iy))+ty*((1-tx)*at(ix,iy+1)+tx*at(ix+1,iy+1));
	};
	for(int y=0;y<image.height;++y) for(int x=0;x<image.width;++x) {
		Rgb signal;
		if(is_rgb(image.format)) {
			auto channel=[&](int p) {
				const double v=sample_at(image.planes[p],x,y,bps);
				return image.color.range==ColorRange::Full?v/maximum:(v-scale.yOffset)/scale.yScale;
			};
			signal={channel(0),channel(1),channel(2)};
		} else {
			const double yp=(double(sample_at(image.planes[0],x,y,bps))-scale.yOffset)/scale.yScale;
			signal={yp,yp,yp};
			if(!is_gray(image.format)) {
				const double cb=(chroma(1,x,y)-scale.cOffset)/scale.cScale;
				const double cr=(chroma(2,x,y)-scale.cOffset)/scale.cScale;
				signal.r=yp+2*(1-kr)*cr; signal.b=yp+2*(1-kb)*cb;
				signal.g=(yp-kr*signal.r-kb*signal.b)/(1-kr-kb);
			}
		}
		Rgb linear{eotf(signal.r),eotf(signal.g),eotf(signal.b)};
		if(image.color.transfer==TransferCharacteristics::HLG) {
			const double peak=image.nominalPeakNits.value_or(1000.0);
			double lr=0,lb=0;primaries_luma_coefficients(image.color.primaries,lr,lb);
			const double luma=lr*linear.r+(1-lr-lb)*linear.g+lb*linear.b;
			const double ootf=peak/203.0*std::pow(std::abs(luma),hlg_system_gamma(peak)-1.0);
			linear={linear.r*ootf,linear.g*ootf,linear.b*ootf};
		}
		if(!nativePrimaries) linear=multiply(matrix.data(),linear);
		const std::size_t i=std::size_t(y)*image.width+x;
		out.planes[0][i]=linear.r;out.planes[1][i]=linear.g;out.planes[2][i]=linear.b;
		for(int p=0;p<3;++p) if(!std::isfinite(out.planes[p][i])) throw std::invalid_argument("non-finite linear metric input");
	}
	return out;
}

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
			for (int y = 0; y < image.height; ++y) {
				for (int x = 0; x < image.width; ++x) {
					const Rgb display = display_srgb(image, {
						.r=rgb_sample_to_signal(image, sample_or_default(image.planes[0], x, y, sampleBytes, 0)),
						.g=rgb_sample_to_signal(image, sample_or_default(image.planes[1], x, y, sampleBytes, 0)),
						.b=rgb_sample_to_signal(image, sample_or_default(image.planes[2], x, y, sampleBytes, 0)),
					});
					store(
						x,
						y,
						clamp_byte(display.r * 255.0),
						clamp_byte(display.g * 255.0),
						clamp_byte(display.b * 255.0)
					);
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
			const auto maxSample = static_cast<double>(max_sample_value(image.format));
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

ComparisonImages normalize_comparison(const RawImage& reference, const RawImage& candidate) {
	if (reference.width != candidate.width || reference.height != candidate.height ||
	    reference.width <= 0 || reference.height <= 0) {
		throw std::invalid_argument("image dimensions differ or are invalid");
	}
	for (const RawImage* image : {&reference, &candidate}) {
		for (int plane = 0; plane < plane_count(image->format); ++plane) {
			if (!plane_available(*image, plane)) throw std::invalid_argument("plane buffer is incomplete");
		}
	}
	if (reference.color.primaries != candidate.color.primaries ||
	    reference.color.transfer != candidate.color.transfer ||
	    (reference.color.transfer == TransferCharacteristics::HLG && reference.nominalPeakNits != candidate.nominalPeakNits)) {
		throw std::invalid_argument("color descriptions differ; an explicit comparison color transform is required");
	}
	const bool refRgb = is_rgb(reference.format);
	const bool candRgb = is_rgb(candidate.format);
	if (!refRgb && !candRgb && (reference.color.matrix != candidate.color.matrix ||
	    reference.color.range != candidate.color.range ||
	    ((is_420(reference.format) && is_420(candidate.format)) &&
	     reference.color.chroma420Location != candidate.color.chroma420Location))) {
		throw std::invalid_argument("color descriptions differ; an explicit comparison color transform is required");
	}
	ColorDescription color = refRgb ? candidate.color : reference.color;
	if (refRgb && candRgb) {
		color.matrix = color.primaries == ColorPrimaries::BT2020 ? MatrixCoefficients::BT2020NonConstant : MatrixCoefficients::BT709;
		color.range = ColorRange::Full;
	}
	color.chroma420Location.reset();
	const PixelFormat format = pixel_format_for(std::max(bit_depth(reference.format), bit_depth(candidate.format)), true);
	auto normalize = [&](const RawImage& image) {
		RawImage out;
		if (is_rgb(image.format)) {
			ColorTransformOptions options;
			options.target = color;
			out = transform_raw_image(image, format, options);
		} else {
			// Limited-range code values scale by powers of two: 8-bit white
			// 235 is 10-bit 940, not round(235 * 1023 / 255) == 943.
			if (image.color.range == ColorRange::Limited && bit_depth(image.format) < bit_depth(format)) {
				const RawImage planes = convert_raw_image_format(image, pixel_format_for(bit_depth(image.format), true));
				out = convert_raw_image_format(planes, format);
				const int shift = bit_depth(format) - bit_depth(image.format);
				for (int plane = 0; plane < 3; ++plane) {
					for (int y = 0; y < image.height; ++y) {
						for (int x = 0; x < image.width; ++x) {
							put_sample(out.planes[plane], x, y, bytes_per_sample(format),
							           sample_at(planes.planes[plane], x, y, bytes_per_sample(planes.format)) << shift,
							           max_sample_value(format));
						}
					}
				}
			} else {
				out = convert_raw_image_format(image, format);
			}
		}
		out.color.chroma420Location.reset();
		return out;
	};
	return {.reference=normalize(reference), .candidate=normalize(candidate)};
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
	out.pixelAspectRatio = image.pixelAspectRatio;
	out.nominalPeakNits = image.nominalPeakNits;
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
	    image.color.range == options.target.range &&
	    options.toneMap == ToneMapMode::None) {
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
	out.pixelAspectRatio = image.pixelAspectRatio;
	const bool sameLightEncoding =
		image.color.primaries == options.target.primaries &&
		image.color.transfer == options.target.transfer &&
		options.toneMap == ToneMapMode::None;
	if (out.color.transfer == TransferCharacteristics::HLG) {
		out.nominalPeakNits = sameLightEncoding
			? image.nominalPeakNits.value_or(options.sourcePeakNits)
			: options.targetPeakNits;
	} else {
		out.nominalPeakNits.reset();
	}
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
	double sourceLumaKr = 0.0;
	double sourceLumaKb = 0.0;
	double targetLumaKr = 0.0;
	double targetLumaKb = 0.0;
	primaries_luma_coefficients(image.color.primaries, sourceLumaKr, sourceLumaKb);
	primaries_luma_coefficients(options.target.primaries, targetLumaKr, targetLumaKb);
	const int sourceBps = bytes_per_sample(image.format);
	const uint32_t neutral = 1u << (bit_depth(image.format) - 1);
	const double sourceHlgPeak = image.nominalPeakNits.value_or(options.sourcePeakNits);
	const double targetHlgPeak = options.targetPeakNits;

	for (int y = 0; y < out.height; ++y) {
		for (int x = 0; x < out.width; ++x) {
			double rp = 0.0;
			double gp = 0.0;
			double bp = 0.0;
			if (is_rgb(image.format)) {
				rp = rgb_sample_to_signal(image, sample_or_default(image.planes[0], x, y, sourceBps, 0));
				gp = rgb_sample_to_signal(image, sample_or_default(image.planes[1], x, y, sourceBps, 0));
				bp = rgb_sample_to_signal(image, sample_or_default(image.planes[2], x, y, sourceBps, 0));
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
			Rgb signal{.r=rp, .g=gp, .b=bp};
			if (!sameLightEncoding) {
				Rgb linear = signal_rgb_to_nits(
					signal,
					image.color.transfer,
					sourceLumaKr,
					sourceLumaKb,
					sourceHlgPeak
				);
				linear = multiply(xyz_to_rgb_matrix(options.target.primaries), multiply(rgb_to_xyz_matrix(image.color.primaries), linear));
				if (options.toneMap != ToneMapMode::None) {
					auto map = [&](double value) {
						const double normalized = std::max(0.0, value / options.targetPeakNits);
						return options.targetPeakNits * (options.toneMap == ToneMapMode::Clip ? std::min(1.0, normalized) : normalized / (1.0 + normalized));
					};
					linear = {.r=map(linear.r), .g=map(linear.g), .b=map(linear.b)};
				}
				signal = nits_rgb_to_signal(
					linear,
					options.target.transfer,
					targetLumaKr,
					targetLumaKb,
					targetHlgPeak
				);
			}
			const double yp = targetKr * signal.r + targetKg * signal.g + targetKb * signal.b;
			const std::size_t index = static_cast<std::size_t>(y) * out.width + x;
			targetY[index] = targetScale.yOffset + yp * targetScale.yScale;
			targetCb[index] = targetScale.cOffset + ((signal.b - yp) / (2.0 - 2.0 * targetKb)) * targetScale.cScale;
			targetCr[index] = targetScale.cOffset + ((signal.r - yp) / (2.0 - 2.0 * targetKr)) * targetScale.cScale;
		}
	}
	auto target_sample = [&](double value) {
		return quantize_sample(value, targetMaximum);
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
