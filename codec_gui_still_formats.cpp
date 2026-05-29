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

int sample_bytes(PixelFormat format) {
	switch (format) {
		case PixelFormat::YUV420P8:
		case PixelFormat::YUV422P8:
		case PixelFormat::YUV444P8:
		case PixelFormat::Gray8:
			return 1;
		default:
			return 2;
	}
}

bool is_gray(PixelFormat format) {
	return format == PixelFormat::Gray8 || format == PixelFormat::Gray10LE;
}

bool is_420(PixelFormat format) {
	return format == PixelFormat::YUV420P8 || format == PixelFormat::YUV420P10LE;
}

bool is_422(PixelFormat format) {
	return format == PixelFormat::YUV422P8 || format == PixelFormat::YUV422P10LE;
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

uint8_t clamp_u8(double value) {
	return static_cast<uint8_t>(std::clamp<int>(static_cast<int>(std::lround(value)), 0, 255));
}

std::vector<uint8_t> raw_to_rgb8(const RawImage& image) {
	if (image.width <= 0 || image.height <= 0) {
		throw std::runtime_error("image dimensions must be positive");
	}
	const int bps = sample_bytes(image.format);
	const double yOffset = image.color.range == ColorRange::Full ? 0.0 : (bps == 1 ? 16.0 : 64.0);
	const double cOffset = bps == 1 ? 128.0 : 512.0;
	const double yScale = image.color.range == ColorRange::Full ? (bps == 1 ? 255.0 : 1023.0) : (bps == 1 ? 219.0 : 876.0);
	const double cScale = image.color.range == ColorRange::Full ? (bps == 1 ? 255.0 : 1023.0) : (bps == 1 ? 224.0 : 896.0);
	const uint16_t neutralY = bps == 1 ? 16 : 64;
	const uint16_t neutralC = bps == 1 ? 128 : 512;

	std::vector<uint8_t> rgb(static_cast<std::size_t>(image.width) * static_cast<std::size_t>(image.height) * 3);
	for (int y = 0; y < image.height; ++y) {
		for (int x = 0; x < image.width; ++x) {
			double r = 0.0;
			double g = 0.0;
			double b = 0.0;
			if (is_gray(image.format)) {
				const double maxSample = bps == 1 ? 255.0 : 1023.0;
				r = g = b = static_cast<double>(load_sample(image.planes[0], x, y, bps, 0)) * 255.0 / maxSample;
			} else {
				const int cx = (is_420(image.format) || is_422(image.format)) ? x / 2 : x;
				const int cy = is_420(image.format) ? y / 2 : y;
				const double yy = (static_cast<double>(load_sample(image.planes[0], x, y, bps, neutralY)) - yOffset) / yScale;
				const double cb = (static_cast<double>(load_sample(image.planes[1], cx, cy, bps, neutralC)) - cOffset) / cScale;
				const double cr = (static_cast<double>(load_sample(image.planes[2], cx, cy, bps, neutralC)) - cOffset) / cScale;
				constexpr double kr = 0.2126;
				constexpr double kb = 0.0722;
				constexpr double kg = 1.0 - kr - kb;
				r = (yy + (2.0 - 2.0 * kr) * cr) * 255.0;
				b = (yy + (2.0 - 2.0 * kb) * cb) * 255.0;
				g = ((yy - kr * (r / 255.0) - kb * (b / 255.0)) / kg) * 255.0;
			}
			const std::size_t offset = (static_cast<std::size_t>(y) * image.width + x) * 3;
			rgb[offset + 0] = clamp_u8(r);
			rgb[offset + 1] = clamp_u8(g);
			rgb[offset + 2] = clamp_u8(b);
		}
	}
	return rgb;
}

std::shared_ptr<const RawImage> embedded_preview(const RawImage& image) {
	return std::make_shared<RawImage>(image);
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

EncodedImage encoded_with_preview(std::vector<std::byte> bytes, const RawImage& preview) {
	EncodedImage encoded;
	encoded.hevcAnnexB = std::move(bytes);
	encoded.previewImage = embedded_preview(preview);
	return encoded;
}

RawImage rgb8_to_yuv444(const std::vector<uint8_t>& rgb, int width, int height) {
	RawImage image;
	image.width = width;
	image.height = height;
	image.format = PixelFormat::YUV444P8;
	image.color.range = ColorRange::Limited;
	for (int p = 0; p < 3; ++p) {
		image.planes[p].strideBytes = width;
		image.planes[p].bytes.resize(static_cast<std::size_t>(width) * static_cast<std::size_t>(height));
	}
	for (int y = 0; y < height; ++y) {
		for (int x = 0; x < width; ++x) {
			const std::size_t i = (static_cast<std::size_t>(y) * width + x) * 3;
			const double r = rgb[i + 0];
			const double g = rgb[i + 1];
			const double b = rgb[i + 2];
			const double yy = 0.2126 * r + 0.7152 * g + 0.0722 * b;
			const auto o = static_cast<std::size_t>(y) * width + x;
			image.planes[0].bytes[o] = clamp_u8(16.0 + 219.0 * yy / 255.0);
			image.planes[1].bytes[o] = clamp_u8(128.0 + 224.0 * (b - yy) / (2.0 * (255.0 - 255.0 * 0.0722)));
			image.planes[2].bytes[o] = clamp_u8(128.0 + 224.0 * (r - yy) / (2.0 * (255.0 - 255.0 * 0.2126)));
		}
	}
	return image;
}

RawImage rgb8_to_yuv420(const std::vector<uint8_t>& rgb, int width, int height) {
	RawImage image;
	image.width = (width + 1) & ~1;
	image.height = (height + 1) & ~1;
	image.format = PixelFormat::YUV420P8;
	image.color.range = ColorRange::Limited;
	image.planes[0].strideBytes = image.width;
	image.planes[1].strideBytes = image.width / 2;
	image.planes[2].strideBytes = image.width / 2;
	image.planes[0].bytes.resize(static_cast<std::size_t>(image.width) * static_cast<std::size_t>(image.height));
	image.planes[1].bytes.resize(static_cast<std::size_t>(image.width / 2) * static_cast<std::size_t>(image.height / 2));
	image.planes[2].bytes.resize(static_cast<std::size_t>(image.width / 2) * static_cast<std::size_t>(image.height / 2));
	auto rgb_at = [&](int x, int y, int c) -> double {
		const int sx = std::min(x, width - 1);
		const int sy = std::min(y, height - 1);
		return rgb[(static_cast<std::size_t>(sy) * width + sx) * 3 + c];
	};
	auto convert = [](double r, double g, double b, uint8_t& yy, uint8_t& cb, uint8_t& cr) {
		const double yFull = 0.2126 * r + 0.7152 * g + 0.0722 * b;
		yy = clamp_u8(16.0 + 219.0 * yFull / 255.0);
		cb = clamp_u8(128.0 + 224.0 * (b - yFull) / (2.0 * (255.0 - 255.0 * 0.0722)));
		cr = clamp_u8(128.0 + 224.0 * (r - yFull) / (2.0 * (255.0 - 255.0 * 0.2126)));
	};
	for (int y = 0; y < image.height; ++y) {
		for (int x = 0; x < image.width; ++x) {
			uint8_t yy = 0;
			uint8_t cb = 0;
			uint8_t cr = 0;
			convert(rgb_at(x, y, 0), rgb_at(x, y, 1), rgb_at(x, y, 2), yy, cb, cr);
			image.planes[0].bytes[static_cast<std::size_t>(y) * image.width + x] = yy;
		}
	}
	for (int y = 0; y < image.height / 2; ++y) {
		for (int x = 0; x < image.width / 2; ++x) {
			double r = 0.0;
			double g = 0.0;
			double b = 0.0;
			for (int dy = 0; dy < 2; ++dy) {
				for (int dx = 0; dx < 2; ++dx) {
					r += rgb_at(x * 2 + dx, y * 2 + dy, 0);
					g += rgb_at(x * 2 + dx, y * 2 + dy, 1);
					b += rgb_at(x * 2 + dx, y * 2 + dy, 2);
				}
			}
			uint8_t yy = 0;
			uint8_t cb = 0;
			uint8_t cr = 0;
			convert(r * 0.25, g * 0.25, b * 0.25, yy, cb, cr);
			const std::size_t i = static_cast<std::size_t>(y) * (image.width / 2) + x;
			image.planes[1].bytes[i] = cb;
			image.planes[2].bytes[i] = cr;
		}
	}
	return image;
}

RawImage yuv420p8_image_for_x264(const RawImage& image) {
	if (image.format == PixelFormat::YUV420P8 && (image.width % 2) == 0 && (image.height % 2) == 0) {
		return image;
	}
	return rgb8_to_yuv420(raw_to_rgb8(image), image.width, image.height);
}

EncodedImage encode_jpeg_like(const RawImage& image, std::span<const EncoderParam> params) {
	const std::vector<uint8_t> rgb = raw_to_rgb8(image);
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
	jpeg_set_quality(&cinfo, std::clamp(quality, 1, 100), TRUE);
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
		JSAMPROW row = const_cast<JSAMPROW>(rgb.data() + static_cast<std::size_t>(cinfo.next_scanline) * image.width * 3);
		jpeg_write_scanlines(&cinfo, &row, 1);
	}
	jpeg_finish_compress(&cinfo);
	std::vector<std::byte> bytes(memSize);
	std::memcpy(bytes.data(), mem, memSize);
	std::free(mem);
	jpeg_destroy_compress(&cinfo);
	return encoded_with_preview(std::move(bytes), rgb8_to_yuv444(rgb, image.width, image.height));
}

void png_write_callback(png_structp png, png_bytep data, png_size_t length) {
	auto* out = static_cast<std::vector<std::byte>*>(png_get_io_ptr(png));
	const auto* first = reinterpret_cast<const std::byte*>(data);
	out->insert(out->end(), first, first + length);
}

} // namespace

