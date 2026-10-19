#include "codec_gui_image_io.hpp"
#include "codec_gui_x265.hpp"
#include "gui/preview_decoders.hpp"
#include "gui/raw_image_utils.hpp"
#include "gui/codec_timer.hpp"
#include "libraw_const.h"
#include "libraw_types.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cctype>
#include <csetjmp>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <ios>
#include <iterator>
#include <jxl/cms_interface.h>
#include <jxl/codestream_header.h>
#include <jxl/color_encoding.h>
#include <jxl/types.h>
#include <limits>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <variant>

#include <libraw/libraw.h>
#include <lcms2.h>
#include <libheif/heif.h>
#include <libheif/heif_items.h>
#include <jxl/cms.h>
#include <jxl/decode.h>
#include <vector>

extern "C" {
#include <jpeglib.h>
#include <png.h>
}

namespace codec_gui {
namespace {

struct JpegError {
	jpeg_error_mgr pub;
	jmp_buf jump;
	char message[JMSG_LENGTH_MAX]{};
};

void jpeg_error_exit(j_common_ptr cinfo) {
	auto* err = reinterpret_cast<JpegError*>(cinfo->err);
	(*cinfo->err->format_message)(cinfo, err->message);
	longjmp(err->jump, 1);
}

struct RgbImage {
	int width = 0;
	int height = 0;
	std::vector<unsigned char> rgb;
	ColorDescription color{.primaries=ColorPrimaries::BT709, .transfer=TransferCharacteristics::SRGB, .matrix=MatrixCoefficients::Identity, .range=ColorRange::Full, .chroma420Location=std::nullopt};
	std::optional<double> nominalPeakNits;
};

struct Rgb16Image {
	int width = 0;
	int height = 0;
	std::vector<unsigned short> rgb;
	ColorDescription color{.primaries=ColorPrimaries::BT709, .transfer=TransferCharacteristics::SRGB, .matrix=MatrixCoefficients::Identity, .range=ColorRange::Full, .chroma420Location=std::nullopt};
	std::optional<double> nominalPeakNits;
};

struct PngImage {
	bool sixteenBit = false;
	RgbImage rgb8;
	Rgb16Image rgb16;
	std::string importNote;
};

struct PngAllocations {
	void* pixels = nullptr;
	png_bytep* rows = nullptr;
	char error[256]{};
};

enum class PngColorKind {
	Cicp,
	Icc,
	Srgb,
	Chromaticities,
	AssumedSrgb,
};

struct PngColorMetadata {
	PngColorKind kind = PngColorKind::AssumedSrgb;
	std::vector<unsigned char> icc;
	ColorDescription cicp;
	std::optional<double> nominalPeakNits;
	double whiteX = 0.3127;
	double whiteY = 0.3290;
	double redX = 0.64;
	double redY = 0.33;
	double greenX = 0.30;
	double greenY = 0.60;
	double blueX = 0.15;
	double blueY = 0.06;
	double fileGamma = 1.0 / 2.2;
};

ColorDescription png_cicp_description(
	unsigned int primaries,
	unsigned int transfer,
	unsigned int matrix,
	unsigned int fullRange
) {
	if (matrix != 0) throw std::runtime_error("PNG cICP RGB image must use identity matrix coefficients");
	if (primaries != static_cast<unsigned int>(ColorPrimaries::BT709) &&
	    primaries != static_cast<unsigned int>(ColorPrimaries::BT2020) &&
	    primaries != static_cast<unsigned int>(ColorPrimaries::DisplayP3)) {
		throw std::runtime_error("PNG cICP uses unsupported H.273 color primaries " + std::to_string(primaries));
	}
	switch (static_cast<TransferCharacteristics>(transfer)) {
		case TransferCharacteristics::BT709:
		case TransferCharacteristics::Gamma22:
		case TransferCharacteristics::Gamma28:
		case TransferCharacteristics::Linear:
		case TransferCharacteristics::SRGB:
		case TransferCharacteristics::BT2020_10:
		case TransferCharacteristics::BT2020_12:
		case TransferCharacteristics::PQ:
		case TransferCharacteristics::HLG:
			break;
		default:
			throw std::runtime_error("PNG cICP uses unsupported H.273 transfer characteristics " + std::to_string(transfer));
	}
	ColorDescription color{
		.primaries=static_cast<ColorPrimaries>(primaries),
		.transfer=static_cast<TransferCharacteristics>(transfer),
		.matrix=MatrixCoefficients::Identity,
		.range=fullRange != 0 ? ColorRange::Full : ColorRange::Limited,
		.chroma420Location=std::nullopt,
	};
	return color;
}

cmsHPROFILE create_linear_bt2020_profile() {
	const cmsCIExyY white{.x=0.3127, .y=0.3290, .Y=1.0};
	const cmsCIExyYTRIPLE primaries{
		.Red={.x=0.708, .y=0.292, .Y=1.0},
		.Green={.x=0.170, .y=0.797, .Y=1.0},
		.Blue={.x=0.131, .y=0.046, .Y=1.0},
	};
	cmsToneCurve* curves[3]{
		cmsBuildGamma(nullptr, 1.0),
		cmsBuildGamma(nullptr, 1.0),
		cmsBuildGamma(nullptr, 1.0),
	};
	if (!curves[0] || !curves[1] || !curves[2]) {
		for (cmsToneCurve* curve : curves) if (curve) cmsFreeToneCurve(curve);
		throw std::runtime_error("LittleCMS failed to create linear transfer curves");
	}
	cmsHPROFILE profile = cmsCreateRGBProfile(&white, &primaries, curves);
	for (cmsToneCurve* curve : curves) cmsFreeToneCurve(curve);
	if (!profile) throw std::runtime_error("LittleCMS failed to create the internal linear BT.2020 profile");
	return profile;
}

cmsHPROFILE create_png_chromaticity_profile(const PngColorMetadata& metadata) {
	if (metadata.fileGamma <= 0.0) throw std::runtime_error("PNG gAMA value must be positive");
	const cmsCIExyY white{.x=metadata.whiteX, .y=metadata.whiteY, .Y=1.0};
	const cmsCIExyYTRIPLE primaries{
		.Red={.x=metadata.redX, .y=metadata.redY, .Y=1.0},
		.Green={.x=metadata.greenX, .y=metadata.greenY, .Y=1.0},
		.Blue={.x=metadata.blueX, .y=metadata.blueY, .Y=1.0},
	};
	const double decodingGamma = 1.0 / metadata.fileGamma;
	cmsToneCurve* curves[3]{
		cmsBuildGamma(nullptr, decodingGamma),
		cmsBuildGamma(nullptr, decodingGamma),
		cmsBuildGamma(nullptr, decodingGamma),
	};
	if (!curves[0] || !curves[1] || !curves[2]) {
		for (cmsToneCurve* curve : curves) if (curve) cmsFreeToneCurve(curve);
		throw std::runtime_error("LittleCMS failed to create PNG gamma curves");
	}
	cmsHPROFILE profile = cmsCreateRGBProfile(&white, &primaries, curves);
	for (cmsToneCurve* curve : curves) cmsFreeToneCurve(curve);
	if (!profile) throw std::runtime_error("LittleCMS rejected PNG cHRM/gAMA metadata");
	return profile;
}

Rgb16Image convert_to_linear_bt2020(
	int width,
	int height,
	const void* input,
	cmsUInt32Number inputFormat,
	cmsHPROFILE inputProfile
) {
	if (!inputProfile ||
	    (cmsGetColorSpace(inputProfile) != cmsSigRgbData &&
	     cmsGetColorSpace(inputProfile) != cmsSigGrayData)) {
		throw std::runtime_error("embedded input ICC profile is not an RGB or grayscale profile");
	}
	const std::size_t pixelCount = static_cast<std::size_t>(width) * static_cast<std::size_t>(height);
	if (pixelCount > std::numeric_limits<cmsUInt32Number>::max()) {
		throw std::runtime_error("image is too large for a LittleCMS transform");
	}
	cmsHPROFILE outputProfile = create_linear_bt2020_profile();
	const cmsUInt32Number intent = std::min<cmsUInt32Number>(cmsGetHeaderRenderingIntent(inputProfile), INTENT_ABSOLUTE_COLORIMETRIC);
	cmsHTRANSFORM transform = cmsCreateTransform(
		inputProfile,
		inputFormat,
		outputProfile,
		TYPE_RGB_16,
		intent,
		cmsFLAGS_BLACKPOINTCOMPENSATION
	);
	if (!transform) {
		cmsCloseProfile(outputProfile);
		throw std::runtime_error("LittleCMS could not transform the embedded input profile");
	}
	Rgb16Image result;
	result.width = width;
	result.height = height;
	result.color = {.primaries=ColorPrimaries::BT2020, .transfer=TransferCharacteristics::Linear, .matrix=MatrixCoefficients::Identity, .range=ColorRange::Full, .chroma420Location=std::nullopt};
	result.rgb.resize(pixelCount * 3);
	cmsDoTransform(transform, input, result.rgb.data(), static_cast<cmsUInt32Number>(pixelCount));
	cmsDeleteTransform(transform);
	cmsCloseProfile(outputProfile);
	return result;
}

PngColorMetadata read_png_color_metadata(png_structp png, png_infop info) {
	PngColorMetadata metadata;
#ifdef PNG_cICP_SUPPORTED
	png_byte primaries = 0;
	png_byte transfer = 0;
	png_byte matrix = 0;
	png_byte fullRange = 0;
	if (png_get_cICP(png, info, &primaries, &transfer, &matrix, &fullRange) != 0) {
		metadata.kind = PngColorKind::Cicp;
		metadata.cicp = png_cicp_description(primaries, transfer, matrix, fullRange);
		if (metadata.cicp.transfer == TransferCharacteristics::HLG) {
			metadata.nominalPeakNits = 1000.0;
		}
		return metadata;
	}
#endif
	png_charp profileName = nullptr;
	int compressionType = 0;
	png_bytep profileData = nullptr;
	png_uint_32 profileSize = 0;
	if (png_get_iCCP(png, info, &profileName, &compressionType, &profileData, &profileSize) != 0 &&
	    profileData != nullptr && profileSize > 0) {
		metadata.kind = PngColorKind::Icc;
		metadata.icc.assign(profileData, profileData + profileSize);
		return metadata;
	}
	int renderingIntent = 0;
	if (png_get_sRGB(png, info, &renderingIntent) != 0) {
		metadata.kind = PngColorKind::Srgb;
		return metadata;
	}
	double gamma = 0.0;
	const bool hasGamma = png_get_gAMA(png, info, &gamma) != 0;
	const bool hasChromaticities = png_get_cHRM(
		png,
		info,
		&metadata.whiteX,
		&metadata.whiteY,
		&metadata.redX,
		&metadata.redY,
		&metadata.greenX,
		&metadata.greenY,
		&metadata.blueX,
		&metadata.blueY
	) != 0;
	if (hasGamma) metadata.fileGamma = gamma;
	if (hasGamma || hasChromaticities) metadata.kind = PngColorKind::Chromaticities;
	return metadata;
}

PngImage load_png_rgb(std::span<const unsigned char> bytes, double* codecSeconds = nullptr) {
	png_structp png = png_create_read_struct(PNG_LIBPNG_VER_STRING, nullptr, nullptr, nullptr);
	if (png == nullptr) {
		throw std::runtime_error("failed to create PNG decoder");
	}
	png_infop info = png_create_info_struct(png);
	if (info == nullptr) {
		png_destroy_read_struct(&png, nullptr, nullptr);
		throw std::runtime_error("failed to create PNG metadata decoder");
	}

	auto* allocations = static_cast<PngAllocations*>(std::calloc(1, sizeof(PngAllocations)));
	if (allocations == nullptr) {
		png_destroy_read_struct(&png, &info, nullptr);
		throw std::runtime_error("out of memory creating PNG decoder");
	}
	if (setjmp(png_jmpbuf(png)) != 0) {
		const std::string message = allocations->error;
		std::free(allocations->rows);
		std::free(allocations->pixels);
		std::free(allocations);
		png_destroy_read_struct(&png, &info, nullptr);
		throw std::runtime_error("PNG decode failed" + (message.empty() ? std::string{} : ": " + message));
	}
	// libpng normally drops malformed ICC metadata with only a stderr warning.
	// Do not silently substitute sRGB when a supplied profile cannot be read.
	png_set_error_fn(png, allocations, [](png_structp decoder, png_const_charp message) {
		auto* memory = static_cast<PngAllocations*>(png_get_error_ptr(decoder));
		std::snprintf(memory->error, sizeof(memory->error), "%s", message);
		png_longjmp(decoder, 1);
	}, [](png_structp decoder, png_const_charp message) {
		if (std::strstr(message, "iCCP") || std::strstr(message, "ICC"))
			png_error(decoder, "Invalid embedded PNG ICC profile");
		std::fprintf(stderr, "libpng warning: %s\n", message);
	});

	struct Reader {std::span<const unsigned char> bytes; std::size_t pos=0;} reader{bytes};
	png_set_read_fn(png,&reader,[](png_structp decoder,png_bytep dst,png_size_t count) {
		auto& r=*static_cast<Reader*>(png_get_io_ptr(decoder));
		if(count>r.bytes.size()-r.pos) png_error(decoder,"Truncated PNG input");
		std::memcpy(dst,r.bytes.data()+r.pos,count); r.pos+=count;
	});
	gui::CodecTimer timer;
	timer.measure([&] {png_read_info(png, info);});
	const png_uint_32 width = png_get_image_width(png, info);
	const png_uint_32 height = png_get_image_height(png, info);
	const int sourceBitDepth = png_get_bit_depth(png, info);
	const int colorType = png_get_color_type(png, info);
	if (width == 0 || height == 0 ||
	    width > static_cast<png_uint_32>(std::numeric_limits<int>::max()) ||
	    height > static_cast<png_uint_32>(std::numeric_limits<int>::max())) {
		png_error(png, "invalid PNG dimensions");
	}
	if (colorType == PNG_COLOR_TYPE_PALETTE) png_set_palette_to_rgb(png);
	if (colorType == PNG_COLOR_TYPE_GRAY && sourceBitDepth < 8) png_set_expand_gray_1_2_4_to_8(png);
	const bool hasTransparency = png_get_valid(png, info, PNG_INFO_tRNS) != 0;
	if (hasTransparency) png_set_tRNS_to_alpha(png);
	if (colorType == PNG_COLOR_TYPE_GRAY || colorType == PNG_COLOR_TYPE_GRAY_ALPHA) png_set_gray_to_rgb(png);
	if ((colorType & PNG_COLOR_MASK_ALPHA) != 0 || hasTransparency) png_set_strip_alpha(png);
	if (sourceBitDepth == 16 && std::endian::native == std::endian::little) png_set_swap(png);
	(void)png_set_interlace_handling(png);
	png_read_update_info(png, info);

	const int bitDepth = png_get_bit_depth(png, info);
	const int channels = png_get_channels(png, info);
	const png_size_t rowBytes = png_get_rowbytes(png, info);
	if ((bitDepth != 8 && bitDepth != 16) || channels != 3 ||
	    rowBytes != static_cast<png_size_t>(width) * 3u * static_cast<unsigned int>(bitDepth / 8) ||
	    height > std::numeric_limits<std::size_t>::max() / rowBytes) {
		png_error(png, "unsupported PNG output layout");
	}
	const std::size_t byteCount = static_cast<std::size_t>(height) * rowBytes;
	allocations->pixels = std::malloc(byteCount);
	allocations->rows = static_cast<png_bytep*>(std::malloc(static_cast<std::size_t>(height) * sizeof(png_bytep)));
	if (allocations->pixels == nullptr || allocations->rows == nullptr) png_error(png, "out of memory decoding PNG");
	for (png_uint_32 y = 0; y < height; ++y) {
		allocations->rows[y] = static_cast<png_bytep>(allocations->pixels) + static_cast<std::size_t>(y) * rowBytes;
	}
	timer.measure([&] {png_read_image(png, allocations->rows); png_read_end(png, info);});
	if(codecSeconds) *codecSeconds=timer.seconds();
	PngColorMetadata colorMetadata;
	try {
		colorMetadata = read_png_color_metadata(png, info);
	} catch (...) {
		std::free(allocations->rows);
		std::free(allocations->pixels);
		std::free(allocations);
		png_destroy_read_struct(&png, &info, nullptr);
		throw;
	}
	png_destroy_read_struct(&png, &info, nullptr);
	std::free(allocations->rows);

	PngImage image;
	if (colorMetadata.kind == PngColorKind::AssumedSrgb)
		image.importNote = "No embedded color profile or PNG color description; assuming sRGB / BT.709, full-range RGB.";
	else if (colorMetadata.kind == PngColorKind::Icc)
		image.importNote = "Embedded PNG ICC profile converted to linear BT.2020.";
	else if (colorMetadata.kind == PngColorKind::Chromaticities)
		image.importNote = "PNG chromaticity/gamma description converted to linear BT.2020.";
	image.sixteenBit = bitDepth == 16;
	if (image.sixteenBit) {
		image.rgb16.width = static_cast<int>(width);
		image.rgb16.height = static_cast<int>(height);
		image.rgb16.rgb.resize(byteCount / sizeof(unsigned short));
		std::memcpy(image.rgb16.rgb.data(), allocations->pixels, byteCount);
	} else {
		image.rgb8.width = static_cast<int>(width);
		image.rgb8.height = static_cast<int>(height);
		image.rgb8.rgb.resize(byteCount);
		std::memcpy(image.rgb8.rgb.data(), allocations->pixels, byteCount);
	}
	std::free(allocations->pixels);
	std::free(allocations);
	if (colorMetadata.kind == PngColorKind::Cicp) {
		if (image.sixteenBit) {
			image.rgb16.color = colorMetadata.cicp;
			image.rgb16.nominalPeakNits = colorMetadata.nominalPeakNits;
		} else {
			image.rgb8.color = colorMetadata.cicp;
			image.rgb8.nominalPeakNits = colorMetadata.nominalPeakNits;
		}
	} else if (colorMetadata.kind == PngColorKind::Icc || colorMetadata.kind == PngColorKind::Chromaticities) {
		cmsHPROFILE sourceProfile = colorMetadata.kind == PngColorKind::Icc
			? cmsOpenProfileFromMem(colorMetadata.icc.data(), static_cast<cmsUInt32Number>(colorMetadata.icc.size()))
			: create_png_chromaticity_profile(colorMetadata);
		if (!sourceProfile) throw std::runtime_error("LittleCMS rejected the embedded PNG ICC profile");
		try {
			Rgb16Image converted = image.sixteenBit
				? convert_to_linear_bt2020(image.rgb16.width, image.rgb16.height, image.rgb16.rgb.data(), TYPE_RGB_16, sourceProfile)
				: convert_to_linear_bt2020(image.rgb8.width, image.rgb8.height, image.rgb8.rgb.data(), TYPE_RGB_8, sourceProfile);
			cmsCloseProfile(sourceProfile);
			image.sixteenBit = true;
			image.rgb16 = std::move(converted);
			image.rgb8 = {};
		} catch (...) {
			cmsCloseProfile(sourceProfile);
			throw;
		}
	}
	return image;
}

bool has_extension(const std::filesystem::path& path, const std::string& extension) {
	std::string ext = path.extension().string();
	std::ranges::transform(ext, ext.begin(), [](unsigned char c) {
		return static_cast<char>(std::tolower(c));
	});
	return ext == extension;
}

std::vector<unsigned char> jpeg_icc_profile(const jpeg_decompress_struct& cinfo) {
	constexpr unsigned char signature[] = {'I', 'C', 'C', '_', 'P', 'R', 'O', 'F', 'I', 'L', 'E', '\0'};
	unsigned int chunkCount = 0;
	std::vector<const jpeg_marker_struct*> chunks;
	for (jpeg_saved_marker_ptr marker = cinfo.marker_list; marker != nullptr; marker = marker->next) {
		if (marker->marker != JPEG_APP0 + 2 || marker->data_length < sizeof(signature) + 2 ||
		    std::memcmp(marker->data, signature, sizeof(signature)) != 0) {
			continue;
		}
		const unsigned int sequence = marker->data[sizeof(signature)];
		const unsigned int total = marker->data[sizeof(signature) + 1];
		if (sequence == 0 || total == 0 || sequence > total) throw std::runtime_error("JPEG contains malformed ICC profile chunks");
		if (chunkCount == 0) {
			chunkCount = total;
			chunks.resize(total, nullptr);
		} else if (chunkCount != total) {
			throw std::runtime_error("JPEG ICC profile chunks disagree on their total count");
		}
		if (chunks[sequence - 1] != nullptr) throw std::runtime_error("JPEG contains duplicate ICC profile chunks");
		chunks[sequence - 1] = marker;
	}
	if (chunkCount == 0) return {};
	std::size_t totalSize = 0;
	for (const jpeg_marker_struct* marker : chunks) {
		if (marker == nullptr) throw std::runtime_error("JPEG ICC profile is missing a chunk");
		totalSize += marker->data_length - sizeof(signature) - 2;
	}
	std::vector<unsigned char> profile;
	profile.reserve(totalSize);
	for (const jpeg_marker_struct* marker : chunks) {
		const unsigned char* first = marker->data + sizeof(signature) + 2;
		const unsigned char* last = marker->data + marker->data_length;
		profile.insert(profile.end(), first, last);
	}
	return profile;
}

std::variant<RgbImage, Rgb16Image> load_jpeg_rgb(std::span<const unsigned char> bytes, double* codecSeconds = nullptr) {
	if (bytes.empty()) {
		throw std::runtime_error("empty JPEG input");
	}

	jpeg_decompress_struct cinfo{};
	JpegError jerr{};
	cinfo.err = jpeg_std_error(&jerr.pub);
	jerr.pub.error_exit = jpeg_error_exit;

	if (setjmp(jerr.jump) != 0) {
		jpeg_destroy_decompress(&cinfo);
		throw std::runtime_error("JPEG decode failed: " + std::string(jerr.message));
	}

	jpeg_create_decompress(&cinfo);
	jpeg_mem_src(&cinfo, bytes.data(), static_cast<unsigned long>(bytes.size()));
	jpeg_save_markers(&cinfo, JPEG_APP0 + 2, 0xFFFF);
	if (jpeg_read_header(&cinfo, TRUE) != JPEG_HEADER_OK) {
		throw std::runtime_error("not a JPEG image");
	}
	std::vector<unsigned char> icc;
	try {
		icc = jpeg_icc_profile(cinfo);
	} catch (...) {
		jpeg_destroy_decompress(&cinfo);
		throw;
	}

	cinfo.out_color_space = JCS_RGB;
	gui::CodecTimer timer;
	timer.measure([&] {jpeg_start_decompress(&cinfo);});
	if (cinfo.output_components != 3) {
		throw std::runtime_error("JPEG decoder did not produce RGB output");
	}
	if (cinfo.output_width > static_cast<JDIMENSION>(std::numeric_limits<int>::max()) ||
	    cinfo.output_height > static_cast<JDIMENSION>(std::numeric_limits<int>::max())) {
		throw std::runtime_error("JPEG dimensions are too large");
	}

	RgbImage image;
	image.width = static_cast<int>(cinfo.output_width);
	image.height = static_cast<int>(cinfo.output_height);
	image.rgb.resize(static_cast<std::size_t>(image.width) * static_cast<std::size_t>(image.height) * 3);
	timer.measure([&] {while (cinfo.output_scanline < cinfo.output_height) {
		unsigned char* row = image.rgb.data() +
		                     static_cast<std::size_t>(cinfo.output_scanline) *
		                         static_cast<std::size_t>(image.width) * 3;
		JSAMPROW rows[] = {row};
		jpeg_read_scanlines(&cinfo, rows, 1);
	}});

	timer.measure([&] {jpeg_finish_decompress(&cinfo);});
	if(codecSeconds) *codecSeconds=timer.seconds();
	jpeg_destroy_decompress(&cinfo);
	if (!icc.empty()) {
		cmsHPROFILE sourceProfile = cmsOpenProfileFromMem(icc.data(), static_cast<cmsUInt32Number>(icc.size()));
		if (!sourceProfile) throw std::runtime_error("LittleCMS rejected the embedded JPEG ICC profile");
		try {
			Rgb16Image converted = convert_to_linear_bt2020(image.width, image.height, image.rgb.data(), TYPE_RGB_8, sourceProfile);
			cmsCloseProfile(sourceProfile);
			return converted;
		} catch (...) {
			cmsCloseProfile(sourceProfile);
			throw;
		}
	}
	return image;
}

std::optional<ColorDescription> jxl_structured_color(const JxlColorEncoding& encoding) {
	if (encoding.color_space != JXL_COLOR_SPACE_RGB && encoding.color_space != JXL_COLOR_SPACE_GRAY) {
		return std::nullopt;
	}
	if (encoding.white_point != JXL_WHITE_POINT_D65) return std::nullopt;

	ColorDescription color;
	color.matrix = MatrixCoefficients::Identity;
	color.range = ColorRange::Full;
	if (encoding.color_space == JXL_COLOR_SPACE_GRAY || encoding.primaries == JXL_PRIMARIES_SRGB) {
		color.primaries = ColorPrimaries::BT709;
	} else if (encoding.primaries == JXL_PRIMARIES_2100) {
		color.primaries = ColorPrimaries::BT2020;
	} else if (encoding.primaries == JXL_PRIMARIES_P3) {
		color.primaries = ColorPrimaries::DisplayP3;
	} else {
		return std::nullopt;
	}

	switch (encoding.transfer_function) {
		case JXL_TRANSFER_FUNCTION_709: color.transfer = TransferCharacteristics::BT709; break;
		case JXL_TRANSFER_FUNCTION_LINEAR: color.transfer = TransferCharacteristics::Linear; break;
		case JXL_TRANSFER_FUNCTION_SRGB: color.transfer = TransferCharacteristics::SRGB; break;
		case JXL_TRANSFER_FUNCTION_PQ: color.transfer = TransferCharacteristics::PQ; break;
		case JXL_TRANSFER_FUNCTION_HLG: color.transfer = TransferCharacteristics::HLG; break;
		default: return std::nullopt;
	}
	return color;
}

std::variant<RgbImage, Rgb16Image> load_jxl_rgb(std::span<const unsigned char> bytes, double* codecSeconds = nullptr) {
	if (bytes.empty()) throw std::runtime_error("empty JPEG XL input");

	std::unique_ptr<JxlDecoder, decltype(&JxlDecoderDestroy)> const decoder{
		JxlDecoderCreate(nullptr),
		JxlDecoderDestroy,
	};
	if (!decoder) throw std::runtime_error("JxlDecoderCreate failed");
	if (const JxlCmsInterface* cms = JxlGetDefaultCms()) {
		if (JxlDecoderSetCms(decoder.get(), *cms) != JXL_DEC_SUCCESS) {
			throw std::runtime_error("JPEG XL decoder rejected its default color-management system");
		}
	}
	if (JxlDecoderSubscribeEvents(
		    decoder.get(),
		    JXL_DEC_BASIC_INFO | JXL_DEC_COLOR_ENCODING | JXL_DEC_FULL_IMAGE
	    ) != JXL_DEC_SUCCESS) {
		throw std::runtime_error("JxlDecoderSubscribeEvents failed");
	}
	if (JxlDecoderSetInput(decoder.get(), bytes.data(), bytes.size()) != JXL_DEC_SUCCESS) {
		throw std::runtime_error("JxlDecoderSetInput failed");
	}
	JxlDecoderCloseInput(decoder.get());

	JxlBasicInfo basic{};
	JxlPixelFormat format{.num_channels=3, .data_type=JXL_TYPE_UINT8, .endianness=JXL_NATIVE_ENDIAN, .align=0};
	ColorDescription color;
	std::vector<unsigned char> icc;
	std::vector<unsigned char> pixels;
	bool haveBasicInfo = false;
	bool haveColor = false;
	bool useIcc = false;
	gui::CodecTimer timer;

	for (;;) {
		const JxlDecoderStatus status = timer.measure([&] {return JxlDecoderProcessInput(decoder.get());});
		if(codecSeconds) *codecSeconds=timer.seconds();
		if (status == JXL_DEC_BASIC_INFO) {
			if (JxlDecoderGetBasicInfo(decoder.get(), &basic) != JXL_DEC_SUCCESS) {
				throw std::runtime_error("JxlDecoderGetBasicInfo failed");
			}
			if (basic.xsize == 0 || basic.ysize == 0 ||
			    basic.xsize > static_cast<uint32_t>(std::numeric_limits<int>::max()) ||
			    basic.ysize > static_cast<uint32_t>(std::numeric_limits<int>::max())) {
				throw std::runtime_error("JPEG XL dimensions are invalid or too large");
			}
			if (basic.num_color_channels != 1 && basic.num_color_channels != 3) {
				throw std::runtime_error("JPEG XL input must use RGB or grayscale color channels");
			}
			if (basic.exponent_bits_per_sample != 0 || basic.bits_per_sample == 0 || basic.bits_per_sample > 16) {
				throw std::runtime_error(
					"JPEG XL import currently supports 1- to 16-bit integer samples; got " +
					std::to_string(basic.bits_per_sample) + "-bit with " +
					std::to_string(basic.exponent_bits_per_sample) + " exponent bits"
				);
			}
			format.num_channels = basic.num_color_channels;
			format.data_type = basic.bits_per_sample <= 8 ? JXL_TYPE_UINT8 : JXL_TYPE_UINT16;
			haveBasicInfo = true;
		} else if (status == JXL_DEC_COLOR_ENCODING) {
			if (!haveBasicInfo) throw std::runtime_error("JPEG XL reported color metadata before basic image information");
			JxlColorEncoding encoding{};
			if (JxlDecoderGetColorAsEncodedProfile(
				    decoder.get(),
				    JXL_COLOR_PROFILE_TARGET_ORIGINAL,
				    &encoding
			    ) == JXL_DEC_SUCCESS) {
				const std::optional<ColorDescription> mapped = jxl_structured_color(encoding);
				if (mapped) {
					color = *mapped;
					if (basic.uses_original_profile == JXL_FALSE &&
					    JxlDecoderSetPreferredColorProfile(decoder.get(), &encoding) != JXL_DEC_SUCCESS) {
						throw std::runtime_error("JPEG XL decoder could not preserve the structured source color profile");
					}
				} else {
					if (encoding.transfer_function == JXL_TRANSFER_FUNCTION_PQ ||
					    encoding.transfer_function == JXL_TRANSFER_FUNCTION_HLG) {
						throw std::runtime_error(
							"JPEG XL HDR import requires D65 BT.2020, Display P3, or BT.709 primaries"
						);
					}
					useIcc = true;
				}
			} else {
				useIcc = true;
			}
			if (useIcc) {
				size_t profileSize = 0;
				if (JxlDecoderGetICCProfileSize(
					    decoder.get(),
					    JXL_COLOR_PROFILE_TARGET_ORIGINAL,
					    &profileSize
				    ) != JXL_DEC_SUCCESS || profileSize == 0) {
					throw std::runtime_error("JPEG XL source color profile is unsupported and has no usable ICC representation");
				}
				if (profileSize > std::numeric_limits<cmsUInt32Number>::max()) {
					throw std::runtime_error("JPEG XL ICC profile is too large for LittleCMS");
				}
				icc.resize(profileSize);
				if (JxlDecoderGetColorAsICCProfile(
					    decoder.get(),
					    JXL_COLOR_PROFILE_TARGET_ORIGINAL,
					    icc.data(),
					    icc.size()
				    ) != JXL_DEC_SUCCESS) {
					throw std::runtime_error("JPEG XL decoder could not read the embedded ICC profile");
				}
				if (JxlDecoderSetOutputColorProfile(
					    decoder.get(),
					    nullptr,
					    icc.data(),
					    icc.size()
				    ) != JXL_DEC_SUCCESS) {
					throw std::runtime_error("JPEG XL decoder could not preserve the embedded ICC output profile");
				}
			}
			haveColor = true;
		} else if (status == JXL_DEC_NEED_IMAGE_OUT_BUFFER) {
			if (!haveBasicInfo || !haveColor) {
				throw std::runtime_error("JPEG XL requested an output buffer before reporting image color metadata");
			}
			size_t outputSize = 0;
			if (JxlDecoderImageOutBufferSize(decoder.get(), &format, &outputSize) != JXL_DEC_SUCCESS) {
				throw std::runtime_error("JxlDecoderImageOutBufferSize failed");
			}
			pixels.resize(outputSize);
			if (JxlDecoderSetImageOutBuffer(decoder.get(), &format, pixels.data(), pixels.size()) != JXL_DEC_SUCCESS) {
				throw std::runtime_error("JxlDecoderSetImageOutBuffer failed");
			}
		} else if (status == JXL_DEC_FULL_IMAGE) {
			if (pixels.empty() || !haveBasicInfo || !haveColor) {
				throw std::runtime_error("JPEG XL decoder produced no complete image");
			}
			const int width = static_cast<int>(basic.xsize);
			const int height = static_cast<int>(basic.ysize);
			const std::size_t pixelCount = static_cast<std::size_t>(width) * static_cast<std::size_t>(height);
			const unsigned int channels = basic.num_color_channels;
			if (useIcc) {
				cmsHPROFILE profile = cmsOpenProfileFromMem(icc.data(), static_cast<cmsUInt32Number>(icc.size()));
				if (!profile) throw std::runtime_error("LittleCMS rejected the JPEG XL ICC profile");
				try {
					const cmsUInt32Number inputFormat = channels == 1
						? (basic.bits_per_sample <= 8 ? TYPE_GRAY_8 : TYPE_GRAY_16)
						: (basic.bits_per_sample <= 8 ? TYPE_RGB_8 : TYPE_RGB_16);
					Rgb16Image converted =
						convert_to_linear_bt2020(width, height, pixels.data(), inputFormat, profile);
					cmsCloseProfile(profile);
					return converted;
				} catch (...) {
					cmsCloseProfile(profile);
					throw;
				}
			}
			if (basic.bits_per_sample <= 8) {
				RgbImage image;
				image.width = width;
				image.height = height;
				image.color = color;
				image.nominalPeakNits = color.transfer == TransferCharacteristics::HLG
					? std::optional<double>{basic.intensity_target > 0.0f ? basic.intensity_target : 1000.0}
					: std::nullopt;
				image.rgb.resize(pixelCount * 3);
				for (std::size_t i = 0; i < pixelCount; ++i) {
					for (unsigned int channel = 0; channel < 3; ++channel) {
						image.rgb[i * 3 + channel] = pixels[i * channels + (channels == 1 ? 0 : channel)];
					}
				}
				return image;
			}
			Rgb16Image image;
			image.width = width;
			image.height = height;
			image.color = color;
			image.nominalPeakNits = color.transfer == TransferCharacteristics::HLG
				? std::optional<double>{basic.intensity_target > 0.0f ? basic.intensity_target : 1000.0}
				: std::nullopt;
			image.rgb.resize(pixelCount * 3);
			for (std::size_t i = 0; i < pixelCount; ++i) {
				for (unsigned int channel = 0; channel < 3; ++channel) {
					const std::size_t inputIndex = i * channels + (channels == 1 ? 0 : channel);
					std::memcpy(
						&image.rgb[i * 3 + channel],
						pixels.data() + inputIndex * sizeof(unsigned short),
						sizeof(unsigned short)
					);
				}
			}
			return image;
		} else if (status == JXL_DEC_NEED_MORE_INPUT) {
			throw std::runtime_error("truncated JPEG XL input");
		} else if (status == JXL_DEC_ERROR) {
			throw std::runtime_error("JPEG XL decode failed");
		} else if (status == JXL_DEC_SUCCESS) {
			throw std::runtime_error("JPEG XL decoder finished without producing an image");
		}
	}
}

void check_libraw(const int rc, const std::string& operation) {
	if (rc != LIBRAW_SUCCESS) {
		throw std::runtime_error("LibRaw " + operation + " failed: " + LibRaw::strerror(rc));
	}
}

Rgb16Image load_nef_rgb16(const std::filesystem::path& path) {
	LibRaw raw;
	check_libraw(raw.open_file(path.c_str()), "open_file");
	check_libraw(raw.unpack(), "unpack");
	raw.imgdata.params.use_camera_wb = 1;
	raw.imgdata.params.output_color = 1;
	raw.imgdata.params.output_bps = 16;
	raw.imgdata.params.no_auto_bright = 1;
	check_libraw(raw.dcraw_process(), "dcraw_process");

	int err = LIBRAW_SUCCESS;
	libraw_processed_image_t* processed = raw.dcraw_make_mem_image(&err);
	check_libraw(err, "dcraw_make_mem_image");
	if (processed == nullptr) {
		throw std::runtime_error("LibRaw dcraw_make_mem_image returned no image");
	}
	struct ProcessedImageDeleter {
		void operator()(libraw_processed_image_t* image) const noexcept { LibRaw::dcraw_clear_mem(image); }
	};
	std::unique_ptr<libraw_processed_image_t, ProcessedImageDeleter> const imageGuard{processed};

	if (processed->type != LIBRAW_IMAGE_BITMAP || processed->colors != 3 || processed->bits != 16) {
		throw std::runtime_error("LibRaw produced an unsupported image format");
	}

	Rgb16Image image;
	image.width = processed->width;
	image.height = processed->height;
	const std::size_t expectedSize =
		static_cast<std::size_t>(image.width) * static_cast<std::size_t>(image.height) * 3;
	if (processed->data_size != expectedSize * sizeof(unsigned short)) {
		throw std::runtime_error("LibRaw produced an unexpected RGB buffer size");
	}
	image.rgb.resize(expectedSize);
	std::memcpy(image.rgb.data(), processed->data, processed->data_size);
	return image;
}

void store_u16le(std::vector<unsigned char>& bytes, const std::size_t sampleIndex, const unsigned short value) {
	bytes[sampleIndex * 2 + 0] = static_cast<unsigned char>(value & 0xffu);
	bytes[sampleIndex * 2 + 1] = static_cast<unsigned char>(value >> 8u);
}

RawImage rgb8_to_planar_source(const RgbImage& rgb) {
	if (rgb.width <= 0 || rgb.height <= 0) {
		throw std::runtime_error("image dimensions must be positive");
	}
	RawImage image;
	image.width = rgb.width;
	image.height = rgb.height;
	image.format = PixelFormat::RGBP8;
	image.color = rgb.color;
	image.nominalPeakNits = rgb.nominalPeakNits;
	for (ImagePlane& plane : image.planes) {
		plane.strideBytes = image.width;
		plane.bytes.resize(static_cast<std::size_t>(image.width) * image.height);
	}
	for (std::size_t i = 0, pixels = static_cast<std::size_t>(image.width) * image.height; i < pixels; ++i) {
		image.planes[0].bytes[i] = rgb.rgb[i * 3 + 0];
		image.planes[1].bytes[i] = rgb.rgb[i * 3 + 1];
		image.planes[2].bytes[i] = rgb.rgb[i * 3 + 2];
	}
	return image;
}

RawImage rgb16_to_planar_source(const Rgb16Image& rgb, int sourceBitDepth) {
	if (rgb.width <= 0 || rgb.height <= 0 || (sourceBitDepth != 14 && sourceBitDepth != 16)) {
		throw std::runtime_error("unsupported high-bit-depth RGB source");
	}
	RawImage image;
	image.width = rgb.width;
	image.height = rgb.height;
	image.format = sourceBitDepth == 14 ? PixelFormat::RGBP14LE : PixelFormat::RGBP16LE;
	image.color = rgb.color;
	image.nominalPeakNits = rgb.nominalPeakNits;
	for (ImagePlane& plane : image.planes) {
		plane.strideBytes = image.width * 2;
		plane.bytes.resize(static_cast<std::size_t>(image.width) * image.height * 2);
	}
	const uint32_t targetMaximum = (1u << sourceBitDepth) - 1u;
	for (std::size_t i = 0, pixels = static_cast<std::size_t>(image.width) * image.height; i < pixels; ++i) {
		for (int plane = 0; plane < 3; ++plane) {
			const uint32_t source = rgb.rgb[i * 3 + static_cast<std::size_t>(plane)];
			const auto value = static_cast<uint16_t>((source * targetMaximum + 32767u) / 65535u);
			store_u16le(image.planes[plane].bytes, i, value);
		}
	}
	return image;
}

void check_heif(heif_error error, const char* operation) {
    if(error.code!=heif_error_Ok)
        throw std::runtime_error(std::string("HEIF ")+operation+": "+(error.message?error.message:"unknown error"));
}

gui::DecodeResult decode_heif_container(std::span<const unsigned char> bytes) {
    struct Library {
        Library(){check_heif(heif_init(nullptr),"initialize library");}
        ~Library(){heif_deinit();}
    } library;
    std::unique_ptr<heif_context,decltype(&heif_context_free)> context(heif_context_alloc(),heif_context_free);
    if(!context)throw std::bad_alloc();
    check_heif(heif_context_read_from_memory_without_copy(context.get(),bytes.data(),bytes.size(),nullptr),"read container");
    heif_item_id primary=0;
    check_heif(heif_context_get_primary_image_ID(context.get(),&primary),"find primary image");
    const auto type=heif_item_get_item_type(context.get(),primary);
    if(type==heif_item_type_grid)throw std::runtime_error("HEIF grid images are not supported; import a single coded image");
    heif_image_handle* handleRaw=nullptr;
    check_heif(heif_context_get_primary_image_handle(context.get(),&handleRaw),"open primary image");
    std::unique_ptr<heif_image_handle,decltype(&heif_image_handle_release)> handle(handleRaw,heif_image_handle_release);
    const auto iccSize=heif_image_handle_get_raw_color_profile_size(handle.get());
    std::vector<uint8_t> icc(iccSize);
    if(iccSize)check_heif(heif_image_handle_get_raw_color_profile(handle.get(),icc.data()),"read ICC profile");
    heif_colorspace requested=heif_colorspace_undefined;
    heif_chroma requestedChroma=heif_chroma_undefined;
    if(iccSize){requested=heif_colorspace_RGB;requestedChroma=heif_chroma_444;}
    else {
        // In particular, identity-matrix HEVC carries RGB in GBR plane order.
        // Let libheif expose those as actual RGB rather than mislabelling them.
        check_heif(heif_image_handle_get_preferred_decoding_colorspace(handle.get(),&requested,&requestedChroma),"inspect pixel format");
        if(requested==heif_colorspace_RGB)requestedChroma=heif_chroma_444;
    }
    std::unique_ptr<heif_decoding_options,decltype(&heif_decoding_options_free)>
        options(heif_decoding_options_alloc(),heif_decoding_options_free);
    if(!options)throw std::bad_alloc();
    if(options->version<10)throw std::runtime_error("HEIF import requires libheif with NCLX passthrough decoding options");
    options->output_image_nclx_profile_passthrough=1;
    options->convert_hdr_to_8bit=0;
    options->strict_decoding=1;
    options->ignore_transformations=0;
    options->autocorrect_broken_input=0;
    heif_image* decodedRaw=nullptr;
    gui::CodecTimer timer;
    auto error=timer.measure([&]{return heif_decode_image(handle.get(),&decodedRaw,requested,requestedChroma,options.get());});
    std::unique_ptr<heif_image,decltype(&heif_image_release)> decoded(decodedRaw,heif_image_release);
    check_heif(error,"decode primary image");
    if(!decoded)throw std::runtime_error("HEIF decoder returned no image");
    auto image=std::make_shared<RawImage>();
    image->width=heif_image_get_primary_width(decoded.get());
    image->height=heif_image_get_primary_height(decoded.get());
    if(image->width<=0||image->height<=0)throw std::runtime_error("HEIF decoded image dimensions are invalid");
    const auto space=heif_image_get_colorspace(decoded.get());
    const auto chroma=heif_image_get_chroma_format(decoded.get());
    const bool rgb=space==heif_colorspace_RGB,gray=space==heif_colorspace_monochrome;
    if(!rgb&&!gray&&space!=heif_colorspace_YCbCr)throw std::runtime_error("Unsupported HEIF decoded colorspace");
    if(rgb&&chroma!=heif_chroma_444)throw std::runtime_error("HEIF decoder did not return planar RGB");
    const heif_channel channelsRGB[]={heif_channel_R,heif_channel_G,heif_channel_B};
    const heif_channel channelsYUV[]={heif_channel_Y,heif_channel_Cb,heif_channel_Cr};
    const auto* channels=rgb?channelsRGB:channelsYUV;
    const int depth=heif_image_get_bits_per_pixel_range(decoded.get(),channels[0]);
    auto choose=[&](PixelFormat a,PixelFormat b,PixelFormat c,PixelFormat d){
        switch(depth){case 8:return a;case 10:return b;case 12:return c;case 14:return d;
            default:throw std::runtime_error("Unsupported HEIF channel precision: "+std::to_string(depth));}
    };
    if(rgb){
        if(depth==8)image->format=PixelFormat::RGBP8;
        else if(depth==14)image->format=PixelFormat::RGBP14LE;
        else if(depth==10||depth==12||depth==16)image->format=PixelFormat::RGBP16LE;
        else throw std::runtime_error("Unsupported HEIF RGB precision");
    }else if(gray)image->format=choose(PixelFormat::Gray8,PixelFormat::Gray10LE,PixelFormat::Gray12LE,PixelFormat::Gray14LE);
    else if(chroma==heif_chroma_420)image->format=choose(PixelFormat::YUV420P8,PixelFormat::YUV420P10LE,PixelFormat::YUV420P12LE,PixelFormat::YUV420P14LE);
    else if(chroma==heif_chroma_422)image->format=choose(PixelFormat::YUV422P8,PixelFormat::YUV422P10LE,PixelFormat::YUV422P12LE,PixelFormat::YUV422P14LE);
    else if(chroma==heif_chroma_444)image->format=depth==16?PixelFormat::YUV444P16LE:choose(PixelFormat::YUV444P8,PixelFormat::YUV444P10LE,PixelFormat::YUV444P12LE,PixelFormat::YUV444P14LE);
    else throw std::runtime_error("Unsupported HEIF chroma subsampling");
    const int bps=depth<=8?1:2;
    for(int p=0;p<(gray?1:3);++p){
        const auto channel=channels[p];
        const int width=heif_image_get_width(decoded.get(),channel),height=heif_image_get_height(decoded.get(),channel);
        const int expectedW=p&&!rgb&&chroma!=heif_chroma_444?(image->width+1)/2:image->width;
        const int expectedH=p&&!rgb&&chroma==heif_chroma_420?(image->height+1)/2:image->height;
        if(width!=expectedW||height!=expectedH||heif_image_get_bits_per_pixel_range(decoded.get(),channel)!=depth)
            throw std::runtime_error("HEIF decoded planes have inconsistent geometry or precision");
        std::size_t stride=0;
        const auto* source=heif_image_get_plane_readonly2(decoded.get(),channel,&stride);
        const auto rowBytes=std::size_t(width)*bps;
        if(!source||stride<rowBytes||std::size_t(height)>std::numeric_limits<std::size_t>::max()/stride)
            throw std::runtime_error("HEIF decoded plane is incomplete");
        auto& plane=image->planes[p];plane.strideBytes=rowBytes;plane.bytes.resize(rowBytes*height);
        const uint32_t sourceMax=(1u<<depth)-1,targetMax=gui::max_sample_value(image->format);
        for(int y=0;y<height;++y){
            if(bps==1)std::memcpy(plane.bytes.data()+y*rowBytes,source+y*stride,rowBytes);
            else for(int x=0;x<width;++x){
                uint16_t v;std::memcpy(&v,source+y*stride+x*2,2); // libheif planar words are native endian.
                if(v>sourceMax)throw std::runtime_error("HEIF sample exceeds the declared precision");
                const uint32_t value=rgb&&sourceMax!=targetMax?(uint64_t(v)*targetMax+sourceMax/2)/sourceMax:v;
                plane.bytes[y*rowBytes+x*2]=value&255;plane.bytes[y*rowBytes+x*2+1]=value>>8;
            }
        }
    }
    heif_color_profile_nclx* nclxRaw=nullptr;
    auto colorError=heif_image_get_nclx_color_profile(decoded.get(),&nclxRaw);
    if(colorError.code==heif_error_Color_profile_does_not_exist)
        colorError=heif_image_handle_get_nclx_color_profile(handle.get(),&nclxRaw);
    std::unique_ptr<heif_color_profile_nclx,decltype(&heif_nclx_color_profile_free)> nclx(nclxRaw,heif_nclx_color_profile_free);
    if(colorError.code==heif_error_Ok&&nclx){
        image->color={.primaries=static_cast<ColorPrimaries>(nclx->color_primaries),
            .transfer=static_cast<TransferCharacteristics>(nclx->transfer_characteristics),
            .matrix=rgb?MatrixCoefficients::Identity:static_cast<MatrixCoefficients>(nclx->matrix_coefficients),
            .range=rgb||nclx->full_range_flag?ColorRange::Full:ColorRange::Limited};
    }else if(colorError.code==heif_error_Color_profile_does_not_exist){
        image->color={.primaries=ColorPrimaries::BT709,.transfer=TransferCharacteristics::SRGB,
            .matrix=rgb?MatrixCoefficients::Identity:MatrixCoefficients::BT601,.range=ColorRange::Full};
        image->importNote="No HEIF NCLX profile; using libheif's default color interpretation. ";
    }else check_heif(colorError,"read decoded color profile");
    image->importNote="Imported the HEIF primary image (item "+std::to_string(primary)+"). "+image->importNote;
    if(rgb&&depth!=gui::bit_depth(image->format))image->importNote+="Native "+std::to_string(depth)+"-bit RGB expanded to 16-bit storage without reducing precision. ";
    if(heif_image_handle_has_alpha_channel(handle.get()))image->importNote+="Alpha is not retained; comparison uses the color planes. ";
    const int warnings=heif_image_get_decoding_warnings(decoded.get(),0,nullptr,0);
    for(int i=0;i<warnings;++i){heif_error warning{};
        if(heif_image_get_decoding_warnings(decoded.get(),i,&warning,1)>0)
            image->importNote+=std::string("HEIF warning: ")+(warning.message?warning.message:"unspecified")+". ";}
    return {.image=std::move(image),.error={},.iccProfile=std::move(icc),.codecSeconds=timer.seconds()};
}

} // namespace

std::vector<unsigned char> read_file_bytes(const std::filesystem::path& path) {
	std::ifstream in(path, std::ios::binary);
	if (!in) {
		throw std::runtime_error("failed to open input file: " + path.string());
	}
	return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

void dump_to_file(const std::filesystem::path& path, const std::vector<std::byte>& data) {
	std::filesystem::remove(path);
	std::ofstream out(path, std::ios::binary);
	if (!out) {
		throw std::runtime_error("failed to open output file: " + path.string());
	}
	out.write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size()));
	if (!out) {
		throw std::runtime_error("failed to write output file: " + path.string());
	}
}

