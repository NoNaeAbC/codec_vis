#include "codec_gui_x265.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <filesystem>
#include <iterator>
#include <memory>
#include <stdexcept>
#include <string>
#include <sstream>
#include <type_traits>
#include <vector>

#include <charls/charls_jpegls_encoder.h>
#include <jxl/encode.h>
#include <openjpeg.h>
#include <png.h>
#include <x264.h>
#include <zlib.h>

extern "C" {
#include <jpeglib.h>
#include <jxrlib/JXRGlue.h>
#include <jxrlib/windowsmediaphoto.h>
}

#ifdef min
#undef min
#endif
#ifdef max
#undef max
#endif

namespace codec_gui {
namespace {

template <typename T>
T param_value(std::span<const EncoderParam> params, const std::string& name, T fallback) {
	for (const EncoderParam& param : params) {
		if (param.name == name) {
			if (const auto* value = std::get_if<T>(&param.value)) {
				return *value;
			}
		}
	}
	return fallback;
}

bool is_gray(PixelFormat format) {
	return format == PixelFormat::Gray8 || format == PixelFormat::Gray10LE ||
	       format == PixelFormat::Gray12LE || format == PixelFormat::Gray14LE;
}

bool is_420(PixelFormat format) {
	return format == PixelFormat::YUV420P8 || format == PixelFormat::YUV420P10LE ||
	       format == PixelFormat::YUV420P12LE || format == PixelFormat::YUV420P14LE;
}

bool is_422(PixelFormat format) {
	return format == PixelFormat::YUV422P8 || format == PixelFormat::YUV422P10LE ||
	       format == PixelFormat::YUV422P12LE || format == PixelFormat::YUV422P14LE;
}

int format_bit_depth(PixelFormat format) {
	switch (format) {
		case PixelFormat::YUV420P8:
		case PixelFormat::YUV422P8:
		case PixelFormat::YUV444P8:
		case PixelFormat::Gray8: return 8;
		case PixelFormat::YUV420P10LE:
		case PixelFormat::YUV422P10LE:
		case PixelFormat::YUV444P10LE:
		case PixelFormat::Gray10LE: return 10;
		case PixelFormat::YUV420P12LE:
		case PixelFormat::YUV422P12LE:
		case PixelFormat::YUV444P12LE:
		case PixelFormat::Gray12LE: return 12;
		case PixelFormat::YUV420P14LE:
		case PixelFormat::YUV422P14LE:
		case PixelFormat::YUV444P14LE:
		case PixelFormat::Gray14LE: return 14;
		case PixelFormat::YUV444P16LE: return 16;
		default: break;
	}
	throw std::invalid_argument("unsupported raw pixel format");
}

std::string value_to_cli_string(const ParamValue& value) {
	return std::visit(
		[](const auto& v) -> std::string {
			using T = std::decay_t<decltype(v)>;
			if constexpr (std::is_same_v<T, bool>) {
				return v ? "1" : "0";
			} else if constexpr (std::is_same_v<T, int64_t>) {
				return std::to_string(v);
			} else if constexpr (std::is_same_v<T, double>) {
				std::ostringstream oss;
				oss << v;
				return oss.str();
			} else {
				return v;
			}
		},
		value
	);
}

uint16_t load_sample(const ImagePlane& plane, int x, int y, int bytesPerSample, uint16_t fallback) {
	if (x < 0 || y < 0 || plane.strideBytes <= 0) {
		return fallback;
	}
	const std::size_t offset = static_cast<std::size_t>(y) * static_cast<std::size_t>(plane.strideBytes) +
	                           static_cast<std::size_t>(x) * static_cast<std::size_t>(bytesPerSample);
	if (offset + static_cast<std::size_t>(bytesPerSample) > plane.bytes.size()) {
		return fallback;
	}
	if (bytesPerSample == 1) {
		return plane.bytes[offset];
	}
	return static_cast<uint16_t>(plane.bytes[offset] | (static_cast<uint16_t>(plane.bytes[offset + 1]) << 8u));
}

struct MatrixWeights {
	double kr;
	double kb;
};

MatrixWeights matrix_weights(MatrixCoefficients matrix) {
	switch (matrix) {
		case MatrixCoefficients::BT709: return {0.2126, 0.0722};
		case MatrixCoefficients::BT601: return {0.2990, 0.1140};
		case MatrixCoefficients::BT2020NonConstant: return {0.2627, 0.0593};
		default:
			throw std::invalid_argument("JPEG XL RGB conversion does not support the selected YCbCr matrix; select BT.709, BT.601, or BT.2020 non-constant explicitly");
	}
}

void validate_jxl_plane(const RawImage& image, int plane, int width, int height, int bytesPerSample) {
	const ImagePlane& source = image.planes[plane];
	if (source.strideBytes < width * bytesPerSample ||
	    source.bytes.size() < static_cast<std::size_t>(source.strideBytes) * static_cast<std::size_t>(height)) {
		throw std::invalid_argument("JPEG XL input plane " + std::to_string(plane) + " is missing or shorter than its declared dimensions");
	}
}

double sample_plane_bilinear(const ImagePlane& plane, double x, double y, int width, int height, int bytesPerSample) {
	x = std::clamp(x, 0.0, static_cast<double>(std::max(0, width - 1)));
	y = std::clamp(y, 0.0, static_cast<double>(std::max(0, height - 1)));
	const int x0 = static_cast<int>(std::floor(x));
	const int y0 = static_cast<int>(std::floor(y));
	const int x1 = std::min(x0 + 1, width - 1);
	const int y1 = std::min(y0 + 1, height - 1);
	const double tx = x - x0;
	const double ty = y - y0;
	const double a = load_sample(plane, x0, y0, bytesPerSample, 0);
	const double b = load_sample(plane, x1, y0, bytesPerSample, 0);
	const double c = load_sample(plane, x0, y1, bytesPerSample, 0);
	const double d = load_sample(plane, x1, y1, bytesPerSample, 0);
	return (a + (b - a) * tx) + ((c + (d - c) * tx) - (a + (b - a) * tx)) * ty;
}

struct JxlInputPixels {
	std::vector<uint8_t> bytes;
	int bitDepth = 8;
	uint32_t channels = 3;
	JxlDataType dataType = JXL_TYPE_UINT8;
};

JxlInputPixels raw_to_jxl_pixels(const RawImage& image) {
	if (image.width <= 0 || image.height <= 0) {
		throw std::invalid_argument("JPEG XL image dimensions must be positive");
	}
	JxlInputPixels result;
	result.bitDepth = format_bit_depth(image.format);
	result.channels = is_gray(image.format) ? 1u : 3u;
	result.dataType = result.bitDepth == 8 ? JXL_TYPE_UINT8 : JXL_TYPE_UINT16;
	const int bytesPerSample = result.bitDepth == 8 ? 1 : 2;
	const int chromaWidth = (is_420(image.format) || is_422(image.format)) ? (image.width + 1) / 2 : image.width;
	const int chromaHeight = is_420(image.format) ? (image.height + 1) / 2 : image.height;
	validate_jxl_plane(image, 0, image.width, image.height, bytesPerSample);
	if (!is_gray(image.format)) {
		validate_jxl_plane(image, 1, chromaWidth, chromaHeight, bytesPerSample);
		validate_jxl_plane(image, 2, chromaWidth, chromaHeight, bytesPerSample);
	}

	const double maximum = static_cast<double>((1u << result.bitDepth) - 1u);
	const double scale = static_cast<double>(1u << (result.bitDepth - 8));
	const double yOffset = image.color.range == ColorRange::Full ? 0.0 : 16.0 * scale;
	const double yScale = image.color.range == ColorRange::Full ? maximum : 219.0 * scale;
	const double cOffset = static_cast<double>(1u << (result.bitDepth - 1));
	const double cScale = image.color.range == ColorRange::Full ? maximum : 224.0 * scale;
	const MatrixWeights weights = is_gray(image.format) ? MatrixWeights{0.2126, 0.0722} : matrix_weights(image.color.matrix);
	const double kg = 1.0 - weights.kr - weights.kb;
	result.bytes.resize(static_cast<std::size_t>(image.width) * static_cast<std::size_t>(image.height) * result.channels * bytesPerSample);

	auto store = [&](std::size_t sampleIndex, double normalized) {
		const uint16_t value = static_cast<uint16_t>(std::clamp<long>(std::lround(normalized * maximum), 0, static_cast<long>(maximum)));
		if (bytesPerSample == 1) {
			result.bytes[sampleIndex] = static_cast<uint8_t>(value);
		} else {
			const std::size_t offset = sampleIndex * 2;
			result.bytes[offset] = static_cast<uint8_t>(value & 0xffu);
			result.bytes[offset + 1] = static_cast<uint8_t>(value >> 8u);
		}
	};

	for (int y = 0; y < image.height; ++y) {
		for (int x = 0; x < image.width; ++x) {
			const double yy = (static_cast<double>(load_sample(image.planes[0], x, y, bytesPerSample, 0)) - yOffset) / yScale;
			const std::size_t pixel = static_cast<std::size_t>(y) * image.width + x;
			if (is_gray(image.format)) {
				store(pixel, yy);
				continue;
			}

			double chromaX = static_cast<double>(x);
			double chromaY = static_cast<double>(y);
			if (is_420(image.format) || is_422(image.format)) {
				const Chroma420SampleLocation location = image.color.chroma420Location.value_or(Chroma420SampleLocation::LeftCenter);
				const bool horizontallyCentered = location == Chroma420SampleLocation::Center ||
				                                  location == Chroma420SampleLocation::TopCenter ||
				                                  location == Chroma420SampleLocation::BottomCenter;
				chromaX = (static_cast<double>(x) - (horizontallyCentered ? 0.5 : 0.0)) * 0.5;
				if (is_420(image.format)) {
					if (location == Chroma420SampleLocation::TopLeft || location == Chroma420SampleLocation::TopCenter) chromaY = static_cast<double>(y) * 0.5;
					else if (location == Chroma420SampleLocation::BottomLeft || location == Chroma420SampleLocation::BottomCenter) chromaY = (static_cast<double>(y) - 1.0) * 0.5;
					else chromaY = (static_cast<double>(y) - 0.5) * 0.5;
				}
			}
			const double cb = (sample_plane_bilinear(image.planes[1], chromaX, chromaY, chromaWidth, chromaHeight, bytesPerSample) - cOffset) / cScale;
			const double cr = (sample_plane_bilinear(image.planes[2], chromaX, chromaY, chromaWidth, chromaHeight, bytesPerSample) - cOffset) / cScale;
			const double r = yy + (2.0 - 2.0 * weights.kr) * cr;
			const double b = yy + (2.0 - 2.0 * weights.kb) * cb;
			const double g = (yy - weights.kr * r - weights.kb * b) / kg;
			store(pixel * 3 + 0, r);
			store(pixel * 3 + 1, g);
			store(pixel * 3 + 2, b);
		}
	}
	return result;
}

JxlColorEncoding jxl_color_encoding(const RawImage& image) {
	JxlColorEncoding color{};
	color.color_space = is_gray(image.format) ? JXL_COLOR_SPACE_GRAY : JXL_COLOR_SPACE_RGB;
	color.white_point = JXL_WHITE_POINT_D65;
	color.primaries = JXL_PRIMARIES_SRGB;
	color.rendering_intent = JXL_RENDERING_INTENT_RELATIVE;
	if (!is_gray(image.format)) {
		switch (image.color.primaries) {
			case ColorPrimaries::BT709: color.primaries = JXL_PRIMARIES_SRGB; break;
			case ColorPrimaries::BT2020: color.primaries = JXL_PRIMARIES_2100; break;
			case ColorPrimaries::DisplayP3: color.primaries = JXL_PRIMARIES_P3; break;
			case ColorPrimaries::DCIP3:
				color.primaries = JXL_PRIMARIES_P3;
				color.white_point = JXL_WHITE_POINT_DCI;
				break;
			default:
				throw std::invalid_argument("JPEG XL cannot represent the selected primaries as a structured color profile; select BT.709, Display P3, or BT.2020 explicitly");
		}
	}
	switch (image.color.transfer) {
		case TransferCharacteristics::BT709:
		case TransferCharacteristics::BT601:
		case TransferCharacteristics::BT2020_10:
		case TransferCharacteristics::BT2020_12: color.transfer_function = JXL_TRANSFER_FUNCTION_709; break;
		case TransferCharacteristics::SRGB: color.transfer_function = JXL_TRANSFER_FUNCTION_SRGB; break;
		case TransferCharacteristics::Linear: color.transfer_function = JXL_TRANSFER_FUNCTION_LINEAR; break;
		case TransferCharacteristics::PQ: color.transfer_function = JXL_TRANSFER_FUNCTION_PQ; break;
		case TransferCharacteristics::HLG: color.transfer_function = JXL_TRANSFER_FUNCTION_HLG; break;
		case TransferCharacteristics::Gamma22:
			color.transfer_function = JXL_TRANSFER_FUNCTION_GAMMA;
			color.gamma = 1.0 / 2.2;
			break;
		case TransferCharacteristics::Gamma28:
			color.transfer_function = JXL_TRANSFER_FUNCTION_GAMMA;
			color.gamma = 1.0 / 2.8;
			break;
		default:
			throw std::invalid_argument("JPEG XL requires explicit supported transfer characteristics; select sRGB, BT.709, Linear, PQ, or HLG");
	}
	return color;
}

std::vector<std::byte> bytes_from_u8(const std::vector<uint8_t>& in) {
	std::vector<std::byte> out(in.size());
	std::memcpy(out.data(), in.data(), in.size());
	return out;
}

std::vector<uint8_t> read_temp_file(const std::filesystem::path& path) {
	std::ifstream in(path, std::ios::binary);
	if (!in) {
		throw std::runtime_error("failed to read temporary encoded file: " + path.string());
	}
	return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

struct X264InputFormat {
	int csp;
	int bitDepth;
};

X264InputFormat x264_input_format(PixelFormat format) {
	switch (format) {
		case PixelFormat::YUV420P8: return {X264_CSP_I420, 8};
		case PixelFormat::YUV420P10LE: return {X264_CSP_I420 | X264_CSP_HIGH_DEPTH, 10};
		case PixelFormat::YUV422P8: return {X264_CSP_I422, 8};
		case PixelFormat::YUV422P10LE: return {X264_CSP_I422 | X264_CSP_HIGH_DEPTH, 10};
		case PixelFormat::YUV444P8: return {X264_CSP_I444, 8};
		case PixelFormat::YUV444P10LE: return {X264_CSP_I444 | X264_CSP_HIGH_DEPTH, 10};
		default: throw std::invalid_argument("x264 requires planar 8- or 10-bit YUV 4:2:0, 4:2:2, or 4:4:4 input");
	}
}

std::string automatic_x264_profile(const RawImage& image, bool lossless) {
	if (lossless || image.format == PixelFormat::YUV444P8 || image.format == PixelFormat::YUV444P10LE) return "high444";
	if (is_422(image.format)) return "high422";
	if (format_bit_depth(image.format) == 10) return "high10";
	return "high";
}

EncodedImage encode_jpeg_like(const RawImage& image, std::span<const EncoderParam> params) {
	const JxlInputPixels rgb = raw_to_jxl_pixels(image);
	if (rgb.bitDepth != 8 && rgb.bitDepth != 12) {
		throw std::invalid_argument("libjpeg DCT encoding supports 8-bit or 12-bit RGB input");
	}
	const int quality = static_cast<int>(param_value<int64_t>(params, "quality", 50));
	jpeg_compress_struct cinfo{};
	jpeg_error_mgr jerr{};
	cinfo.err = jpeg_std_error(&jerr);
	jpeg_create_compress(&cinfo);
	unsigned char* mem = nullptr;
	unsigned long memSize = 0;
	jpeg_mem_dest(&cinfo, &mem, &memSize);
	cinfo.image_width = static_cast<JDIMENSION>(image.width);
	cinfo.image_height = static_cast<JDIMENSION>(image.height);
	cinfo.input_components = 3;
	cinfo.in_color_space = JCS_RGB;
	jpeg_set_defaults(&cinfo);
	cinfo.data_precision = rgb.bitDepth;
	jpeg_set_quality(&cinfo, std::clamp(quality, 1, 100), rgb.bitDepth == 8 ? TRUE : FALSE);
	cinfo.optimize_coding = param_value<bool>(params, "optimize", true) ? TRUE : FALSE;
	cinfo.arith_code = param_value<bool>(params, "arithmetic", false) ? TRUE : FALSE;
	cinfo.smoothing_factor = static_cast<int>(param_value<int64_t>(params, "smoothing", 0));
	cinfo.restart_in_rows = static_cast<int>(param_value<int64_t>(params, "restart-rows", 0));
	const std::string dct = param_value<std::string>(params, "dct", "islow");
	if (dct == "ifast") cinfo.dct_method = JDCT_IFAST;
	else if (dct == "float") cinfo.dct_method = JDCT_FLOAT;
	else cinfo.dct_method = JDCT_ISLOW;
	if (param_value<bool>(params, "progressive", false)) {
		jpeg_simple_progression(&cinfo);
	}
	jpeg_start_compress(&cinfo, TRUE);
	while (cinfo.next_scanline < cinfo.image_height) {
		if (rgb.bitDepth == 8) {
			JSAMPROW row = const_cast<JSAMPROW>(
				rgb.bytes.data() + static_cast<std::size_t>(cinfo.next_scanline) * image.width * 3
			);
			jpeg_write_scanlines(&cinfo, &row, 1);
		} else {
			J12SAMPROW row = reinterpret_cast<J12SAMPROW>(
				const_cast<uint8_t*>(rgb.bytes.data()) +
				static_cast<std::size_t>(cinfo.next_scanline) * image.width * 3 * 2
			);
			jpeg12_write_scanlines(&cinfo, &row, 1);
		}
	}
	jpeg_finish_compress(&cinfo);
	std::vector<std::byte> bytes(memSize);
	std::memcpy(bytes.data(), mem, memSize);
	std::free(mem);
	jpeg_destroy_compress(&cinfo);
	EncodedImage encoded;
	encoded.hevcAnnexB = std::move(bytes);
	encoded.codedColor = image.color;
	return encoded;
}

void png_write_callback(png_structp png, png_bytep data, png_size_t length) {
	auto* out = static_cast<std::vector<std::byte>*>(png_get_io_ptr(png));
	const auto* first = reinterpret_cast<const std::byte*>(data);
	out->insert(out->end(), first, first + length);
}

} // namespace

std::vector<EncoderParamInfo> query_jpegls_parameters() {
	return {
		{.name = "near", .label = "NEAR", .group = "Coding", .kind = ParamKind::Int, .defaultValue = int64_t{0}, .intRange = IntRange{0, 127, 1}, .help = "JPEG-LS near-lossless error tolerance. 0 is lossless; 127 is valid at every exposed precision."},
		{.name = "color-transform", .label = "Color transform", .group = "Coding", .kind = ParamKind::Enum, .defaultValue = std::string{"none"}, .enumValues = {{"none", "None"}, {"hp1", "HP1"}, {"hp2", "HP2"}, {"hp3", "HP3"}}, .help = "Optional HP reversible color transform. Not part of baseline JPEG-LS interchange."},
		{.name = "even-size", .label = "Even size", .group = "Bitstream", .kind = ParamKind::Bool, .defaultValue = false, .help = "Pad the destination to even byte size."},
		{.name = "version-comment", .label = "Version comment", .group = "Bitstream", .kind = ParamKind::Bool, .defaultValue = false, .help = "Include CharLS version comment segment."},
		{.name = "jai-pc-parameters", .label = "JAI PC params", .group = "Bitstream", .kind = ParamKind::Bool, .defaultValue = false, .help = "Include JAI-compatible preset coding parameters."},
	};
}

EncodedImage encode_jpegls_still_image(const RawImage& image, std::span<const EncoderParam> params) {
	const JxlInputPixels rgb = raw_to_jxl_pixels(image);
	charls::jpegls_encoder encoder;
	encoder.frame_info({static_cast<uint32_t>(image.width), static_cast<uint32_t>(image.height), rgb.bitDepth, 3})
		.interleave_mode(charls::interleave_mode::sample)
		.near_lossless(static_cast<int32_t>(param_value<int64_t>(params, "near", 0)));
	const std::string transform = param_value<std::string>(params, "color-transform", "none");
	if (transform == "hp1") encoder.color_transformation(charls::color_transformation::hp1);
	else if (transform == "hp2") encoder.color_transformation(charls::color_transformation::hp2);
	else if (transform == "hp3") encoder.color_transformation(charls::color_transformation::hp3);
	charls::encoding_options options = charls::encoding_options::none;
	if (param_value<bool>(params, "even-size", false)) options |= charls::encoding_options::even_destination_size;
	if (param_value<bool>(params, "version-comment", false)) options |= charls::encoding_options::include_version_number;
	if (param_value<bool>(params, "jai-pc-parameters", false)) options |= charls::encoding_options::include_pc_parameters_jai;
	encoder.encoding_options(options);
	std::vector<uint8_t> out(encoder.estimated_destination_size());
	encoder.destination(out);
	const std::size_t written = encoder.encode(
		rgb.bytes, static_cast<uint32_t>(image.width * 3 * (rgb.bitDepth == 8 ? 1 : 2))
	);
	out.resize(written);
	EncodedImage encoded;
	encoded.hevcAnnexB = bytes_from_u8(out);
	encoded.codedColor = image.color;
	return encoded;
}

std::vector<EncoderParamInfo> query_jpeg_parameters() {
	return {
		{.name = "quality", .label = "Quality", .group = "Rate Control", .kind = ParamKind::Int, .defaultValue = int64_t{50}, .intRange = IntRange{1, 100, 1}, .help = "JPEG quantization quality."},
		{.name = "optimize", .label = "Optimize Huffman", .group = "Entropy", .kind = ParamKind::Bool, .defaultValue = true, .help = "Optimize Huffman tables."},
		{.name = "arithmetic", .label = "Arithmetic", .group = "Entropy", .kind = ParamKind::Bool, .defaultValue = false, .help = "Use arithmetic coding instead of Huffman if supported."},
		{.name = "progressive", .label = "Progressive", .group = "Scan", .kind = ParamKind::Bool, .defaultValue = false, .help = "Emit progressive JPEG scans."},
		{.name = "smoothing", .label = "Smoothing", .group = "Preprocess", .kind = ParamKind::Int, .defaultValue = int64_t{0}, .intRange = IntRange{0, 100, 1}, .help = "Input smoothing factor."},
		{.name = "restart-rows", .label = "Restart rows", .group = "Bitstream", .kind = ParamKind::Int, .defaultValue = int64_t{0}, .intRange = IntRange{0, 1024, 1}, .help = "Restart interval in MCU rows. 0 disables restart markers."},
		{.name = "dct", .label = "DCT", .group = "Transform", .kind = ParamKind::Enum, .defaultValue = std::string{"islow"}, .enumValues = {{"islow", "Integer accurate"}, {"ifast", "Integer fast"}, {"float", "Float"}}, .help = "Forward DCT implementation."},
	};
}

EncodedImage encode_jpeg_still_image(const RawImage& image, std::span<const EncoderParam> params) {
	return encode_jpeg_like(image, params);
}

std::vector<EncoderParamInfo> query_jpeg2000_parameters() {
	return {
		{.name = "quality", .label = "Quality", .group = "Rate Control", .kind = ParamKind::Int, .defaultValue = int64_t{50}, .intRange = IntRange{1, 100, 1}, .help = "Approximate compression quality. Ignored when Lossless is enabled."},
		{.name = "lossless", .label = "Lossless", .group = "Rate Control", .kind = ParamKind::Bool, .defaultValue = false, .help = "Use reversible 5-3 wavelet and lossless final layer."},
		{.name = "num-resolutions", .label = "Resolutions", .group = "Wavelet", .kind = ParamKind::Int, .defaultValue = int64_t{6}, .intRange = IntRange{1, 33, 1}, .help = "Number of DWT resolution levels."},
		{.name = "codeblock-size", .label = "Codeblock size", .group = "Codeblocks", .kind = ParamKind::Enum, .defaultValue = std::string{"64"}, .enumValues = {{"4", "4×4"}, {"8", "8×8"}, {"16", "16×16"}, {"32", "32×32"}, {"64", "64×64"}}, .help = "Square JPEG 2000 codeblocks satisfying the power-of-two and 4096-sample limits."},
		{.name = "progression", .label = "Progression", .group = "Ordering", .kind = ParamKind::Enum, .defaultValue = std::string{"LRCP"}, .enumValues = {{"LRCP", "LRCP"}, {"RLCP", "RLCP"}, {"RPCL", "RPCL"}, {"PCRL", "PCRL"}, {"CPRL", "CPRL"}}, .help = "JPEG 2000 progression order."},
		{.name = "mct", .label = "MCT", .group = "Color", .kind = ParamKind::Bool, .defaultValue = true, .help = "Multiple component transform for RGB."},
		{.name = "tile-width", .label = "Tile W", .group = "Tiling", .kind = ParamKind::Int, .defaultValue = int64_t{0}, .intRange = IntRange{0, 65535, 1}, .help = "0 disables tiling; otherwise tile width."},
		{.name = "tile-height", .label = "Tile H", .group = "Tiling", .kind = ParamKind::Int, .defaultValue = int64_t{0}, .intRange = IntRange{0, 65535, 1}, .help = "0 disables tiling; otherwise tile height."},
	};
}

EncodedImage encode_jpeg2000_still_image(const RawImage& image, std::span<const EncoderParam> encoderParams) {
	const JxlInputPixels rgb = raw_to_jxl_pixels(image);
	opj_cparameters_t params{};
	opj_set_default_encoder_parameters(&params);
	params.tcp_numlayers = 1;
	const bool lossless = param_value<bool>(encoderParams, "lossless", false);
	const int quality = static_cast<int>(param_value<int64_t>(encoderParams, "quality", 50));
	const int clampedQuality = std::clamp(quality, 1, 100);
	params.tcp_rates[0] = lossless ? 1.0f : static_cast<float>(101 - clampedQuality);
	params.cp_disto_alloc = 1;
	params.irreversible = lossless ? 0 : 1;
	const int requestedResolutions = static_cast<int>(param_value<int64_t>(encoderParams, "num-resolutions", 6));
	const int minDimension = std::max(1, std::min(image.width, image.height));
	int maxResolutions = 1;
	while (maxResolutions < 33 && (1 << maxResolutions) <= minDimension) {
		++maxResolutions;
	}
	params.numresolution = std::clamp(requestedResolutions, 1, maxResolutions);
	const int codeblockSize = std::stoi(param_value<std::string>(encoderParams, "codeblock-size", "64"));
	params.cblockw_init = codeblockSize;
	params.cblockh_init = codeblockSize;
	params.tcp_mct = param_value<bool>(encoderParams, "mct", true) ? 1 : 0;
	const std::string progression = param_value<std::string>(encoderParams, "progression", "LRCP");
	if (progression == "RLCP") params.prog_order = OPJ_RLCP;
	else if (progression == "RPCL") params.prog_order = OPJ_RPCL;
	else if (progression == "PCRL") params.prog_order = OPJ_PCRL;
	else if (progression == "CPRL") params.prog_order = OPJ_CPRL;
	else params.prog_order = OPJ_LRCP;
	const int tileWidth = static_cast<int>(param_value<int64_t>(encoderParams, "tile-width", 0));
	const int tileHeight = static_cast<int>(param_value<int64_t>(encoderParams, "tile-height", 0));
	if (tileWidth > 0 && tileHeight > 0) {
		params.tile_size_on = OPJ_TRUE;
		params.cp_tdx = tileWidth;
		params.cp_tdy = tileHeight;
	}
	opj_image_cmptparm_t cmpt[3]{};
	for (auto& c : cmpt) {
		c.dx = 1;
		c.dy = 1;
		c.w = static_cast<OPJ_UINT32>(image.width);
		c.h = static_cast<OPJ_UINT32>(image.height);
		c.prec = static_cast<OPJ_UINT32>(rgb.bitDepth);
		c.sgnd = 0;
	}
	std::unique_ptr<opj_image_t, decltype(&opj_image_destroy)> ojImage(opj_image_create(3, cmpt, OPJ_CLRSPC_SRGB), opj_image_destroy);
	if (!ojImage) throw std::runtime_error("OpenJPEG image allocation failed");
	ojImage->x1 = static_cast<OPJ_UINT32>(image.width);
	ojImage->y1 = static_cast<OPJ_UINT32>(image.height);
	for (int y = 0; y < image.height; ++y) {
		for (int x = 0; x < image.width; ++x) {
			const std::size_t src = (static_cast<std::size_t>(y) * image.width + x) * 3;
			const std::size_t dst = static_cast<std::size_t>(y) * image.width + x;
			auto sample = [&](std::size_t index) {
				if (rgb.bitDepth == 8) return static_cast<int>(rgb.bytes[index]);
				const std::size_t offset = index * 2;
				return static_cast<int>(rgb.bytes[offset] | (static_cast<uint16_t>(rgb.bytes[offset + 1]) << 8u));
			};
			ojImage->comps[0].data[dst] = sample(src + 0);
			ojImage->comps[1].data[dst] = sample(src + 1);
			ojImage->comps[2].data[dst] = sample(src + 2);
		}
	}
	std::filesystem::path path = std::filesystem::temp_directory_path() / "codec_vis_tmp.jp2";
	std::unique_ptr<opj_codec_t, decltype(&opj_destroy_codec)> codec(opj_create_compress(OPJ_CODEC_JP2), opj_destroy_codec);
	if (!codec || !opj_setup_encoder(codec.get(), &params, ojImage.get())) throw std::runtime_error("OpenJPEG encoder setup failed");
	std::unique_ptr<opj_stream_t, decltype(&opj_stream_destroy)> stream(opj_stream_create_default_file_stream(path.c_str(), OPJ_FALSE), opj_stream_destroy);
	if (!stream || !opj_start_compress(codec.get(), ojImage.get(), stream.get()) || !opj_encode(codec.get(), stream.get()) || !opj_end_compress(codec.get(), stream.get())) {
		throw std::runtime_error("OpenJPEG encode failed");
	}
	const std::vector<uint8_t> file = read_temp_file(path);
	std::filesystem::remove(path);
	EncodedImage encoded;
	encoded.hevcAnnexB = bytes_from_u8(file);
	encoded.codedColor = image.color;
	return encoded;
}

std::vector<EncoderParamInfo> query_jpegxl_parameters() {
	std::vector<EncoderParamInfo> result = {
		{.name = "rate-control", .label = "Rate control", .group = "Coding", .kind = ParamKind::Enum, .defaultValue = std::string{"quality"}, .enumValues = {{"quality", "Quality"}, {"distance", "Butteraugli distance"}, {"lossless", "Lossless"}}, .help = "Select exactly one JPEG XL rate-control mode. The inactive controls are disabled and are not sent to libjxl."},
		{.name = "quality", .label = "Quality", .group = "Coding", .kind = ParamKind::Int, .defaultValue = int64_t{90}, .intRange = IntRange{1, 100, 1}, .help = "JPEG-style quality mapped by libjxl to distance. Quality 90 corresponds to distance 1.0 (visually lossless); quality 100 is distance 0 but is still not mathematically lossless.", .enabledWhen = {{"rate-control", {"quality"}, "Quality is used only in Quality rate-control mode."}}},
		{.name = "distance", .label = "Butteraugli distance", .group = "Coding", .kind = ParamKind::Float, .defaultValue = double{1.0}, .floatRange = FloatRange{0.0, 25.0, 0.1}, .help = "Direct libjxl distance target. Lower is higher quality; 1.0 is visually lossless and 0 alone is not true lossless.", .enabledWhen = {{"rate-control", {"distance"}, "Distance is used only in Butteraugli distance mode."}}},
		{.name = "effort", .label = "Effort", .group = "Coding", .kind = ParamKind::Int, .defaultValue = int64_t{7}, .intRange = IntRange{1, 10, 1}, .help = "JPEG XL encoder effort."},
		{.name = "decoding-speed", .label = "Decoding speed", .group = "Coding", .kind = ParamKind::Int, .defaultValue = int64_t{0}, .intRange = IntRange{0, 4, 1}, .help = "Decode speed tier. Higher can reduce density."},
		{.name = "resampling", .label = "Resampling", .group = "Coding", .kind = ParamKind::Int, .defaultValue = int64_t{-1}, .intRange = IntRange{-1, 8, 1}, .help = "-1 default, 1 none, 2/4/8 downsample before compression."},
		{.name = "extra-channel-resampling", .label = "Extra resampling", .group = "Coding", .kind = ParamKind::Int, .defaultValue = int64_t{-1}, .intRange = IntRange{-1, 8, 1}, .help = "Extra-channel resampling. Has no effect for RGB-only input."},
		{.name = "already-downsampled", .label = "Already downsampled", .group = "Coding", .kind = ParamKind::Int, .defaultValue = int64_t{0}, .intRange = IntRange{0, 1, 1}, .help = "Tell libjxl that the provided frame is already downsampled by Resampling."},
		{.name = "photon-noise", .label = "Photon noise", .group = "Tools", .kind = ParamKind::Int, .defaultValue = int64_t{0}, .intRange = IntRange{0, 3200, 1}, .help = "Adds synthetic photographic noise."},
		{.name = "noise", .label = "Adaptive noise", .group = "Tools", .kind = ParamKind::Int, .defaultValue = int64_t{-1}, .intRange = IntRange{-1, 1, 1}, .help = "Legacy adaptive noise generation toggle. Prefer Photon noise."},
		{.name = "modular", .label = "Modular", .group = "Tools", .kind = ParamKind::Int, .defaultValue = int64_t{-1}, .intRange = IntRange{-1, 1, 1}, .help = "-1 default, 0 VarDCT, 1 modular."},
		{.name = "epf", .label = "EPF", .group = "Tools", .kind = ParamKind::Int, .defaultValue = int64_t{-1}, .intRange = IntRange{-1, 3, 1}, .help = "Edge preserving filter level."},
		{.name = "gaborish", .label = "Gaborish", .group = "Tools", .kind = ParamKind::Int, .defaultValue = int64_t{-1}, .intRange = IntRange{-1, 1, 1}, .help = "Gaborish filter toggle."},
		{.name = "dots", .label = "Dots", .group = "Tools", .kind = ParamKind::Int, .defaultValue = int64_t{-1}, .intRange = IntRange{-1, 1, 1}, .help = "Dots generation toggle."},
		{.name = "patches", .label = "Patches", .group = "Tools", .kind = ParamKind::Int, .defaultValue = int64_t{-1}, .intRange = IntRange{-1, 1, 1}, .help = "Patch generation toggle."},
		{.name = "keep-invisible", .label = "Keep invisible", .group = "Tools", .kind = ParamKind::Int, .defaultValue = int64_t{-1}, .intRange = IntRange{-1, 1, 1}, .help = "Preserve invisible pixel color. Relevant only with alpha."},
		{.name = "palette-colors", .label = "Palette colors", .group = "Tools", .kind = ParamKind::Int, .defaultValue = int64_t{-1}, .intRange = IntRange{-1, 70913, 1}, .help = "Palette color limit. -1 default."},
		{.name = "lossy-palette", .label = "Lossy palette", .group = "Tools", .kind = ParamKind::Int, .defaultValue = int64_t{-1}, .intRange = IntRange{-1, 1, 1}, .help = "Palette quantization toggle."},
		{.name = "color-transform", .label = "Color transform", .group = "Tools", .kind = ParamKind::Int, .defaultValue = int64_t{-1}, .intRange = IntRange{-1, 2, 1}, .help = "Automatic uses libjxl's choice; 0 is XYB, 1 preserves RGB, and 2 marks losslessly represented YCbCr values."},
		{.name = "channel-colors-global-percent", .label = "Global palette %", .group = "Tools", .kind = ParamKind::Int, .defaultValue = int64_t{-1}, .intRange = IntRange{-1, 100, 1}, .help = "Global channel palette threshold for modular encoding."},
		{.name = "channel-colors-group-percent", .label = "Group palette %", .group = "Tools", .kind = ParamKind::Int, .defaultValue = int64_t{-1}, .intRange = IntRange{-1, 100, 1}, .help = "Per-group channel palette threshold for modular encoding."},
		{.name = "modular-color-space", .label = "Modular RCT", .group = "Modular", .kind = ParamKind::Int, .defaultValue = int64_t{-1}, .intRange = IntRange{-1, 41, 1}, .help = "Reversible color transform index for modular encoding."},
		{.name = "modular-group-size", .label = "Modular group", .group = "Modular", .kind = ParamKind::Int, .defaultValue = int64_t{-1}, .intRange = IntRange{-1, 3, 1}, .help = "-1 default, 0/1/2/3 = 128/256/512/1024."},
		{.name = "modular-predictor", .label = "Modular predictor", .group = "Modular", .kind = ParamKind::Int, .defaultValue = int64_t{-1}, .intRange = IntRange{-1, 15, 1}, .help = "Predictor for modular encoding."},
		{.name = "modular-ma-tree-learning-percent", .label = "MA tree learn %", .group = "Modular", .kind = ParamKind::Int, .defaultValue = int64_t{-1}, .intRange = IntRange{-1, 200, 1}, .help = "Percentage of pixels used for MA tree learning."},
		{.name = "modular-nb-prev-channels", .label = "Prev channels", .group = "Modular", .kind = ParamKind::Int, .defaultValue = int64_t{-1}, .intRange = IntRange{-1, 11, 1}, .help = "Previous-channel properties for modular MA trees."},
		{.name = "progressive-ac", .label = "Progressive AC", .group = "Progressive", .kind = ParamKind::Int, .defaultValue = int64_t{-1}, .intRange = IntRange{-1, 1, 1}, .help = "Progressive AC toggle."},
		{.name = "qprogressive-ac", .label = "Q progressive AC", .group = "Progressive", .kind = ParamKind::Int, .defaultValue = int64_t{-1}, .intRange = IntRange{-1, 1, 1}, .help = "Quantized progressive AC toggle."},
		{.name = "progressive-dc", .label = "Progressive DC", .group = "Progressive", .kind = ParamKind::Int, .defaultValue = int64_t{-1}, .intRange = IntRange{-1, 2, 1}, .help = "Progressive DC mode."},
		{.name = "group-order", .label = "Group order", .group = "Progressive", .kind = ParamKind::Int, .defaultValue = int64_t{-1}, .intRange = IntRange{-1, 1, 1}, .help = "-1 default, 0 scanline, 1 center-first."},
		{.name = "group-order-center-x", .label = "Center X", .group = "Progressive", .kind = ParamKind::Int, .defaultValue = int64_t{-1}, .intRange = IntRange{-1, 65535, 1}, .help = "Center-first group order X position."},
		{.name = "group-order-center-y", .label = "Center Y", .group = "Progressive", .kind = ParamKind::Int, .defaultValue = int64_t{-1}, .intRange = IntRange{-1, 65535, 1}, .help = "Center-first group order Y position."},
		{.name = "responsive", .label = "Responsive", .group = "Progressive", .kind = ParamKind::Int, .defaultValue = int64_t{-1}, .intRange = IntRange{-1, 1, 1}, .help = "Progressive encoding for modular mode."},
		{.name = "jpeg-recon-cfl", .label = "JPEG recon CFL", .group = "JPEG", .kind = ParamKind::Int, .defaultValue = int64_t{-1}, .intRange = IntRange{-1, 1, 1}, .help = "CFL toggle for lossless JPEG recompression. No effect for RGB input frames."},
		{.name = "brotli-effort", .label = "Brotli effort", .group = "Container", .kind = ParamKind::Int, .defaultValue = int64_t{-1}, .intRange = IntRange{-1, 11, 1}, .help = "Effort for compressed boxes. -1 default."},
		{.name = "jpeg-compress-boxes", .label = "JPEG boxes", .group = "JPEG", .kind = ParamKind::Int, .defaultValue = int64_t{-1}, .intRange = IntRange{-1, 1, 1}, .help = "Compress metadata boxes derived from JPEG input. No effect for RGB input frames."},
		{.name = "buffering", .label = "Buffering", .group = "Container", .kind = ParamKind::Int, .defaultValue = int64_t{-1}, .intRange = IntRange{-1, 3, 1}, .help = "Chunked frame buffering mode."},
		{.name = "full-image-heuristics", .label = "Full image heuristics", .group = "Heuristics", .kind = ParamKind::Int, .defaultValue = int64_t{-1}, .intRange = IntRange{-1, 1, 1}, .help = "Use full image heuristics toggle."},
		{.name = "disable-perceptual-heuristics", .label = "Disable perceptual", .group = "Heuristics", .kind = ParamKind::Int, .defaultValue = int64_t{0}, .intRange = IntRange{0, 1, 1}, .help = "Disable perceptual heuristics."},
	};
	// These libjxl switches only affect alpha/extra channels, JPEG
	// reconstruction, or box metadata. This backend supplies a plain RGB/gray
	// frame, so showing them would imply capabilities they cannot affect.
	for (const std::string_view irrelevant : {
		     "extra-channel-resampling", "already-downsampled", "keep-invisible",
		     "jpeg-recon-cfl", "brotli-effort", "jpeg-compress-boxes"
	     }) {
		result.erase(std::remove_if(result.begin(), result.end(), [&](const EncoderParamInfo& parameter) {
			return parameter.name == irrelevant;
		}), result.end());
	}
	auto make_enum = [&](std::string_view name, std::string defaultValue, std::vector<EnumValue> values) {
		auto parameter = std::find_if(result.begin(), result.end(), [&](const EncoderParamInfo& item) { return item.name == name; });
		if (parameter == result.end()) return;
		parameter->kind = ParamKind::Enum;
		parameter->defaultValue = std::move(defaultValue);
		parameter->intRange.reset();
		parameter->enumValues = std::move(values);
	};
	make_enum("resampling", "-1", {{"-1", "Automatic"}, {"1", "None"}, {"2", "2×"}, {"4", "4×"}, {"8", "8×"}});
	make_enum("noise", "-1", {{"-1", "Automatic"}, {"0", "Off"}, {"1", "On"}});
	make_enum("modular", "-1", {{"-1", "Automatic"}, {"0", "VarDCT"}, {"1", "Modular"}});
	make_enum("epf", "-1", {{"-1", "Automatic"}, {"0", "Off"}, {"1", "Level 1"}, {"2", "Level 2"}, {"3", "Level 3"}});
	make_enum("gaborish", "-1", {{"-1", "Automatic"}, {"0", "Off"}, {"1", "On"}});
	make_enum("dots", "-1", {{"-1", "Automatic"}, {"0", "Off"}, {"1", "On"}});
	make_enum("patches", "-1", {{"-1", "Automatic"}, {"0", "Off"}, {"1", "On"}});
	make_enum("lossy-palette", "-1", {{"-1", "Automatic"}, {"0", "Off"}, {"1", "On"}});
	make_enum("color-transform", "-1", {{"-1", "Automatic"}, {"0", "XYB"}, {"1", "RGB"}});
	make_enum("progressive-ac", "-1", {{"-1", "Automatic"}, {"0", "Off"}, {"1", "On"}});
	make_enum("qprogressive-ac", "-1", {{"-1", "Automatic"}, {"0", "Off"}, {"1", "On"}});
	make_enum("group-order", "-1", {{"-1", "Automatic"}, {"0", "Scanline"}, {"1", "Center first"}});
	make_enum("responsive", "-1", {{"-1", "Automatic"}, {"0", "Off"}, {"1", "On"}});
	make_enum("full-image-heuristics", "-1", {{"-1", "Automatic"}, {"0", "Off"}, {"1", "On"}});
	for (EncoderParamInfo& parameter : result) {
		if (parameter.name == "color-transform") {
			parameter.help = "Internal libjxl color transform. Automatic lets libjxl choose; XYB is the perceptual transform and RGB preserves RGB channels. YCbCr is intentionally unavailable because this backend supplies RGB/gray samples.";
		}
		if (parameter.name == "resampling") {
			parameter.help = "Optional whole-image downsampling inside libjxl. Only valid factors are offered; this is not chroma subsampling.";
		}
		if (parameter.name == "group-order-center-x" || parameter.name == "group-order-center-y") {
			parameter.enabledWhen = {{"group-order", {"1"}, "Center coordinates apply only to Center-first group order."}};
		}
		if (parameter.group == "Modular") {
			parameter.enabledWhen = {{"modular", {"1"}, "This option applies only when Modular encoding is selected explicitly."}};
		}
		if (parameter.name == "responsive") {
			parameter.enabledWhen = {{"modular", {"1"}, "Responsive progression applies only to Modular encoding."}};
		}
	}
	for (EncoderParamInfo& parameter : result) {
		if (parameter.kind == ParamKind::Int && std::get_if<int64_t>(&parameter.defaultValue) != nullptr &&
		    *std::get_if<int64_t>(&parameter.defaultValue) == -1) {
			parameter.automaticIntValue = -1;
			parameter.automaticLabel = "Automatic";
		}
	}
	return result;
}

EncodedImage encode_jpegxl_still_image(const RawImage& image, std::span<const EncoderParam> params) {
	const JxlInputPixels pixels = raw_to_jxl_pixels(image);
	const std::string rateControl = param_value<std::string>(params, "rate-control", "quality");
	if (rateControl != "quality" && rateControl != "distance" && rateControl != "lossless") {
		throw std::invalid_argument("unknown JPEG XL rate-control mode: " + rateControl);
	}
	// Keep accepting the former boolean for command-line callers, but the GUI
	// exposes a single mutually-exclusive rate-control selector.
	const bool lossless = rateControl == "lossless" || param_value<bool>(params, "lossless", false);
	std::unique_ptr<JxlEncoder, decltype(&JxlEncoderDestroy)> enc(JxlEncoderCreate(nullptr), JxlEncoderDestroy);
	if (!enc) throw std::runtime_error("JxlEncoderCreate failed");
	auto encoder_error = [&](const std::string& operation) {
		return std::runtime_error(operation + " failed (libjxl encoder error " + std::to_string(static_cast<int>(JxlEncoderGetError(enc.get()))) + ")");
	};
	JxlBasicInfo info{};
	JxlEncoderInitBasicInfo(&info);
	info.xsize = static_cast<uint32_t>(image.width);
	info.ysize = static_cast<uint32_t>(image.height);
	info.bits_per_sample = static_cast<uint32_t>(pixels.bitDepth);
	info.exponent_bits_per_sample = 0;
	info.num_color_channels = pixels.channels;
	info.uses_original_profile = lossless ? JXL_TRUE : JXL_FALSE;
	if (JxlEncoderSetBasicInfo(enc.get(), &info) != JXL_ENC_SUCCESS) throw encoder_error("JxlEncoderSetBasicInfo");
	const JxlColorEncoding color = jxl_color_encoding(image);
	if (JxlEncoderSetColorEncoding(enc.get(), &color) != JXL_ENC_SUCCESS) throw encoder_error("JxlEncoderSetColorEncoding");
	JxlEncoderFrameSettings* frame = JxlEncoderFrameSettingsCreate(enc.get(), nullptr);
	if (frame == nullptr) throw encoder_error("JxlEncoderFrameSettingsCreate");
	auto setOption = [&](const std::string& name, JxlEncoderFrameSettingId option, int64_t fallback) {
		int64_t resolved = fallback;
		for (const EncoderParam& parameter : params) {
			if (parameter.name != name) continue;
			if (const auto* integer = std::get_if<int64_t>(&parameter.value)) resolved = *integer;
			else if (const auto* boolean = std::get_if<bool>(&parameter.value)) resolved = *boolean ? 1 : 0;
			else if (const auto* text = std::get_if<std::string>(&parameter.value)) {
				try {
					std::size_t consumed = 0;
					resolved = std::stoll(*text, &consumed);
					if (consumed != text->size()) throw std::invalid_argument("trailing characters");
				} catch (const std::exception&) {
					throw std::invalid_argument("JPEG XL option '" + name + "' has a non-numeric selection: " + *text);
				}
			}
			break;
		}
		const int value = static_cast<int>(resolved);
		if (value == fallback) {
			return;
		}
		if (JxlEncoderFrameSettingsSetOption(frame, option, value) != JXL_ENC_SUCCESS) {
			throw encoder_error("JPEG XL option '" + name + "' with value " + std::to_string(value));
		}
	};
	setOption("effort", JXL_ENC_FRAME_SETTING_EFFORT, 7);
	setOption("decoding-speed", JXL_ENC_FRAME_SETTING_DECODING_SPEED, 0);
	setOption("resampling", JXL_ENC_FRAME_SETTING_RESAMPLING, -1);
	setOption("extra-channel-resampling", JXL_ENC_FRAME_SETTING_EXTRA_CHANNEL_RESAMPLING, -1);
	setOption("already-downsampled", JXL_ENC_FRAME_SETTING_ALREADY_DOWNSAMPLED, 0);
	setOption("photon-noise", JXL_ENC_FRAME_SETTING_PHOTON_NOISE, 0);
	setOption("noise", JXL_ENC_FRAME_SETTING_NOISE, -1);
	setOption("modular", JXL_ENC_FRAME_SETTING_MODULAR, -1);
	setOption("epf", JXL_ENC_FRAME_SETTING_EPF, -1);
	setOption("gaborish", JXL_ENC_FRAME_SETTING_GABORISH, -1);
	setOption("dots", JXL_ENC_FRAME_SETTING_DOTS, -1);
	setOption("patches", JXL_ENC_FRAME_SETTING_PATCHES, -1);
	setOption("keep-invisible", JXL_ENC_FRAME_SETTING_KEEP_INVISIBLE, -1);
	setOption("palette-colors", JXL_ENC_FRAME_SETTING_PALETTE_COLORS, -1);
	setOption("lossy-palette", JXL_ENC_FRAME_SETTING_LOSSY_PALETTE, -1);
	setOption("color-transform", JXL_ENC_FRAME_SETTING_COLOR_TRANSFORM, -1);
	setOption("channel-colors-global-percent", JXL_ENC_FRAME_SETTING_CHANNEL_COLORS_GLOBAL_PERCENT, -1);
	setOption("channel-colors-group-percent", JXL_ENC_FRAME_SETTING_CHANNEL_COLORS_GROUP_PERCENT, -1);
	setOption("modular-color-space", JXL_ENC_FRAME_SETTING_MODULAR_COLOR_SPACE, -1);
	setOption("modular-group-size", JXL_ENC_FRAME_SETTING_MODULAR_GROUP_SIZE, -1);
	setOption("modular-predictor", JXL_ENC_FRAME_SETTING_MODULAR_PREDICTOR, -1);
	setOption("modular-ma-tree-learning-percent", JXL_ENC_FRAME_SETTING_MODULAR_MA_TREE_LEARNING_PERCENT, -1);
	setOption("modular-nb-prev-channels", JXL_ENC_FRAME_SETTING_MODULAR_NB_PREV_CHANNELS, -1);
	setOption("progressive-ac", JXL_ENC_FRAME_SETTING_PROGRESSIVE_AC, -1);
	setOption("qprogressive-ac", JXL_ENC_FRAME_SETTING_QPROGRESSIVE_AC, -1);
	setOption("progressive-dc", JXL_ENC_FRAME_SETTING_PROGRESSIVE_DC, -1);
	setOption("group-order", JXL_ENC_FRAME_SETTING_GROUP_ORDER, -1);
	setOption("group-order-center-x", JXL_ENC_FRAME_SETTING_GROUP_ORDER_CENTER_X, -1);
	setOption("group-order-center-y", JXL_ENC_FRAME_SETTING_GROUP_ORDER_CENTER_Y, -1);
	setOption("responsive", JXL_ENC_FRAME_SETTING_RESPONSIVE, -1);
	setOption("jpeg-recon-cfl", JXL_ENC_FRAME_SETTING_JPEG_RECON_CFL, -1);
	setOption("brotli-effort", JXL_ENC_FRAME_SETTING_BROTLI_EFFORT, -1);
	setOption("jpeg-compress-boxes", JXL_ENC_FRAME_SETTING_JPEG_COMPRESS_BOXES, -1);
	setOption("buffering", JXL_ENC_FRAME_SETTING_BUFFERING, -1);
	setOption("full-image-heuristics", JXL_ENC_FRAME_SETTING_USE_FULL_IMAGE_HEURISTICS, -1);
	setOption("disable-perceptual-heuristics", JXL_ENC_FRAME_SETTING_DISABLE_PERCEPTUAL_HEURISTICS, 0);
	if (JxlEncoderSetFrameLossless(frame, lossless ? JXL_TRUE : JXL_FALSE) != JXL_ENC_SUCCESS) {
		throw encoder_error("JxlEncoderSetFrameLossless");
	}
	if (lossless) {
		if (JxlEncoderSetFrameDistance(frame, 0.0f) != JXL_ENC_SUCCESS) {
			throw encoder_error("JxlEncoderSetFrameDistance(0)");
		}
	} else {
		const float distance = rateControl == "distance"
			? static_cast<float>(param_value<double>(params, "distance", 1.0))
			: JxlEncoderDistanceFromQuality(static_cast<float>(param_value<int64_t>(params, "quality", 90)));
		if (JxlEncoderSetFrameDistance(frame, distance) != JXL_ENC_SUCCESS) {
			throw encoder_error("JxlEncoderSetFrameDistance(" + std::to_string(distance) + ")");
		}
	}
	if (pixels.bitDepth > 8) {
		const JxlBitDepth inputDepth{JXL_BIT_DEPTH_FROM_CODESTREAM, 0, 0};
		if (JxlEncoderSetFrameBitDepth(frame, &inputDepth) != JXL_ENC_SUCCESS) throw encoder_error("JxlEncoderSetFrameBitDepth");
	}
	const JxlPixelFormat format{pixels.channels, pixels.dataType, JXL_LITTLE_ENDIAN, 0};
	if (JxlEncoderAddImageFrame(frame, &format, pixels.bytes.data(), pixels.bytes.size()) != JXL_ENC_SUCCESS) throw encoder_error("JxlEncoderAddImageFrame");
	JxlEncoderCloseInput(enc.get());
	std::vector<std::byte> out(4096);
	uint8_t* next = reinterpret_cast<uint8_t*>(out.data());
	size_t avail = out.size();
	for (;;) {
		const JxlEncoderStatus status = JxlEncoderProcessOutput(enc.get(), &next, &avail);
		if (status == JXL_ENC_SUCCESS) break;
		if (status != JXL_ENC_NEED_MORE_OUTPUT) throw encoder_error("JxlEncoderProcessOutput");
		const std::size_t used = out.size() - avail;
		out.resize(out.size() * 2);
		next = reinterpret_cast<uint8_t*>(out.data()) + used;
		avail = out.size() - used;
	}
	out.resize(out.size() - avail);
	EncodedImage encoded;
	encoded.hevcAnnexB = std::move(out);
	encoded.codedColor = image.color;
	return encoded;
}

std::vector<EncoderParamInfo> query_jpegxr_parameters() {
	return {
		{.name = "qp", .label = "QP", .group = "Quantization", .kind = ParamKind::Int, .defaultValue = int64_t{32}, .intRange = IntRange{1, 255, 1}, .help = "JPEG XR base quantizer index. Lower is higher quality."},
		{.name = "qp-ylp", .label = "QP Y LP", .group = "Quantization", .kind = ParamKind::Int, .defaultValue = int64_t{0}, .intRange = IntRange{0, 255, 1}, .help = "Luma low-pass quantizer. 0 lets the encoder derive it."},
		{.name = "qp-yhp", .label = "QP Y HP", .group = "Quantization", .kind = ParamKind::Int, .defaultValue = int64_t{0}, .intRange = IntRange{0, 255, 1}, .help = "Luma high-pass quantizer. 0 lets the encoder derive it."},
		{.name = "qp-u", .label = "QP U", .group = "Quantization", .kind = ParamKind::Int, .defaultValue = int64_t{0}, .intRange = IntRange{0, 255, 1}, .help = "U channel quantizer. 0 lets the encoder derive it."},
		{.name = "qp-v", .label = "QP V", .group = "Quantization", .kind = ParamKind::Int, .defaultValue = int64_t{0}, .intRange = IntRange{0, 255, 1}, .help = "V channel quantizer. 0 lets the encoder derive it."},
		{.name = "overlap", .label = "Overlap", .group = "Transform", .kind = ParamKind::Enum, .defaultValue = std::string{"one"}, .enumValues = {{"none", "None"}, {"one", "One"}, {"two", "Two"}}, .help = "JPEG XR overlap filtering."},
		{.name = "bitstream-order", .label = "Order", .group = "Bitstream", .kind = ParamKind::Enum, .defaultValue = std::string{"spatial"}, .enumValues = {{"spatial", "Spatial"}, {"frequency", "Frequency"}}, .help = "Spatial or frequency bitstream order."},
		{.name = "subband", .label = "Subband", .group = "Bitstream", .kind = ParamKind::Enum, .defaultValue = std::string{"all"}, .enumValues = {{"all", "All"}, {"no-flexbits", "No flexbits"}, {"no-highpass", "No highpass"}, {"dc-only", "DC only"}}, .help = "Subbands retained in the codestream."},
		{.name = "trim-flex-bits", .label = "Trim flexbits", .group = "Bitstream", .kind = ParamKind::Int, .defaultValue = int64_t{0}, .intRange = IntRange{0, 15, 1}, .help = "Number of flex bits to trim."},
		{.name = "hard-tiles", .label = "Hard tiles", .group = "Tiling", .kind = ParamKind::Bool, .defaultValue = false, .help = "Use hard tile boundaries."},
		{.name = "tiles-x", .label = "Tiles X", .group = "Tiling", .kind = ParamKind::Int, .defaultValue = int64_t{1}, .intRange = IntRange{1, 16, 1}, .help = "Number of horizontal tile slices."},
		{.name = "tiles-y", .label = "Tiles Y", .group = "Tiling", .kind = ParamKind::Int, .defaultValue = int64_t{1}, .intRange = IntRange{1, 16, 1}, .help = "Number of vertical tile slices."},
		{.name = "progressive", .label = "Progressive", .group = "Bitstream", .kind = ParamKind::Bool, .defaultValue = false, .help = "Emit progressive mode."},
		{.name = "unscaled-arith", .label = "Unscaled arithmetic", .group = "Entropy", .kind = ParamKind::Bool, .defaultValue = false, .help = "Force unscaled arithmetic coding."},
	};
}

EncodedImage encode_jpegxr_still_image(const RawImage& image, std::span<const EncoderParam> params) {
	const JxlInputPixels rgb = raw_to_jxl_pixels(image);
	if (rgb.bitDepth != 8 && rgb.bitDepth != 16) {
		throw std::invalid_argument("JPEG XR supports 8-bit or 16-bit RGB input in this implementation");
	}
	std::filesystem::path path = std::filesystem::temp_directory_path() / "codec_vis_tmp.jxr";
	WMPStream* stream = nullptr;
	if (CreateWS_File(&stream, path.c_str(), "wb") != 0) throw std::runtime_error("jxrlib CreateWS_File failed");
	std::unique_ptr<WMPStream, void (*)(WMPStream*)> streamGuard(stream, [](WMPStream* s) {
		WMPStream* tmp = s;
		CloseWS_File(&tmp);
	});
	PKImageEncode* enc = nullptr;
	if (PKImageEncode_Create_WMP(&enc) != 0) throw std::runtime_error("jxrlib encoder allocation failed");
	std::unique_ptr<PKImageEncode, void (*)(PKImageEncode*)> encGuard(enc, [](PKImageEncode* e) {
		PKImageEncode* tmp = e;
		PKImageEncode_Release(&tmp);
	});
	CWMIStrCodecParam scp{};
	scp.uiDefaultQPIndex = static_cast<U8>(std::clamp<int64_t>(param_value<int64_t>(params, "qp", 32), 1, 255));
	scp.uiDefaultQPIndexYLP = static_cast<U8>(std::clamp<int64_t>(param_value<int64_t>(params, "qp-ylp", 0), 0, 255));
	scp.uiDefaultQPIndexYHP = static_cast<U8>(std::clamp<int64_t>(param_value<int64_t>(params, "qp-yhp", 0), 0, 255));
	scp.uiDefaultQPIndexU = static_cast<U8>(std::clamp<int64_t>(param_value<int64_t>(params, "qp-u", 0), 0, 255));
	scp.uiDefaultQPIndexV = static_cast<U8>(std::clamp<int64_t>(param_value<int64_t>(params, "qp-v", 0), 0, 255));
	const std::string overlap = param_value<std::string>(params, "overlap", "one");
	scp.olOverlap = overlap == "none" ? OL_NONE : (overlap == "two" ? OL_TWO : OL_ONE);
	const std::string order = param_value<std::string>(params, "bitstream-order", "spatial");
	scp.bfBitstreamFormat = order == "frequency" ? FREQUENCY : SPATIAL;
	const std::string subband = param_value<std::string>(params, "subband", "all");
	if (subband == "no-flexbits") scp.sbSubband = SB_NO_FLEXBITS;
	else if (subband == "no-highpass") scp.sbSubband = SB_NO_HIGHPASS;
	else if (subband == "dc-only") scp.sbSubband = SB_DC_ONLY;
	else scp.sbSubband = SB_ALL;
	scp.uiTrimFlexBits = static_cast<U8>(std::clamp<int64_t>(param_value<int64_t>(params, "trim-flex-bits", 0), 0, 15));
	scp.bUseHardTileBoundaries = param_value<bool>(params, "hard-tiles", false);
	scp.bProgressiveMode = param_value<bool>(params, "progressive", false);
	scp.bUnscaledArith = param_value<bool>(params, "unscaled-arith", false);
	const int tilesX = static_cast<int>(std::clamp<int64_t>(param_value<int64_t>(params, "tiles-x", 1), 1, 16));
	const int tilesY = static_cast<int>(std::clamp<int64_t>(param_value<int64_t>(params, "tiles-y", 1), 1, 16));
	scp.cNumOfSliceMinus1V = static_cast<U32>(tilesX - 1);
	scp.cNumOfSliceMinus1H = static_cast<U32>(tilesY - 1);
	for (int i = 0; i < tilesX; ++i) scp.uiTileX[i] = static_cast<U32>(std::max(1, (image.width + 15) / 16 / tilesX));
	for (int i = 0; i < tilesY; ++i) scp.uiTileY[i] = static_cast<U32>(std::max(1, (image.height + 15) / 16 / tilesY));
	const PKPixelFormatGUID pixelFormat =
		rgb.bitDepth == 8 ? GUID_PKPixelFormat24bppRGB : GUID_PKPixelFormat48bppRGB;
	if (enc->Initialize(enc, stream, &scp, sizeof(scp)) != 0 ||
	    enc->SetPixelFormat(enc, pixelFormat) != 0 ||
	    enc->SetSize(enc, image.width, image.height) != 0 ||
	    enc->WritePixels(
		    enc, static_cast<U32>(image.height), const_cast<U8*>(rgb.bytes.data()),
		    static_cast<U32>(image.width * 3 * (rgb.bitDepth == 8 ? 1 : 2))
	    ) != 0 ||
	    enc->Terminate(enc) != 0) {
		throw std::runtime_error("jxrlib encode failed");
	}
	encGuard.reset();
	streamGuard.release();
	const std::vector<uint8_t> file = read_temp_file(path);
	std::filesystem::remove(path);
	EncodedImage encoded;
	encoded.hevcAnnexB = bytes_from_u8(file);
	encoded.codedColor = image.color;
	return encoded;
}

std::vector<EncoderParamInfo> query_png_parameters() {
	return {
		{.name = "compression", .label = "Compression", .group = "Size / Time", .kind = ParamKind::Int, .defaultValue = int64_t{6}, .intRange = IntRange{0, 9, 1}, .help = "PNG zlib compression level. 0 is fastest/largest, 9 is slowest/smallest."},
		{.name = "strategy", .label = "Strategy", .group = "Size / Time", .kind = ParamKind::Enum, .defaultValue = std::string{"default"}, .enumValues = {{"default", "Default"}, {"filtered", "Filtered"}, {"rle", "RLE"}, {"huffman", "Huffman only"}, {"fixed", "Fixed Huffman"}}, .help = "zlib strategy. RLE/Huffman can be faster; Default is usually smallest."},
		{.name = "mem-level", .label = "Memory level", .group = "Size / Time", .kind = ParamKind::Int, .defaultValue = int64_t{8}, .intRange = IntRange{1, 9, 1}, .help = "zlib memory level. Higher can improve compression and speed at higher memory cost."},
		{.name = "window-bits", .label = "Window bits", .group = "Size / Time", .kind = ParamKind::Int, .defaultValue = int64_t{15}, .intRange = IntRange{8, 15, 1}, .help = "zlib window size. Smaller can reduce memory and sometimes size for tiny images."},
		{.name = "filters", .label = "Filters", .group = "Prediction", .kind = ParamKind::Enum, .defaultValue = std::string{"all"}, .enumValues = {{"none", "None"}, {"fast", "Fast"}, {"all", "All"}, {"sub", "Sub"}, {"up", "Up"}, {"avg", "Average"}, {"paeth", "Paeth"}}, .help = "PNG row filters to try. More filters can improve size at extra CPU cost."},
	};
}

EncodedImage encode_png_still_image(const RawImage& image, std::span<const EncoderParam> params) {
	const JxlInputPixels rgb = raw_to_jxl_pixels(image);
	if (rgb.bitDepth != 8 && rgb.bitDepth != 16) {
		throw std::invalid_argument("PNG supports 8-bit or 16-bit RGB input");
	}
	std::vector<std::byte> out;
	png_structp png = png_create_write_struct(PNG_LIBPNG_VER_STRING, nullptr, nullptr, nullptr);
	if (!png) throw std::runtime_error("png_create_write_struct failed");
	png_infop info = png_create_info_struct(png);
	if (!info) {
		png_destroy_write_struct(&png, nullptr);
		throw std::runtime_error("png_create_info_struct failed");
	}
	if (setjmp(png_jmpbuf(png))) {
		png_destroy_write_struct(&png, &info);
		throw std::runtime_error("libpng encode failed");
	}
	png_set_write_fn(png, &out, png_write_callback, nullptr);
	png_set_compression_level(png, static_cast<int>(param_value<int64_t>(params, "compression", 6)));
	png_set_compression_mem_level(png, static_cast<int>(param_value<int64_t>(params, "mem-level", 8)));
	png_set_compression_window_bits(png, static_cast<int>(param_value<int64_t>(params, "window-bits", 15)));
	const std::string strategy = param_value<std::string>(params, "strategy", "default");
	if (strategy == "filtered") png_set_compression_strategy(png, Z_FILTERED);
	else if (strategy == "rle") png_set_compression_strategy(png, Z_RLE);
	else if (strategy == "huffman") png_set_compression_strategy(png, Z_HUFFMAN_ONLY);
	else if (strategy == "fixed") png_set_compression_strategy(png, Z_FIXED);
	else png_set_compression_strategy(png, Z_DEFAULT_STRATEGY);
	const std::string filters = param_value<std::string>(params, "filters", "all");
	if (filters == "none") png_set_filter(png, PNG_FILTER_TYPE_BASE, PNG_FILTER_NONE);
	else if (filters == "fast") png_set_filter(png, PNG_FILTER_TYPE_BASE, PNG_FAST_FILTERS);
	else if (filters == "sub") png_set_filter(png, PNG_FILTER_TYPE_BASE, PNG_FILTER_SUB);
	else if (filters == "up") png_set_filter(png, PNG_FILTER_TYPE_BASE, PNG_FILTER_UP);
	else if (filters == "avg") png_set_filter(png, PNG_FILTER_TYPE_BASE, PNG_FILTER_AVG);
	else if (filters == "paeth") png_set_filter(png, PNG_FILTER_TYPE_BASE, PNG_FILTER_PAETH);
	else png_set_filter(png, PNG_FILTER_TYPE_BASE, PNG_ALL_FILTERS);
	png_set_IHDR(png, info, image.width, image.height, rgb.bitDepth, PNG_COLOR_TYPE_RGB, PNG_INTERLACE_NONE, PNG_COMPRESSION_TYPE_DEFAULT, PNG_FILTER_TYPE_DEFAULT);
	if (rgb.bitDepth == 16) png_set_swap(png);
	png_write_info(png, info);
	for (int y = 0; y < image.height; ++y) {
		png_bytep row = const_cast<png_bytep>(
			rgb.bytes.data() + static_cast<std::size_t>(y) * image.width * 3 * (rgb.bitDepth == 8 ? 1 : 2)
		);
		png_write_rows(png, &row, 1);
	}
	png_write_end(png, info);
	png_destroy_write_struct(&png, &info);
	EncodedImage encoded;
	encoded.hevcAnnexB = std::move(out);
	encoded.codedColor = image.color;
	return encoded;
}

std::vector<EncoderParamInfo> query_x264_parameters() {
	return {
		{.name = "preset", .label = "Preset", .group = "Speed / Search", .kind = ParamKind::Enum, .defaultValue = std::string{"medium"}, .enumValues = {{"ultrafast", "Ultrafast"}, {"superfast", "Superfast"}, {"veryfast", "Veryfast"}, {"faster", "Faster"}, {"fast", "Fast"}, {"medium", "Medium"}, {"slow", "Slow"}, {"slower", "Slower"}, {"veryslow", "Veryslow"}, {"placebo", "Placebo"}}, .help = "x264 preset."},
		{.name = "tune", .label = "Tune", .group = "Speed / Search", .kind = ParamKind::Enum, .defaultValue = std::string{"stillimage"}, .enumValues = {{"stillimage", "Still image"}, {"psnr", "PSNR"}, {"ssim", "SSIM"}, {"grain", "Grain"}, {"film", "Film"}, {"animation", "Animation"}, {"fastdecode", "Fast decode"}, {"zerolatency", "Zero latency"}}, .help = "x264 tune."},
		{.name = "rate-control", .label = "Mode", .group = "Rate Control", .kind = ParamKind::Enum, .defaultValue = std::string{"qp"}, .enumValues = {{"qp", "Constant QP"}, {"crf", "Constant quality (CRF)"}, {"lossless", "Lossless"}}, .help = "Still-image rate-control mode."},
		{.name = "qp", .label = "QP", .group = "Rate Control", .kind = ParamKind::Int, .defaultValue = int64_t{22}, .intRange = IntRange{1, 51, 1}, .help = "Constant quantizer. Lossless is a separate mode so profile compatibility is explicit.", .enabledWhen = {{"rate-control", {"qp"}, "Constant QP mode only"}}},
		{.name = "crf", .label = "CRF", .group = "Rate Control", .kind = ParamKind::Float, .defaultValue = double{23.0}, .floatRange = FloatRange{0.0, 51.0, 0.1}, .help = "Constant-rate-factor quality target.", .enabledWhen = {{"rate-control", {"crf"}, "CRF mode only"}}},
		{.name = "profile", .label = "Compatibility profile", .group = "Bitstream", .kind = ParamKind::Enum, .defaultValue = std::string{"auto"}, .enumValues = {{"auto", "Auto"}, {"baseline", "Baseline"}, {"main", "Main"}, {"high", "High"}}, .help = "Explicit compatibility profiles apply only to lossy 8-bit 4:2:0. Other formats and lossless mode require the profile derived by x264.", .enabledWhen = {{"bit-depth", {"8"}, "8-bit output only"}, {"chroma-subsampling", {"420"}, "4:2:0 output only"}, {"rate-control", {"qp", "crf"}, "Lossy modes only"}}},
		{.name = "cabac", .label = "CABAC", .group = "Entropy", .kind = ParamKind::Bool, .defaultValue = true, .help = "Enable CABAC entropy coding.", .enabledWhen = {{"profile", {"auto", "main", "high"}, "Baseline profile requires CAVLC"}}},
		{.name = "8x8dct", .label = "8x8 DCT", .group = "Transform", .kind = ParamKind::Bool, .defaultValue = true, .help = "Enable 8x8 transform.", .enabledWhen = {{"profile", {"auto", "high"}, "8x8 transform requires a High-family profile"}, {"rate-control", {"qp", "crf"}, "Lossy modes only"}}},
		{.name = "partitions", .label = "Partitions", .group = "Intra Analysis", .kind = ParamKind::Enum, .defaultValue = std::string{"i4x4,i8x8"}, .enumValues = {{"none", "None"}, {"i4x4", "I4x4"}, {"i8x8", "I8x8"}, {"i4x4,i8x8", "I4x4 + I8x8"}, {"all", "All"}}, .help = "Intra partition search modes.", .enabledWhen = {{"profile", {"auto", "high"}, "The exposed partition choices include High-profile I8x8"}}},
		{.name = "subme", .label = "Subme", .group = "Intra Analysis", .kind = ParamKind::Int, .defaultValue = int64_t{7}, .intRange = IntRange{0, 11, 1}, .help = "Subpixel/refinement and mode decision quality."},
		{.name = "trellis", .label = "Trellis", .group = "Quantization", .kind = ParamKind::Int, .defaultValue = int64_t{1}, .intRange = IntRange{0, 2, 1}, .help = "Trellis RD quantization.", .enabledWhen = {{"rate-control", {"qp", "crf"}, "Lossy modes only"}}},
		{.name = "psy", .label = "Psy", .group = "Psychovisual", .kind = ParamKind::Bool, .defaultValue = true, .help = "Enable psychovisual optimizations.", .enabledWhen = {{"rate-control", {"qp", "crf"}, "Lossy modes only"}}},
		{.name = "psy-rd", .label = "Psy RD", .group = "Psychovisual", .kind = ParamKind::String, .defaultValue = std::string{"1.0:0.0"}, .help = "x264 psy-rd string: psy-rd:psy-trellis.", .enabledWhen = {{"rate-control", {"qp", "crf"}, "Lossy modes only"}}},
		{.name = "aq-mode", .label = "AQ mode", .group = "Quantization", .kind = ParamKind::Int, .defaultValue = int64_t{1}, .intRange = IntRange{0, 3, 1}, .help = "Adaptive quantization mode.", .enabledWhen = {{"rate-control", {"qp", "crf"}, "Lossy modes only"}}},
		{.name = "aq-strength", .label = "AQ strength", .group = "Quantization", .kind = ParamKind::Float, .defaultValue = double{1.0}, .floatRange = FloatRange{0.0, 3.0, 0.05}, .help = "Adaptive quantization strength.", .enabledWhen = {{"rate-control", {"qp", "crf"}, "Lossy modes only"}}},
		{.name = "chroma-qp-offset", .label = "Chroma QP offset", .group = "Quantization", .kind = ParamKind::Int, .defaultValue = int64_t{0}, .intRange = IntRange{-12, 12, 1}, .help = "Chroma QP offset.", .enabledWhen = {{"rate-control", {"qp", "crf"}, "Lossy modes only"}}},
		{.name = "deadzone-intra", .label = "Deadzone intra", .group = "Quantization", .kind = ParamKind::Int, .defaultValue = int64_t{11}, .intRange = IntRange{0, 32, 1}, .help = "Luma intra quantization deadzone.", .enabledWhen = {{"rate-control", {"qp", "crf"}, "Lossy modes only"}}},
		{.name = "nr", .label = "Noise reduction", .group = "Quantization", .kind = ParamKind::Int, .defaultValue = int64_t{0}, .intRange = IntRange{0, 100000, 1}, .help = "Adaptive pseudo-deadzone noise reduction.", .enabledWhen = {{"rate-control", {"qp", "crf"}, "Lossy modes only"}}},
		{.name = "deblock", .label = "Deblock", .group = "Loop Filter", .kind = ParamKind::String, .defaultValue = std::string{"0:0"}, .help = "Deblocking filter alpha:beta offsets, or 0/false to disable.", .enabledWhen = {{"rate-control", {"qp", "crf"}, "Lossy modes only"}}},
		{.name = "constrained-intra", .label = "Constrained intra", .group = "Prediction", .kind = ParamKind::Bool, .defaultValue = false, .help = "Constrained intra prediction."},
		{.name = "slices", .label = "Slices", .group = "Bitstream", .kind = ParamKind::Int, .defaultValue = int64_t{1}, .intRange = IntRange{1, 128, 1}, .help = "Number of slices."},
		{.name = "aud", .label = "AUD", .group = "Bitstream", .kind = ParamKind::Bool, .defaultValue = false, .help = "Emit access unit delimiters."},
		{.name = "level", .label = "Level", .group = "Bitstream", .kind = ParamKind::String, .defaultValue = std::string{""}, .help = "Optional H.264 level, e.g. 4.1. Empty lets x264 choose."},
	};
}

EncodedImage encode_x264_intra_still_image(const RawImage& image, std::span<const EncoderParam> params) {
	x264_param_t p{};
	const std::string preset = param_value<std::string>(params, "preset", "medium");
	const std::string tune = param_value<std::string>(params, "tune", "stillimage");
	if (x264_param_default_preset(&p, preset.c_str(), tune.empty() ? nullptr : tune.c_str()) < 0) throw std::runtime_error("x264 preset setup failed");
	const X264InputFormat input = x264_input_format(image.format);
	p.i_width = image.width;
	p.i_height = image.height;
	p.i_fps_num = 1;
	p.i_fps_den = 1;
	p.i_keyint_max = 1;
	p.i_keyint_min = 1;
	p.i_frame_total = 1;
	p.i_bframe = 0;
	p.b_annexb = 1;
	p.b_repeat_headers = 1;
	p.i_csp = input.csp;
	p.i_bitdepth = input.bitDepth;
	p.i_threads = 1;
	p.vui.b_fullrange = image.color.range == ColorRange::Full;
	p.vui.i_colorprim = static_cast<int>(image.color.primaries);
	p.vui.i_transfer = static_cast<int>(image.color.transfer);
	p.vui.i_colmatrix = static_cast<int>(image.color.matrix);
	p.vui.i_chroma_loc = static_cast<int>(image.color.chroma420Location.value_or(Chroma420SampleLocation::LeftCenter));
	const std::string rateControl = param_value<std::string>(params, "rate-control", "qp");
	const bool lossless = rateControl == "lossless";
	if (rateControl == "qp") {
		p.rc.i_rc_method = X264_RC_CQP;
		p.rc.i_qp_constant = static_cast<int>(param_value<int64_t>(params, "qp", 22));
	}
	else if (rateControl == "crf") {
		p.rc.i_rc_method = X264_RC_CRF;
		p.rc.f_rf_constant = static_cast<float>(param_value<double>(params, "crf", 23.0));
	}
	else if (lossless) {
		p.rc.i_rc_method = X264_RC_CQP;
		p.rc.i_qp_constant = 0;
	}
	else {
		throw std::invalid_argument("x264: unsupported rate-control mode " + rateControl);
	}
	for (const EncoderParam& param : params) {
		if (param.name == "preset" || param.name == "tune" || param.name == "profile" ||
		    param.name == "rate-control" || param.name == "qp" || param.name == "crf") {
			continue;
		}
		const std::string value = value_to_cli_string(param.value);
		if (value.empty()) {
			continue;
		}
		if (x264_param_parse(&p, param.name.c_str(), value.c_str()) < 0) {
			throw std::invalid_argument("x264: invalid parameter '" + param.name + "' = " + value);
		}
	}
	std::string profile = param_value<std::string>(params, "profile", "auto");
	if (profile != "auto" && (lossless || input.bitDepth != 8 || !is_420(image.format))) {
		throw std::invalid_argument("x264: explicit baseline/main/high profiles require lossy 8-bit 4:2:0 output");
	}
	if (profile == "auto") profile = automatic_x264_profile(image, lossless);
	if (x264_param_apply_profile(&p, profile.c_str()) < 0) {
		throw std::invalid_argument("x264: invalid profile " + profile);
	}
	std::unique_ptr<x264_t, decltype(&x264_encoder_close)> enc(x264_encoder_open(&p), x264_encoder_close);
	if (!enc) throw std::runtime_error("x264_encoder_open failed");
	x264_picture_t picIn{};
	x264_picture_init(&picIn);
	picIn.img.i_csp = input.csp;
	picIn.img.i_plane = 3;
	for (int i = 0; i < 3; ++i) {
		picIn.img.plane[i] = const_cast<uint8_t*>(image.planes[i].bytes.data());
		picIn.img.i_stride[i] = image.planes[i].strideBytes;
	}
	picIn.i_type = X264_TYPE_IDR;
	x264_nal_t* nals = nullptr;
	int nalCount = 0;
	x264_picture_t picOut{};
	const int bytes = x264_encoder_encode(enc.get(), &nals, &nalCount, &picIn, &picOut);
	if (bytes < 0) throw std::runtime_error("x264_encoder_encode failed");
	EncodedImage encoded;
	auto appendNals = [&]() {
		for (int i = 0; i < nalCount; ++i) {
			const auto* first = reinterpret_cast<const std::byte*>(nals[i].p_payload);
			encoded.hevcAnnexB.insert(encoded.hevcAnnexB.end(), first, first + nals[i].i_payload);
		}
	};
	appendNals();
	while (x264_encoder_delayed_frames(enc.get()) > 0) {
		const int flushBytes = x264_encoder_encode(enc.get(), &nals, &nalCount, nullptr, &picOut);
		if (flushBytes < 0) throw std::runtime_error("x264_encoder_encode flush failed");
		appendNals();
	}
	encoded.codedColor = image.color;
	return encoded;
}

} // namespace codec_gui