std::vector<EncoderParamInfo> query_jpegls_parameters() {
	return {
		{.name = "near", .label = "NEAR", .group = "Coding", .kind = ParamKind::Int, .defaultValue = int64_t{0}, .intRange = IntRange{0, 15, 1}, .help = "JPEG-LS near-lossless error tolerance. 0 is lossless."},
		{.name = "color-transform", .label = "Color transform", .group = "Coding", .kind = ParamKind::Enum, .defaultValue = std::string{"none"}, .enumValues = {{"none", "None"}, {"hp1", "HP1"}, {"hp2", "HP2"}, {"hp3", "HP3"}}, .help = "Optional HP reversible color transform. Not part of baseline JPEG-LS interchange."},
		{.name = "even-size", .label = "Even size", .group = "Bitstream", .kind = ParamKind::Bool, .defaultValue = false, .help = "Pad the destination to even byte size."},
		{.name = "version-comment", .label = "Version comment", .group = "Bitstream", .kind = ParamKind::Bool, .defaultValue = false, .help = "Include CharLS version comment segment."},
		{.name = "jai-pc-parameters", .label = "JAI PC params", .group = "Bitstream", .kind = ParamKind::Bool, .defaultValue = false, .help = "Include JAI-compatible preset coding parameters."},
	};
}