RawImage load_input_image(const std::filesystem::path& path) {
	if (has_extension(path, ".nef")) {
		return rgb16_to_planar_source(load_nef_rgb16(path), 14);
	}
	const auto bytes=read_file_bytes(path);
	try {return decode_input_image(bytes,path.filename().string());}
	catch(const std::exception& e) {throw std::runtime_error("Import " + path.filename().string() + ": " + e.what());}
}

RawImage decode_input_image(std::span<const unsigned char> bytes, std::string_view filenameHint, std::optional<double>* codecSeconds) {
	using namespace gui;
	if(codecSeconds) codecSeconds->reset();
	if(bytes.empty()) throw std::runtime_error("Empty image input");
	std::string extension=std::filesystem::path(filenameHint).extension().string();
	std::transform(extension.begin(),extension.end(),extension.begin(),[](unsigned char c){return std::tolower(c);});
	auto matches=[&](std::initializer_list<const char*> values) {return std::any_of(values.begin(),values.end(),[&](const char* s){return extension==s;});};
	auto signature=[&](std::size_t at,std::string_view data) {return at<=bytes.size() && data.size()<=bytes.size()-at && std::memcmp(bytes.data()+at,data.data(),data.size())==0;};
	enum class Format {Unknown,Png,Jpeg,Jxl,JpegLs,Jpeg2000,JpegXr,JpegXs,Webp,Ivf,H264,Hevc,Vvc,Heif};
	Format format=Format::Unknown;
	if(signature(0,std::string_view("\x89PNG\r\n\x1a\n",8))) format=Format::Png;
	else if(signature(0,std::string_view("\xff\xd8",2))) {
		format=Format::Jpeg;
		// Walk JPEG marker lengths: APP/ICC payloads may themselves contain
		// 0xfff7, which is not a JPEG-LS SOF unless it is a real marker.
		std::size_t at=2;
		while(at<bytes.size() && bytes[at]==0xff) {
			while(at<bytes.size() && bytes[at]==0xff) ++at;
			if(at==bytes.size()) break;
			const int marker=bytes[at++];
			if(marker==0xf7) {format=Format::JpegLs;break;}
			if(marker==0xda || marker==0xd9) break;
			if(marker==0xd8 || marker==0x01 || (marker>=0xd0 && marker<=0xd7)) continue;
			if(bytes.size()-at<2) break;
			const std::size_t size=(std::size_t(bytes[at])<<8)|bytes[at+1];
			if(size<2 || size>bytes.size()-at) break;
			at+=size;
		}
	}
	else if(signature(0,std::string_view("\xff\x0a",2)) || signature(0,std::string_view("\0\0\0\x0cJXL \r\n\x87\n",12))) format=Format::Jxl;
	else if(signature(0,"RIFF") && signature(8,"WEBP")) format=Format::Webp;
	else if(signature(0,std::string_view("\0\0\0\x0cjP  \r\n\x87\n",12)) || signature(0,std::string_view("\xff\x4f\xff\x51",4))) format=Format::Jpeg2000;
	else if(signature(0,std::string_view("II\xbc\x01",4))) format=Format::JpegXr;
	else if(signature(0,std::string_view("\xff\x10\xff\x50",4))) format=Format::JpegXs;
	else if(signature(0,"DKIF")) format=Format::Ivf;
    if(format==Format::Unknown){
        const auto kind=bytes.size()<12?heif_filetype_no:heif_check_filetype(bytes.data(),int(std::min<std::size_t>(bytes.size(),std::numeric_limits<int>::max())));
        if(kind==heif_filetype_yes_supported||kind==heif_filetype_yes_unsupported)format=Format::Heif;
    }
	if(format==Format::Unknown) {
        if(matches({".heif",".heic",".hif",".avif",".vvic"}))format=Format::Heif;
		else if(matches({".h264",".264",".avc"})) format=Format::H264;
		else if(matches({".h265",".265",".hevc"})) format=Format::Hevc;
		else if(matches({".h266",".266",".vvc"})) format=Format::Vvc;
		else if(matches({".jxs"})) format=Format::JpegXs;
		else if(matches({".jls"})) format=Format::JpegLs;
		else if(matches({".jp2",".j2k",".j2c",".jpc"})) format=Format::Jpeg2000;
		else if(matches({".jxr",".wdp",".hdp"})) format=Format::JpegXr;
		else if(matches({".png"})) format=Format::Png;
		else if(matches({".jpg",".jpeg"})) format=Format::Jpeg;
		else if(matches({".jxl"})) format=Format::Jxl;
		else if(matches({".webp"})) format=Format::Webp;
		else if(matches({".ivf"})) format=Format::Ivf;
	}
	auto retain=[&](RawImage image) {
        auto compressed=std::make_shared<EncodedImage>();
        compressed->hevcAnnexB.resize(bytes.size());
        std::memcpy(compressed->hevcAnnexB.data(),bytes.data(),bytes.size());
        compressed->codedColor=image.color;
        const char* names[]={"","PNG","JPEG","JPEG XL","JPEG-LS","JPEG 2000","JPEG XR","JPEG XS","WebP","IVF","H.264","HEVC","VVC","HEIF"};
        image.compressedCodec=names[static_cast<int>(format)];
        if(format==Format::Ivf) {
            if(signature(8,"AV01")) image.compressedCodec="AV1";
            if(signature(8,"AV02")) image.compressedCodec="AV2";
            if(signature(8,"VP80")) image.compressedCodec="VP8";
            if(signature(8,"VP90")) image.compressedCodec="VP9";
        }
        image.compressedSource=std::move(compressed);return image;
    };
	if (format==Format::Png) {
		double seconds;
		PngImage const png = load_png_rgb(bytes,&seconds);
		if(codecSeconds) *codecSeconds=seconds;
		auto image = png.sixteenBit
			? rgb16_to_planar_source(png.rgb16, 16)
			: rgb8_to_planar_source(png.rgb8);
		image.importNote = png.importNote;
		return retain(std::move(image));
	}
	if (format==Format::Jpeg) {
		double seconds;
		std::variant<RgbImage, Rgb16Image> jpeg = load_jpeg_rgb(bytes,&seconds);
		if(codecSeconds) *codecSeconds=seconds;
		if (const auto* rgb8 = std::get_if<RgbImage>(&jpeg)) return retain(rgb8_to_planar_source(*rgb8));
		return retain(rgb16_to_planar_source(std::get<Rgb16Image>(jpeg), 16));
	}
	if (format==Format::Jxl) {
		double seconds;
		std::variant<RgbImage, Rgb16Image> jxl = load_jxl_rgb(bytes,&seconds);
		if(codecSeconds) *codecSeconds=seconds;
		if (const auto* rgb8 = std::get_if<RgbImage>(&jxl)) return retain(rgb8_to_planar_source(*rgb8));
		return retain(rgb16_to_planar_source(std::get<Rgb16Image>(jxl), 16));
	}
	EncodedImage encoded;
	encoded.hevcAnnexB.resize(bytes.size());
	std::memcpy(encoded.hevcAnnexB.data(),bytes.data(),bytes.size());
	DecodeResult decoded;
	bool rgb=false, sequence=false;
	switch(format) {
		case Format::JpegLs: decoded=decode_rgb_source(encoded,RgbSourceCodec::JpegLs);rgb=true;break;
		case Format::Jpeg2000: decoded=decode_rgb_source(encoded,RgbSourceCodec::Jpeg2000);rgb=true;break;
		case Format::JpegXr: decoded=decode_rgb_source(encoded,RgbSourceCodec::JpegXr);rgb=true;break;
		case Format::Webp: {
			decoded=decode_webp_preview(encoded);rgb=true;
			// RIFF ICCP chunks carry the actual RGB profile. Do not ignore them
			// just because the pixel decoder itself only returns RGB samples.
			for(std::size_t at=12;at+8<=bytes.size();) {
				uint32_t size=0;for(int i=0;i<4;++i) size|=uint32_t(bytes[at+4+i])<<(8*i);
				if(size>bytes.size()-at-8) throw std::runtime_error("Truncated WebP metadata chunk");
				if(signature(at,"ICCP")) decoded.iccProfile.assign(bytes.begin()+at+8,bytes.begin()+at+8+size);
				at+=8+std::size_t(size)+(size&1);
			}
			break;
		}
        case Format::Heif: decoded=decode_heif_container(bytes);break;
		case Format::JpegXs: decoded=decode_jpegxs_preview(encoded);break;
		case Format::H264: decoded=decode_h264_preview(encoded);sequence=true;break;
		case Format::Hevc: decoded=decode_hevc_preview(encoded);sequence=true;break;
		case Format::Vvc: decoded=decode_vvc_preview(encoded);sequence=true;break;
		case Format::Ivf:
			sequence=true;
			if(signature(8,"AV01")) decoded=decode_av1_preview(encoded);
			else if(signature(8,"AV02")) decoded=decode_av2_preview(encoded);
			else if(signature(8,"VP80")) decoded=decode_vp8_preview(encoded);
			else if(signature(8,"VP90")) decoded=decode_vp9_preview(encoded);
			else throw std::runtime_error("Unsupported IVF codec; expected AV01, AV02, VP80, or VP90");
			break;
		default: throw std::runtime_error("Unsupported input format; supported: PNG, JPEG, JPEG XL, WebP, JPEG-LS, JPEG 2000, JPEG XR, JPEG XS, HEIF/HEIC/AVIF/VVIC (non-grid primary images), H.264/HEVC/VVC Annex B, and AV1/AV2/VP8/VP9 IVF");
	}
	if(!decoded.image) throw std::runtime_error(decoded.error.empty() ? "Decoder returned no image" : decoded.error);
	if(codecSeconds) *codecSeconds=decoded.codecSeconds;
	RawImage image=*decoded.image;
	if(sequence) image.importNote="Imported the first decoded frame. ";
	if(!decoded.iccProfile.empty()) {
		std::unique_ptr<void,decltype(&cmsCloseProfile)> profile(cmsOpenProfileFromMem(decoded.iccProfile.data(),decoded.iccProfile.size()),cmsCloseProfile);
		if(!profile) throw std::runtime_error("Invalid embedded ICC profile");
		const int bps=bytes_per_sample(image.format);
		const bool gray=cmsGetColorSpace(profile.get())==cmsSigGrayData;
		if(cmsGetColorSpace(profile.get())!=cmsSigRgbData && (!gray || format==Format::Webp))
			throw std::runtime_error("Embedded source ICC profile must describe RGB or grayscale samples");
		const int channels=gray ? 1 : 3;
		std::vector<uint16_t> interleaved(std::size_t(image.width)*image.height*channels);
		const uint32_t maximum=max_sample_value(image.format);
		for(int y=0;y<image.height;++y) for(int x=0;x<image.width;++x) for(int p=0;p<channels;++p)
			interleaved[(std::size_t(y)*image.width+x)*channels+p]=(uint64_t(sample_at(image.planes[p],x,y,bps))*65535+maximum/2)/maximum;
        const auto note=image.importNote;
		image=rgb16_to_planar_source(convert_to_linear_bt2020(image.width,image.height,interleaved.data(),gray ? TYPE_GRAY_16 : TYPE_RGB_16,profile.get()),16);
        image.importNote=note+"Embedded ICC profile converted to linear BT.2020 using the shared import color-management path.";
	} else if(rgb && format!=Format::Webp) image.importNote+="No embedded color profile; assuming sRGB / BT.709, full-range RGB.";
	if(image.color.primaries==ColorPrimaries::Unspecified || image.color.transfer==TransferCharacteristics::Unspecified || image.color.matrix==MatrixCoefficients::Unspecified) {
		image.importNote+="Missing color-description fields: using BT.709 primaries/transfer and the matching YCbCr matrix; signaled fields are retained.";
		if(image.color.primaries==ColorPrimaries::Unspecified) image.color.primaries=ColorPrimaries::BT709;
		if(image.color.transfer==TransferCharacteristics::Unspecified) image.color.transfer=TransferCharacteristics::BT709;
		if(image.color.matrix==MatrixCoefficients::Unspecified) image.color.matrix=image.color.primaries==ColorPrimaries::BT2020 ? MatrixCoefficients::BT2020NonConstant : MatrixCoefficients::BT709;
	}
	return retain(std::move(image));
}