EncodedImage encode_jpegls_still_image(const RawImage& image, std::span<const EncoderParam> params) {
	const std::vector<uint8_t> rgb = raw_to_rgb8(image);
	charls::jpegls_encoder encoder;
	encoder.frame_info({static_cast<uint32_t>(image.width), static_cast<uint32_t>(image.height), 8, 3})
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
	const std::size_t written = encoder.encode(rgb, static_cast<uint32_t>(image.width * 3));
	out.resize(written);
	return encoded_with_preview(bytes_from_u8(out), rgb8_to_yuv444(rgb, image.width, image.height));
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
		{.name = "codeblock-width", .label = "Codeblock W", .group = "Codeblocks", .kind = ParamKind::Int, .defaultValue = int64_t{64}, .intRange = IntRange{4, 1024, 1}, .help = "Initial codeblock width."},
		{.name = "codeblock-height", .label = "Codeblock H", .group = "Codeblocks", .kind = ParamKind::Int, .defaultValue = int64_t{64}, .intRange = IntRange{4, 1024, 1}, .help = "Initial codeblock height."},
		{.name = "progression", .label = "Progression", .group = "Ordering", .kind = ParamKind::Enum, .defaultValue = std::string{"LRCP"}, .enumValues = {{"LRCP", "LRCP"}, {"RLCP", "RLCP"}, {"RPCL", "RPCL"}, {"PCRL", "PCRL"}, {"CPRL", "CPRL"}}, .help = "JPEG 2000 progression order."},
		{.name = "mct", .label = "MCT", .group = "Color", .kind = ParamKind::Bool, .defaultValue = true, .help = "Multiple component transform for RGB."},
		{.name = "tile-width", .label = "Tile W", .group = "Tiling", .kind = ParamKind::Int, .defaultValue = int64_t{0}, .intRange = IntRange{0, 65535, 1}, .help = "0 disables tiling; otherwise tile width."},
		{.name = "tile-height", .label = "Tile H", .group = "Tiling", .kind = ParamKind::Int, .defaultValue = int64_t{0}, .intRange = IntRange{0, 65535, 1}, .help = "0 disables tiling; otherwise tile height."},
	};
}

EncodedImage encode_jpeg2000_still_image(const RawImage& image, std::span<const EncoderParam> encoderParams) {
	const std::vector<uint8_t> rgb = raw_to_rgb8(image);
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
	params.cblockw_init = static_cast<int>(param_value<int64_t>(encoderParams, "codeblock-width", 64));
	params.cblockh_init = static_cast<int>(param_value<int64_t>(encoderParams, "codeblock-height", 64));
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
		c.prec = 8;
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
			ojImage->comps[0].data[dst] = rgb[src + 0];
			ojImage->comps[1].data[dst] = rgb[src + 1];
			ojImage->comps[2].data[dst] = rgb[src + 2];
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
	return encoded_with_preview(bytes_from_u8(file), rgb8_to_yuv444(rgb, image.width, image.height));
}

std::vector<EncoderParamInfo> query_jpegxl_parameters() {
	return {
		{.name = "quality", .label = "Quality", .group = "Coding", .kind = ParamKind::Int, .defaultValue = int64_t{50}, .intRange = IntRange{1, 100, 1}, .help = "JPEG-style quality mapped to JPEG XL distance. 100 maps to distance 0 but does not force lossless unless Lossless is enabled."},
		{.name = "distance", .label = "Distance", .group = "Coding", .kind = ParamKind::Float, .defaultValue = double{-1.0}, .floatRange = FloatRange{-1.0, 25.0, 0.1}, .help = "Butteraugli distance override. -1 uses Quality."},
		{.name = "lossless", .label = "Lossless", .group = "Coding", .kind = ParamKind::Bool, .defaultValue = false, .help = "Use true JPEG XL lossless mode."},
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
		{.name = "color-transform", .label = "Color transform", .group = "Tools", .kind = ParamKind::Int, .defaultValue = int64_t{-1}, .intRange = IntRange{-1, 2, 1}, .help = "-1 default, 0 none, 1 XYB, 2 YCbCr."},
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
}

EncodedImage encode_jpegxl_still_image(const RawImage& image, std::span<const EncoderParam> params) {
	const std::vector<uint8_t> rgb = raw_to_rgb8(image);
	std::unique_ptr<JxlEncoder, decltype(&JxlEncoderDestroy)> enc(JxlEncoderCreate(nullptr), JxlEncoderDestroy);
	if (!enc) throw std::runtime_error("JxlEncoderCreate failed");
	JxlBasicInfo info{};
	JxlEncoderInitBasicInfo(&info);
	info.xsize = static_cast<uint32_t>(image.width);
	info.ysize = static_cast<uint32_t>(image.height);
	info.bits_per_sample = 8;
	info.num_color_channels = 3;
	info.uses_original_profile = JXL_FALSE;
	if (JxlEncoderSetBasicInfo(enc.get(), &info) != JXL_ENC_SUCCESS) throw std::runtime_error("JxlEncoderSetBasicInfo failed");
	JxlColorEncoding color{};
	JxlColorEncodingSetToSRGB(&color, JXL_FALSE);
	if (JxlEncoderSetColorEncoding(enc.get(), &color) != JXL_ENC_SUCCESS) throw std::runtime_error("JxlEncoderSetColorEncoding failed");
	JxlEncoderFrameSettings* frame = JxlEncoderFrameSettingsCreate(enc.get(), nullptr);
	auto setOption = [&](const std::string& name, JxlEncoderFrameSettingId option, int64_t fallback) {
		const int value = static_cast<int>(param_value<int64_t>(params, name, fallback));
		if (value == fallback) {
			return;
		}
		if (JxlEncoderFrameSettingsSetOption(frame, option, value) != JXL_ENC_SUCCESS) {
			throw std::runtime_error("JxlEncoderFrameSettingsSetOption failed for " + name);
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
	const int quality = static_cast<int>(param_value<int64_t>(params, "quality", 50));
	const bool lossless = param_value<bool>(params, "lossless", false);
	if (JxlEncoderSetFrameLossless(frame, lossless ? JXL_TRUE : JXL_FALSE) != JXL_ENC_SUCCESS) {
		throw std::runtime_error("JxlEncoderSetFrameLossless failed");
	}
	if (lossless) {
		if (JxlEncoderSetFrameDistance(frame, 0.0f) != JXL_ENC_SUCCESS) {
			throw std::runtime_error("JxlEncoderSetFrameDistance failed");
		}
	} else {
		const double distanceOverride = param_value<double>(params, "distance", -1.0);
		const float distance = distanceOverride >= 0.0 ? static_cast<float>(distanceOverride) : JxlEncoderDistanceFromQuality(static_cast<float>(quality));
		if (JxlEncoderSetFrameDistance(frame, distance) != JXL_ENC_SUCCESS) {
			throw std::runtime_error("JxlEncoderSetFrameDistance failed");
		}
	}
	const JxlPixelFormat format{3, JXL_TYPE_UINT8, JXL_NATIVE_ENDIAN, 0};
	if (JxlEncoderAddImageFrame(frame, &format, rgb.data(), rgb.size()) != JXL_ENC_SUCCESS) throw std::runtime_error("JxlEncoderAddImageFrame failed");
	JxlEncoderCloseInput(enc.get());
	std::vector<std::byte> out(4096);
	uint8_t* next = reinterpret_cast<uint8_t*>(out.data());
	size_t avail = out.size();
	for (;;) {
		const JxlEncoderStatus status = JxlEncoderProcessOutput(enc.get(), &next, &avail);
		if (status == JXL_ENC_SUCCESS) break;
		if (status != JXL_ENC_NEED_MORE_OUTPUT) throw std::runtime_error("JxlEncoderProcessOutput failed");
		const std::size_t used = out.size() - avail;
		out.resize(out.size() * 2);
		next = reinterpret_cast<uint8_t*>(out.data()) + used;
		avail = out.size() - used;
	}
	out.resize(out.size() - avail);
	return encoded_with_preview(std::move(out), rgb8_to_yuv444(rgb, image.width, image.height));
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
	const std::vector<uint8_t> rgb = raw_to_rgb8(image);
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
	if (enc->Initialize(enc, stream, &scp, sizeof(scp)) != 0 ||
	    enc->SetPixelFormat(enc, GUID_PKPixelFormat24bppRGB) != 0 ||
	    enc->SetSize(enc, image.width, image.height) != 0 ||
	    enc->WritePixels(enc, static_cast<U32>(image.height), const_cast<U8*>(rgb.data()), static_cast<U32>(image.width * 3)) != 0 ||
	    enc->Terminate(enc) != 0) {
		throw std::runtime_error("jxrlib encode failed");
	}
	encGuard.reset();
	streamGuard.release();
	const std::vector<uint8_t> file = read_temp_file(path);
	std::filesystem::remove(path);
	return encoded_with_preview(bytes_from_u8(file), rgb8_to_yuv444(rgb, image.width, image.height));
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
	const std::vector<uint8_t> rgb = raw_to_rgb8(image);
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
	png_set_IHDR(png, info, image.width, image.height, 8, PNG_COLOR_TYPE_RGB, PNG_INTERLACE_NONE, PNG_COMPRESSION_TYPE_DEFAULT, PNG_FILTER_TYPE_DEFAULT);
	png_write_info(png, info);
	for (int y = 0; y < image.height; ++y) {
		png_bytep row = const_cast<png_bytep>(rgb.data() + static_cast<std::size_t>(y) * image.width * 3);
		png_write_rows(png, &row, 1);
	}
	png_write_end(png, info);
	png_destroy_write_struct(&png, &info);
	return encoded_with_preview(std::move(out), rgb8_to_yuv444(rgb, image.width, image.height));
}

std::vector<EncoderParamInfo> query_x264_parameters() {
	return {
		{.name = "preset", .label = "Preset", .group = "Speed / Search", .kind = ParamKind::Enum, .defaultValue = std::string{"medium"}, .enumValues = {{"ultrafast", "Ultrafast"}, {"superfast", "Superfast"}, {"veryfast", "Veryfast"}, {"faster", "Faster"}, {"fast", "Fast"}, {"medium", "Medium"}, {"slow", "Slow"}, {"slower", "Slower"}, {"veryslow", "Veryslow"}, {"placebo", "Placebo"}}, .help = "x264 preset."},
		{.name = "tune", .label = "Tune", .group = "Speed / Search", .kind = ParamKind::Enum, .defaultValue = std::string{"stillimage"}, .enumValues = {{"stillimage", "Still image"}, {"psnr", "PSNR"}, {"ssim", "SSIM"}, {"grain", "Grain"}, {"film", "Film"}, {"animation", "Animation"}, {"fastdecode", "Fast decode"}, {"zerolatency", "Zero latency"}}, .help = "x264 tune."},
		{.name = "profile", .label = "Profile", .group = "Bitstream", .kind = ParamKind::Enum, .defaultValue = std::string{"high"}, .enumValues = {{"baseline", "Baseline"}, {"main", "Main"}, {"high", "High"}, {"high10", "High 10"}, {"high422", "High 4:2:2"}, {"high444", "High 4:4:4"}}, .help = "H.264 profile constraint."},
		{.name = "qp", .label = "QP", .group = "Rate Control", .kind = ParamKind::Int, .defaultValue = int64_t{22}, .intRange = IntRange{0, 51, 1}, .help = "Constant quantizer. 0 is lossless when supported by profile/pixel format."},
		{.name = "cabac", .label = "CABAC", .group = "Entropy", .kind = ParamKind::Bool, .defaultValue = true, .help = "Enable CABAC entropy coding."},
		{.name = "8x8dct", .label = "8x8 DCT", .group = "Transform", .kind = ParamKind::Bool, .defaultValue = true, .help = "Enable 8x8 transform."},
		{.name = "partitions", .label = "Partitions", .group = "Intra Analysis", .kind = ParamKind::Enum, .defaultValue = std::string{"i4x4,i8x8"}, .enumValues = {{"none", "None"}, {"i4x4", "I4x4"}, {"i8x8", "I8x8"}, {"i4x4,i8x8", "I4x4 + I8x8"}, {"all", "All"}}, .help = "Intra partition search modes."},
		{.name = "subme", .label = "Subme", .group = "Intra Analysis", .kind = ParamKind::Int, .defaultValue = int64_t{7}, .intRange = IntRange{0, 11, 1}, .help = "Subpixel/refinement and mode decision quality."},
		{.name = "trellis", .label = "Trellis", .group = "Quantization", .kind = ParamKind::Int, .defaultValue = int64_t{1}, .intRange = IntRange{0, 2, 1}, .help = "Trellis RD quantization."},
		{.name = "psy", .label = "Psy", .group = "Psychovisual", .kind = ParamKind::Bool, .defaultValue = true, .help = "Enable psychovisual optimizations."},
		{.name = "psy-rd", .label = "Psy RD", .group = "Psychovisual", .kind = ParamKind::String, .defaultValue = std::string{"1.0:0.0"}, .help = "x264 psy-rd string: psy-rd:psy-trellis."},
		{.name = "aq-mode", .label = "AQ mode", .group = "Quantization", .kind = ParamKind::Int, .defaultValue = int64_t{1}, .intRange = IntRange{0, 3, 1}, .help = "Adaptive quantization mode."},
		{.name = "aq-strength", .label = "AQ strength", .group = "Quantization", .kind = ParamKind::Float, .defaultValue = double{1.0}, .floatRange = FloatRange{0.0, 3.0, 0.05}, .help = "Adaptive quantization strength."},
		{.name = "chroma-qp-offset", .label = "Chroma QP offset", .group = "Quantization", .kind = ParamKind::Int, .defaultValue = int64_t{0}, .intRange = IntRange{-12, 12, 1}, .help = "Chroma QP offset."},
		{.name = "deadzone-intra", .label = "Deadzone intra", .group = "Quantization", .kind = ParamKind::Int, .defaultValue = int64_t{11}, .intRange = IntRange{0, 32, 1}, .help = "Luma intra quantization deadzone."},
		{.name = "nr", .label = "Noise reduction", .group = "Quantization", .kind = ParamKind::Int, .defaultValue = int64_t{0}, .intRange = IntRange{0, 100000, 1}, .help = "Adaptive pseudo-deadzone noise reduction."},
		{.name = "deblock", .label = "Deblock", .group = "Loop Filter", .kind = ParamKind::String, .defaultValue = std::string{"0:0"}, .help = "Deblocking filter alpha:beta offsets, or 0/false to disable."},
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
	RawImage src = yuv420p8_image_for_x264(image);
	p.i_width = src.width;
	p.i_height = src.height;
	p.i_fps_num = 1;
	p.i_fps_den = 1;
	p.i_keyint_max = 1;
	p.i_keyint_min = 1;
	p.i_frame_total = 1;
	p.i_bframe = 0;
	p.b_annexb = 1;
	p.b_repeat_headers = 1;
	p.i_csp = X264_CSP_I420;
	p.rc.i_rc_method = X264_RC_CQP;
	p.i_threads = 1;
	for (const EncoderParam& param : params) {
		if (param.name == "preset" || param.name == "tune" || param.name == "profile") {
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
	const std::string profile = param_value<std::string>(params, "profile", "high");
	if (!profile.empty() && x264_param_apply_profile(&p, profile.c_str()) < 0) {
		throw std::invalid_argument("x264: invalid profile " + profile);
	}
	std::unique_ptr<x264_t, decltype(&x264_encoder_close)> enc(x264_encoder_open(&p), x264_encoder_close);
	if (!enc) throw std::runtime_error("x264_encoder_open failed");
	x264_picture_t picIn{};
	x264_picture_init(&picIn);
	picIn.img.i_csp = X264_CSP_I420;
	picIn.img.i_plane = 3;
	for (int i = 0; i < 3; ++i) {
		picIn.img.plane[i] = const_cast<uint8_t*>(src.planes[i].bytes.data());
		picIn.img.i_stride[i] = src.planes[i].strideBytes;
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
	return encoded;
}

} // namespace codec_gui