RawImage make_test_pattern() {
	RawImage image;
	constexpr unsigned int height = 1080;
	constexpr unsigned int width = 1920;
	image.width = width;
	image.height = height;
	image.format = PixelFormat::YUV420P8;
	image.color = {.primaries=ColorPrimaries::BT709, .transfer=TransferCharacteristics::SRGB, .matrix=MatrixCoefficients::BT709, .range=ColorRange::Limited, .chroma420Location=Chroma420SampleLocation::LeftCenter};
	image.planes[0].bytes.resize(static_cast<std::size_t>(height * width));
	image.planes[0].strideBytes = width;
	image.planes[1].bytes.resize(height * width / 4);
	image.planes[1].strideBytes = width / 2;
	image.planes[2].bytes.resize(height * width / 4);
	image.planes[2].strideBytes = width / 2;

	for (unsigned int y = 0; y < height; ++y) {
		for (unsigned int x = 0; x < width; ++x) {
			image.planes[0].bytes[y * width + x] =
				std::clamp(
					(std::sin(static_cast<float>(x) / static_cast<float>(height) * 4) / 2.0f + 0.5f) *
						(std::sin(static_cast<float>(y) / static_cast<float>(height) * 4) / 2.0f + 0.5f),
					0.0f,
					std::bit_cast<float>(std::bit_cast<unsigned int>(1.0f) - 1)
				) *
				256;
		}
	}

	for (unsigned int y = 0; y < height / 2; ++y) {
		for (unsigned int x = 0; x < width / 2; ++x) {
			image.planes[1].bytes[y * (width / 2) + x] =
				std::clamp(static_cast<float>(x) / static_cast<float>(width / 2), 0.0f, std::bit_cast<float>(std::bit_cast<unsigned int>(1.0f) - 1)) * 256;
			image.planes[2].bytes[y * (width / 2) + x] =
				std::clamp(static_cast<float>(y) / static_cast<float>(height / 2), 0.0f, std::bit_cast<float>(std::bit_cast<unsigned int>(1.0f) - 1)) * 256;
		}
	}
	return image;
}

} // namespace codec_gui
