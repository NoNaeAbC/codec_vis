#include "preview_decoders.hpp"
#include "scc_decode.hpp"
#include "../codec_gui_jpegxs.hpp"
#include "../codec_gui_extra.hpp"
#include "dav2d_bridge.h"
#include "raw_image_utils.hpp"
#include "codec_timer.hpp"

#include <algorithm>
#include <csetjmp>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <cerrno>
#include <cmath>
#include <cstdio>
#include <exception>
#include <ios>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include <charls/charls_jpegls_decoder.h>
#include <jxl/decode.h>
#include <openjpeg.h>
#include <png.h>
#define GST_USE_UNSTABLE_API
#include <gst/codecparsers/gsth265parser.h>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/error.h>
#include <libavutil/hwcontext.h>
#include <libavutil/hwcontext_vaapi.h>
#include <libavutil/hwcontext_vulkan.h>
#include <libavutil/pixfmt.h>
#include <libavutil/pixdesc.h>
#include <dav1d/dav1d.h>
#include <jpeglib.h>
#include <JXRGlue.h>
#include <windowsmediaphoto.h>
#include <libde265/de265.h>
#include <vvdec/vvdec.h>
}

#ifdef min
#undef min
#endif
#ifdef max
#undef max
#endif

namespace codec_gui::gui {
namespace {

void checked_de265(de265_error err, const char* operation) {
	if (!de265_isOK(err)) {
		throw std::runtime_error(std::string(operation) + " failed: " + de265_get_error_text(err));
	}
}

uint16_t read_u16le(const std::byte* p) {
	return static_cast<uint16_t>(std::to_integer<uint8_t>(p[0]) | (std::to_integer<uint8_t>(p[1]) << 8u));
}

uint32_t read_u32le(const std::byte* p) {
	uint32_t out = 0;
	for (int i = 0; i < 4; ++i) {
		out |= static_cast<uint32_t>(std::to_integer<uint8_t>(p[i])) << (i * 8);
	}
	return out;
}

struct MatrixWeights {
	double kr;
	double kb;
};

MatrixWeights preview_matrix_weights(MatrixCoefficients matrix) {
	switch (matrix) {
		case MatrixCoefficients::BT709: return {.kr=0.2126, .kb=0.0722};
		case MatrixCoefficients::BT601: return {.kr=0.2990, .kb=0.1140};
		case MatrixCoefficients::BT2020NonConstant: return {.kr=0.2627, .kb=0.0593};
		default: throw std::invalid_argument("JPEG XL preview cannot reconstruct the selected YCbCr matrix");
	}
}

PixelFormat yuv444_format_for_depth(int bitDepth) {
	if (bitDepth == 8) return PixelFormat::YUV444P8;
	if (bitDepth == 10) return PixelFormat::YUV444P10LE;
	if (bitDepth == 12) return PixelFormat::YUV444P12LE;
	if (bitDepth == 14) return PixelFormat::YUV444P14LE;
	if (bitDepth == 16) return PixelFormat::YUV444P16LE;
	throw std::invalid_argument("preview supports integer 8-, 10-, 12-, 14-, and 16-bit images; codestream reports " + std::to_string(bitDepth) + " bits");
}

PixelFormat gray_format_for_depth(int bitDepth) {
	if (bitDepth == 8) return PixelFormat::Gray8;
	if (bitDepth == 10) return PixelFormat::Gray10LE;
	if (bitDepth == 12) return PixelFormat::Gray12LE;
	if (bitDepth == 14) return PixelFormat::Gray14LE;
	throw std::invalid_argument("JPEG XL preview supports integer 8-, 10-, 12-, and 14-bit images; codestream reports " + std::to_string(bitDepth) + " bits");
}

uint16_t interleaved_sample(const std::vector<uint8_t>& pixels, std::size_t sample, int bytesPerSample) {
	if (bytesPerSample == 1) return pixels.at(sample);
	const std::size_t offset = sample * 2;
	if (offset + 1 >= pixels.size()) throw std::runtime_error("JPEG XL decoder returned a truncated pixel buffer");
	return static_cast<uint16_t>(pixels[offset] | (static_cast<uint16_t>(pixels[offset + 1]) << 8u));
}

void store_preview_sample(ImagePlane& plane, std::size_t sample, uint16_t value, int bytesPerSample) {
	if (bytesPerSample == 1) {
		plane.bytes[sample] = static_cast<uint8_t>(value);
	} else {
		const std::size_t offset = sample * 2;
		plane.bytes[offset] = static_cast<uint8_t>(value & 0xffu);
		plane.bytes[offset + 1] = static_cast<uint8_t>(value >> 8u);
	}
}

std::shared_ptr<RawImage> jxl_pixels_to_preview(
	const std::vector<uint8_t>& pixels,
	int width,
	int height,
	int bitDepth,
	uint32_t channels,
	const ColorDescription& color,
	double inputMaximum
) {
	if (channels != 1 && channels != 3) throw std::invalid_argument("unsupported JPEG XL color channel count: " + std::to_string(channels));
	const int bytesPerSample = bitDepth == 8 ? 1 : 2;
	const std::size_t expected = static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * channels * bytesPerSample;
	if (pixels.size() < expected) throw std::runtime_error("JPEG XL decoder pixel buffer is shorter than the declared image dimensions");
	const auto maximum = static_cast<double>((1u << bitDepth) - 1u);
	// libjxl's default UINT16 output uses the full 0..65535 container range,
	// independently of the codestream precision. Converting that normalized
	// value back to the declared depth preserves all 10/12/14-bit information.
	const auto scale = static_cast<double>(1u << (bitDepth - 8));
	const double yOffset = color.range == ColorRange::Full ? 0.0 : 16.0 * scale;
	const double yScale = color.range == ColorRange::Full ? maximum : 219.0 * scale;
	const auto cOffset = static_cast<double>(1u << (bitDepth - 1));
	const double cScale = color.range == ColorRange::Full ? maximum : 224.0 * scale;

	auto image = std::make_shared<RawImage>();
	image->width = width;
	image->height = height;
	image->format = channels == 1 ? gray_format_for_depth(bitDepth) : yuv444_format_for_depth(bitDepth);
	image->color = color;
	const int planeCount = channels == 1 ? 1 : 3;
	for (int plane = 0; plane < planeCount; ++plane) {
		image->planes[plane].strideBytes = width * bytesPerSample;
		image->planes[plane].bytes.resize(static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * bytesPerSample);
	}
	auto quantize = [&](double value) {
		return static_cast<uint16_t>(std::clamp<long>(std::lround(value), 0, static_cast<long>(maximum)));
	};
	if (channels == 1) {
		for (std::size_t pixel = 0; pixel < static_cast<std::size_t>(width) * height; ++pixel) {
			const double y = static_cast<double>(interleaved_sample(pixels, pixel, bytesPerSample)) / inputMaximum;
			store_preview_sample(image->planes[0], pixel, quantize(yOffset + yScale * y), bytesPerSample);
		}
		return image;
	}

	const MatrixWeights weights = preview_matrix_weights(color.matrix);
	for (std::size_t pixel = 0; pixel < static_cast<std::size_t>(width) * height; ++pixel) {
		const double r = static_cast<double>(interleaved_sample(pixels, pixel * 3 + 0, bytesPerSample)) / inputMaximum;
		const double g = static_cast<double>(interleaved_sample(pixels, pixel * 3 + 1, bytesPerSample)) / inputMaximum;
		const double b = static_cast<double>(interleaved_sample(pixels, pixel * 3 + 2, bytesPerSample)) / inputMaximum;
		const double y = weights.kr * r + (1.0 - weights.kr - weights.kb) * g + weights.kb * b;
		const double cb = (b - y) / (2.0 * (1.0 - weights.kb));
		const double cr = (r - y) / (2.0 * (1.0 - weights.kr));
		store_preview_sample(image->planes[0], pixel, quantize(yOffset + yScale * y), bytesPerSample);
		store_preview_sample(image->planes[1], pixel, quantize(cOffset + cScale * cb), bytesPerSample);
		store_preview_sample(image->planes[2], pixel, quantize(cOffset + cScale * cr), bytesPerSample);
	}
	return image;
}

ColorDescription rgb_preview_color(const EncodedImage& encoded) {
	if (encoded.codedColor) return *encoded.codedColor;
	ColorDescription color;
	color.primaries = ColorPrimaries::BT709;
	color.transfer = TransferCharacteristics::SRGB;
	color.matrix = MatrixCoefficients::BT709;
	color.range = ColorRange::Limited;
	return color;
}

std::shared_ptr<const RawImage> decoded_rgb_pixels(const std::vector<uint8_t>& pixels,
	int width, int height, int depth, int channels, const EncodedImage& encoded, bool source) {
	const uint32_t maximum=(1u<<depth)-1;
	if(!source) return jxl_pixels_to_preview(pixels,width,height,depth,channels,rgb_preview_color(encoded),maximum);
	const int inputBytes=depth<=8 ? 1 : 2;
	const int outputDepth=depth<=8 ? 8 : depth==14 ? 14 : 16;
	const int outputBytes=outputDepth<=8 ? 1 : 2;
	const uint32_t outputMaximum=(1u<<outputDepth)-1;
	if(width<=0 || height<=0 || (channels!=1 && channels!=3) || pixels.size()<std::size_t(width)*height*channels*inputBytes)
		throw std::runtime_error("Invalid decoded RGB source layout");
	auto image=std::make_shared<RawImage>(); image->width=width; image->height=height;
	image->format=outputDepth==8 ? PixelFormat::RGBP8 : outputDepth==14 ? PixelFormat::RGBP14LE : PixelFormat::RGBP16LE;
	image->color={.primaries=ColorPrimaries::BT709,.transfer=TransferCharacteristics::SRGB,.matrix=MatrixCoefficients::Identity,.range=ColorRange::Full};
	for(auto& p:image->planes) {p.strideBytes=width*outputBytes; p.bytes.resize(std::size_t(p.strideBytes)*height);}
	for(std::size_t i=0;i<std::size_t(width)*height;++i) for(int p=0;p<3;++p) {
		const uint32_t v=interleaved_sample(pixels,i*channels+(channels==1 ? 0 : p),inputBytes);
		store_preview_sample(image->planes[p],i,(uint64_t(v)*outputMaximum+maximum/2)/maximum,outputBytes);
	}
	return image;
}

ColorDescription color_description_from_jxl(const JxlColorEncoding& profile) {
	ColorDescription color;
	color.range = ColorRange::Full;
	color.chroma420Location.reset();
	switch (profile.primaries) {
		case JXL_PRIMARIES_SRGB: color.primaries = ColorPrimaries::BT709; break;
		case JXL_PRIMARIES_2100: color.primaries = ColorPrimaries::BT2020; break;
		case JXL_PRIMARIES_P3: color.primaries = profile.white_point == JXL_WHITE_POINT_DCI ? ColorPrimaries::DCIP3 : ColorPrimaries::DisplayP3; break;
		default: color.primaries = ColorPrimaries::Unspecified; break;
	}
	switch (profile.transfer_function) {
		case JXL_TRANSFER_FUNCTION_709: color.transfer = TransferCharacteristics::BT709; break;
		case JXL_TRANSFER_FUNCTION_SRGB: color.transfer = TransferCharacteristics::SRGB; break;
		case JXL_TRANSFER_FUNCTION_LINEAR: color.transfer = TransferCharacteristics::Linear; break;
		case JXL_TRANSFER_FUNCTION_PQ: color.transfer = TransferCharacteristics::PQ; break;
		case JXL_TRANSFER_FUNCTION_HLG: color.transfer = TransferCharacteristics::HLG; break;
		default: color.transfer = TransferCharacteristics::Unspecified; break;
	}
	color.matrix = color.primaries == ColorPrimaries::BT2020 ? MatrixCoefficients::BT2020NonConstant : MatrixCoefficients::BT709;
	return color;
}

std::vector<uint8_t> bytes_to_u8(const std::vector<std::byte>& bytes) {
	std::vector<uint8_t> out(bytes.size());
	std::memcpy(out.data(), bytes.data(), bytes.size());
	return out;
}

struct PngMemoryReader {
	const uint8_t* data = nullptr;
	std::size_t size = 0;
	std::size_t offset = 0;
};

void png_read_callback(png_structp png, png_bytep data, png_size_t length) {
	auto* reader = static_cast<PngMemoryReader*>(png_get_io_ptr(png));
	if (reader->offset + length > reader->size) {
		png_error(png, "PNG read past end of buffer");
	}
	std::memcpy(data, reader->data + reader->offset, length);
	reader->offset += length;
}

struct IvfFrame {
	const uint8_t* data = nullptr;
	std::size_t size = 0;
};

IvfFrame first_ivf_frame(const std::vector<std::byte>& bytes, const char* fourcc = "AV01", const char* codec = "AV1") {
	if (bytes.size() < 44) {
		throw std::runtime_error(std::string(codec) + " IVF byte buffer is too small");
	}
	if (std::memcmp(bytes.data(), "DKIF", 4) != 0) {
		throw std::runtime_error(std::string(codec) + " preview expects IVF data");
	}
	if (read_u16le(bytes.data() + 6) != 32) {
		throw std::runtime_error("unsupported IVF header size");
	}
	if (std::memcmp(bytes.data() + 8, fourcc, 4) != 0) {
		throw std::runtime_error("IVF stream is not " + std::string(codec));
	}
	const uint32_t frameSize = read_u32le(bytes.data() + 32);
	if (frameSize == 0 || 44ull + frameSize > bytes.size()) {
		throw std::runtime_error("invalid IVF frame size");
	}
	return {.data=reinterpret_cast<const uint8_t*>(bytes.data() + 44), .size=frameSize};
}

std::string dav1d_error(int err, const char* operation) {
	return std::string(operation) + " failed: " + std::strerror(-err);
}

PixelFormat hevc_pixel_format(const de265_image* picture) {
	const int bits = de265_get_bits_per_pixel(picture, 0);
	const de265_chroma chroma = de265_get_chroma_format(picture);
	if (chroma == de265_chroma_mono) {
		if (bits <= 8) return PixelFormat::Gray8;
		if (bits <= 10) return PixelFormat::Gray10LE;
		if (bits <= 12) return PixelFormat::Gray12LE;
		if (bits <= 14) return PixelFormat::Gray14LE;
	}
	if (chroma == de265_chroma_420) {
		if (bits <= 8) return PixelFormat::YUV420P8;
		if (bits <= 10) return PixelFormat::YUV420P10LE;
		if (bits <= 12) return PixelFormat::YUV420P12LE;
		if (bits <= 14) return PixelFormat::YUV420P14LE;
	}
	if (chroma == de265_chroma_422) {
		if (bits <= 8) return PixelFormat::YUV422P8;
		if (bits <= 10) return PixelFormat::YUV422P10LE;
		if (bits <= 12) return PixelFormat::YUV422P12LE;
		if (bits <= 14) return PixelFormat::YUV422P14LE;
	}
	if (chroma == de265_chroma_444) {
		if (bits <= 8) return PixelFormat::YUV444P8;
		if (bits <= 10) return PixelFormat::YUV444P10LE;
		if (bits <= 12) return PixelFormat::YUV444P12LE;
		if (bits <= 14) return PixelFormat::YUV444P14LE;
	}
	throw std::runtime_error(
		"unsupported HEVC preview format: " + std::to_string(bits) + "-bit chroma " +
		std::to_string(static_cast<int>(chroma))
	);
}

PixelAspectRatio hevc_pixel_aspect_ratio(const EncodedImage& encoded) {
    std::unique_ptr<GstH265Parser, decltype(&gst_h265_parser_free)> parser(gst_h265_parser_new(), gst_h265_parser_free);
    const auto* bytes = reinterpret_cast<const guint8*>(encoded.hevcAnnexB.data());
    guint offset = 0;
    while (offset < encoded.hevcAnnexB.size()) {
        GstH265NalUnit nal{};
        const auto status = gst_h265_parser_identify_nalu(parser.get(), bytes, offset, encoded.hevcAnnexB.size(), &nal);
        if (status != GST_H265_PARSER_OK && status != GST_H265_PARSER_NO_NAL_END) break;
        if (nal.type == GST_H265_NAL_SPS) {
            GstH265SPS sps{};
            if (gst_h265_parser_parse_sps(parser.get(), &nal, &sps, TRUE) == GST_H265_PARSER_OK &&
                sps.vui_parameters_present_flag && sps.vui_params.par_n && sps.vui_params.par_d)
                return PixelAspectRatio(sps.vui_params.par_n, sps.vui_params.par_d);
        }
        const auto next = nal.offset + nal.size;
        if (next <= offset) break;
        offset = next;
    }
    return {};
}

std::shared_ptr<const RawImage> copy_de265_picture(const de265_image* picture,const std::optional<ColorDescription>& context, PixelAspectRatio sar) {
	auto image = std::make_shared<RawImage>();
	image->pixelAspectRatio = sar;
	image->width = de265_get_image_width(picture, 0);
	image->height = de265_get_image_height(picture, 0);
	image->format = hevc_pixel_format(picture);
	image->color.primaries = static_cast<ColorPrimaries>(de265_get_image_colour_primaries(picture));
	image->color.transfer = static_cast<TransferCharacteristics>(de265_get_image_transfer_characteristics(picture));
	image->color.matrix = static_cast<MatrixCoefficients>(de265_get_image_matrix_coefficients(picture));
	image->color.range = de265_get_image_full_range_flag(picture) ? ColorRange::Full : ColorRange::Limited;
    if(context) {
        bool retained=false;
        if(image->color.primaries==ColorPrimaries::Unspecified){image->color.primaries=context->primaries;retained=true;}
        if(image->color.transfer==TransferCharacteristics::Unspecified){image->color.transfer=context->transfer;retained=true;}
        if(image->color.matrix==MatrixCoefficients::Unspecified){image->color.matrix=context->matrix;retained=true;}
        if(retained)image->importNote="libde265 omitted a color field; using retained encoded-image color context for that field.";
    }

	const int bytesPerSample = de265_get_bits_per_pixel(picture, 0) <= 8 ? 1 : 2;
	const int planeCount = de265_get_chroma_format(picture) == de265_chroma_mono ? 1 : 3;
	for (int plane = 0; plane < planeCount; ++plane) {
		int srcStride = 0;
		const uint8_t* src = de265_get_image_plane(picture, plane, &srcStride);
		if (src == nullptr) {
			continue;
		}
		const int planeWidth = de265_get_image_width(picture, plane);
		const int planeHeight = de265_get_image_height(picture, plane);
		const int dstStride = planeWidth * bytesPerSample;
		image->planes[plane].strideBytes = dstStride;
		image->planes[plane].bytes.resize(static_cast<std::size_t>(dstStride) * static_cast<std::size_t>(planeHeight));
		for (int y = 0; y < planeHeight; ++y) {
			std::memcpy(
				image->planes[plane].bytes.data() + static_cast<std::size_t>(y) * static_cast<std::size_t>(dstStride),
				src + static_cast<std::size_t>(y) * static_cast<std::size_t>(srcStride),
				static_cast<std::size_t>(dstStride)
			);
		}
	}
	return image;
}

PixelFormat av1_pixel_format(const Dav1dPicture& picture) {
	if (picture.p.bpc <= 8 && picture.p.layout == DAV1D_PIXEL_LAYOUT_I420) {
		return PixelFormat::YUV420P8;
	}
	if (picture.p.bpc <= 10 && picture.p.layout == DAV1D_PIXEL_LAYOUT_I420) {
		return PixelFormat::YUV420P10LE;
	}
	if (picture.p.bpc == 12 && picture.p.layout == DAV1D_PIXEL_LAYOUT_I420) return PixelFormat::YUV420P12LE;
	if (picture.p.bpc <= 8 && picture.p.layout == DAV1D_PIXEL_LAYOUT_I422) {
		return PixelFormat::YUV422P8;
	}
	if (picture.p.bpc <= 10 && picture.p.layout == DAV1D_PIXEL_LAYOUT_I422) {
		return PixelFormat::YUV422P10LE;
	}
	if (picture.p.bpc == 12 && picture.p.layout == DAV1D_PIXEL_LAYOUT_I422) return PixelFormat::YUV422P12LE;
	if (picture.p.bpc <= 8 && picture.p.layout == DAV1D_PIXEL_LAYOUT_I444) {
		return PixelFormat::YUV444P8;
	}
	if (picture.p.bpc <= 10 && picture.p.layout == DAV1D_PIXEL_LAYOUT_I444) {
		return PixelFormat::YUV444P10LE;
	}
	if (picture.p.bpc == 12 && picture.p.layout == DAV1D_PIXEL_LAYOUT_I444) return PixelFormat::YUV444P12LE;
	if (picture.p.bpc <= 8 && picture.p.layout == DAV1D_PIXEL_LAYOUT_I400) {
		return PixelFormat::Gray8;
	}
	if (picture.p.bpc <= 10 && picture.p.layout == DAV1D_PIXEL_LAYOUT_I400) {
		return PixelFormat::Gray10LE;
	}
	if (picture.p.bpc == 12 && picture.p.layout == DAV1D_PIXEL_LAYOUT_I400) return PixelFormat::Gray12LE;
	throw std::runtime_error("unsupported AV1 preview format");
}

int av1_plane_width(const Dav1dPicture& picture, int plane) {
	if (plane == 0 || picture.p.layout == DAV1D_PIXEL_LAYOUT_I444 || picture.p.layout == DAV1D_PIXEL_LAYOUT_I400) {
		return picture.p.w;
	}
	return (picture.p.w + 1) / 2;
}

int av1_plane_height(const Dav1dPicture& picture, int plane) {
	if (plane == 0 || picture.p.layout == DAV1D_PIXEL_LAYOUT_I444 || picture.p.layout == DAV1D_PIXEL_LAYOUT_I422 || picture.p.layout == DAV1D_PIXEL_LAYOUT_I400) {
		return picture.p.h;
	}
	return (picture.p.h + 1) / 2;
}

std::shared_ptr<const RawImage> copy_dav1d_picture(const Dav1dPicture& picture) {
	auto image = std::make_shared<RawImage>();
	image->width = picture.p.w;
	image->height = picture.p.h;
	image->format = av1_pixel_format(picture);
	if (picture.seq_hdr != nullptr) {
		image->color.primaries = static_cast<ColorPrimaries>(picture.seq_hdr->pri);
		image->color.transfer = static_cast<TransferCharacteristics>(picture.seq_hdr->trc);
		image->color.matrix = static_cast<MatrixCoefficients>(picture.seq_hdr->mtrx);
		image->color.range = picture.seq_hdr->color_range ? ColorRange::Full : ColorRange::Limited;
		if (picture.p.layout == DAV1D_PIXEL_LAYOUT_I420) {
			// AV1: 0 unknown, 1 vertical (left), 2 colocated (top-left).
			if (picture.seq_hdr->chr == 1) image->color.chroma420Location = Chroma420SampleLocation::LeftCenter;
			if (picture.seq_hdr->chr == 2) image->color.chroma420Location = Chroma420SampleLocation::TopLeft;
		}
	}
	const int bytesPerSample = picture.p.bpc <= 8 ? 1 : 2;
	const int planeCount = picture.p.layout == DAV1D_PIXEL_LAYOUT_I400 ? 1 : 3;
	for (int plane = 0; plane < planeCount; ++plane) {
		const auto* src = static_cast<const uint8_t*>(picture.data[plane]);
		if (src == nullptr) {
			continue;
		}
		const int width = av1_plane_width(picture, plane);
		const int height = av1_plane_height(picture, plane);
		const int dstStride = width * bytesPerSample;
		const ptrdiff_t srcStride = plane == 0 ? picture.stride[0] : picture.stride[1];
		if (srcStride < dstStride) {
			throw std::runtime_error("AV1 preview plane stride is smaller than row size");
		}
		image->planes[plane].strideBytes = dstStride;
		image->planes[plane].bytes.resize(static_cast<std::size_t>(dstStride) * static_cast<std::size_t>(height));
		for (int y = 0; y < height; ++y) {
			std::memcpy(
				image->planes[plane].bytes.data() + static_cast<std::size_t>(y) * static_cast<std::size_t>(dstStride),
				src + static_cast<std::size_t>(y) * static_cast<std::size_t>(srcStride),
				static_cast<std::size_t>(dstStride)
			);
		}
	}
	return image;
}

std::string vvdec_error(vvdecDecoder* decoder, int ret, const char* operation) {
	std::string out = std::string(operation) + " failed: " + vvdec_get_error_msg(ret);
	if (decoder != nullptr) {
		const char* last = vvdec_get_last_error(decoder);
		if (last != nullptr && *last != '\0') {
			out += ": ";
			out += last;
		}
		const char* additional = vvdec_get_last_additional_error(decoder);
		if (additional != nullptr && *additional != '\0') {
			out += ": ";
			out += additional;
		}
	}
	return out;
}

PixelFormat vvc_pixel_format(const vvdecFrame* frame) {
	if (frame->bitDepth <= 8 && frame->colorFormat == VVDEC_CF_YUV420_PLANAR) {
		return PixelFormat::YUV420P8;
	}
	if (frame->bitDepth <= 10 && frame->colorFormat == VVDEC_CF_YUV420_PLANAR) {
		return PixelFormat::YUV420P10LE;
	}
	if (frame->bitDepth <= 8 && frame->colorFormat == VVDEC_CF_YUV422_PLANAR) {
		return PixelFormat::YUV422P8;
	}
	if (frame->bitDepth <= 10 && frame->colorFormat == VVDEC_CF_YUV422_PLANAR) {
		return PixelFormat::YUV422P10LE;
	}
	if (frame->bitDepth <= 8 && frame->colorFormat == VVDEC_CF_YUV444_PLANAR) {
		return PixelFormat::YUV444P8;
	}
	if (frame->bitDepth <= 10 && frame->colorFormat == VVDEC_CF_YUV444_PLANAR) {
		return PixelFormat::YUV444P10LE;
	}
	if (frame->bitDepth <= 8 && frame->colorFormat == VVDEC_CF_YUV400_PLANAR) {
		return PixelFormat::Gray8;
	}
	if (frame->bitDepth <= 10 && frame->colorFormat == VVDEC_CF_YUV400_PLANAR) {
		return PixelFormat::Gray10LE;
	}
	throw std::runtime_error("unsupported VVC preview format");
}

std::shared_ptr<const RawImage> copy_vvdec_frame(const vvdecFrame* frame) {
	auto image = std::make_shared<RawImage>();
	image->width = static_cast<int>(frame->width);
	image->height = static_cast<int>(frame->height);
	image->format = vvc_pixel_format(frame);
	if (frame->picAttributes != nullptr && frame->picAttributes->vui != nullptr) {
		const vvdecVui& vui = *frame->picAttributes->vui;
		if (vui.aspectRatioInfoPresentFlag && vui.sarWidth > 0 && vui.sarHeight > 0)
            image->pixelAspectRatio = PixelAspectRatio(vui.sarWidth, vui.sarHeight);
		if (vui.colourDescriptionPresentFlag) {
			image->color.primaries = static_cast<ColorPrimaries>(vui.colourPrimaries);
			image->color.transfer = static_cast<TransferCharacteristics>(vui.transferCharacteristics);
			image->color.matrix = static_cast<MatrixCoefficients>(vui.matrixCoefficients);
		}
		image->color.range = vui.videoFullRangeFlag ? ColorRange::Full : ColorRange::Limited;
		if (vui.chromaLocInfoPresentFlag) image->color.chroma420Location = static_cast<Chroma420SampleLocation>(vui.chromaSampleLocType);
	}
	const uint32_t planeCount = std::min<uint32_t>(frame->numPlanes, VVDEC_MAX_NUM_COMPONENT);
	for (uint32_t plane = 0; plane < planeCount; ++plane) {
		const vvdecPlane& src = frame->planes[plane];
		if (src.ptr == nullptr || src.width == 0 || src.height == 0 || src.bytesPerSample == 0) {
			continue;
		}
		const std::size_t rowBytes = static_cast<std::size_t>(src.width) * static_cast<std::size_t>(src.bytesPerSample);
		if (rowBytes > src.stride) {
			throw std::runtime_error("VVC preview plane stride is smaller than row size");
		}
		image->planes[plane].strideBytes = static_cast<int>(rowBytes);
		image->planes[plane].bytes.resize(rowBytes * static_cast<std::size_t>(src.height));
		for (uint32_t y = 0; y < src.height; ++y) {
			std::memcpy(
				image->planes[plane].bytes.data() + static_cast<std::size_t>(y) * rowBytes,
				src.ptr + static_cast<std::size_t>(y) * static_cast<std::size_t>(src.stride),
				rowBytes
			);
		}
	}
	return image;
}

} // namespace

DecodeResult decode_embedded_preview(const EncodedImage& encoded) {
	if (encoded.previewImage) {
		return {.image=encoded.previewImage, .error={}};
	}
	return {.image=nullptr, .error="encoded preview image is unavailable"};
}

DecodeResult decode_jpegxs_preview(const EncodedImage& encoded) {
    try { double seconds; auto image=decode_jpegxs_image(encoded,1,&seconds); return {.image=std::move(image), .error={}, .codecSeconds=seconds}; }
    catch (const std::exception& error) { return {.image=nullptr, .error=error.what()}; }
}

DecodeResult decode_webp_preview(const EncodedImage& encoded) {
	try { double seconds; auto image=decode_webp_image(encoded,&seconds); return {.image=std::move(image), .error={}, .codecSeconds=seconds}; }
	catch (const std::exception& e) { return {.image=nullptr, .error=e.what()}; }
}
DecodeResult decode_vp8_preview(const EncodedImage& encoded) {
	try { double seconds; auto image=decode_vpx_image(encoded,false,&seconds); return {.image=std::move(image), .error={}, .codecSeconds=seconds}; }
	catch (const std::exception& e) { return {.image=nullptr, .error=e.what()}; }
}
DecodeResult decode_vp9_preview(const EncodedImage& encoded) {
	try { double seconds; auto image=decode_vpx_image(encoded,true,&seconds); return {.image=std::move(image), .error={}, .codecSeconds=seconds}; }
	catch (const std::exception& e) { return {.image=nullptr, .error=e.what()}; }
}

static DecodeResult decode_jpegls_image(const EncodedImage& encoded, bool source) {
	if (encoded.hevcAnnexB.empty()) {
		return {.image=nullptr, .error="JPEG-LS encoded byte buffer is empty"};
	}
	try {
		const std::vector<uint8_t> bytes = bytes_to_u8(encoded.hevcAnnexB);
		charls::jpegls_decoder const decoder{bytes, true};
		const charls::frame_info info = decoder.frame_info();
		if(source && decoder.spiff_header_has_value()) {
			const auto color=decoder.spiff_header().color_space;
			if(color!=charls::spiff_color_space::none && color!=charls::spiff_color_space::rgb && color!=charls::spiff_color_space::grayscale)
				throw std::runtime_error("JPEG-LS source requires RGB or grayscale SPIFF color interpretation");
		}
		if ((info.bits_per_sample < 2 || info.bits_per_sample > 16) || (info.component_count != 1 && info.component_count != 3)) {
			throw std::runtime_error("unsupported JPEG-LS preview format");
		}
		const int sampleBytes = info.bits_per_sample <= 8 ? 1 : 2;
		std::vector<uint8_t> rgb(static_cast<std::size_t>(info.width) * info.height * info.component_count * sampleBytes);
		CodecTimer timer;
		timer.measure([&] {decoder.decode(rgb);});
		if(info.component_count==3 && decoder.interleave_mode()!=charls::interleave_mode::sample) {
			const auto planar=rgb;
			for(uint32_t y=0;y<info.height;++y) for(uint32_t x=0;x<info.width;++x) for(int c=0;c<3;++c) {
				const std::size_t src=decoder.interleave_mode()==charls::interleave_mode::none
					? (std::size_t(c)*info.height+y)*info.width+x : (std::size_t(y)*3+c)*info.width+x;
				std::memcpy(rgb.data()+((std::size_t(y)*info.width+x)*3+c)*sampleBytes,planar.data()+src*sampleBytes,sampleBytes);
			}
		}
		return {.image=decoded_rgb_pixels(rgb,info.width,info.height,info.bits_per_sample,info.component_count,encoded,source),.error={},.codecSeconds=timer.seconds()};
	} catch (const std::exception& e) {
		return {.image=nullptr, .error=e.what()};
	}
}

DecodeResult decode_jpegls_preview(const EncodedImage& encoded) {return decode_jpegls_image(encoded,false);}

DecodeResult decode_jpeg_preview(const EncodedImage& encoded) {
	if (encoded.hevcAnnexB.empty()) {
		return {.image=nullptr, .error="JPEG encoded byte buffer is empty"};
	}
	jpeg_decompress_struct cinfo{};
	jpeg_error_mgr jerr{};
	cinfo.err = jpeg_std_error(&jerr);
	try {
		jpeg_create_decompress(&cinfo);
		jpeg_mem_src(
			&cinfo,
			reinterpret_cast<const unsigned char*>(encoded.hevcAnnexB.data()),
			static_cast<unsigned long>(encoded.hevcAnnexB.size())
		);
		jpeg_read_header(&cinfo, TRUE);
		const PixelAspectRatio sar = cinfo.X_density && cinfo.Y_density ? PixelAspectRatio(cinfo.Y_density, cinfo.X_density) : PixelAspectRatio{};
		const int bitDepth = cinfo.data_precision;
		if (bitDepth != 8 && bitDepth != 12) throw std::runtime_error("unsupported JPEG preview precision");
		cinfo.out_color_space = JCS_RGB;
		CodecTimer timer;
		timer.measure([&] {jpeg_start_decompress(&cinfo);});
		std::vector<uint8_t> rgb(
			static_cast<std::size_t>(cinfo.output_width) * cinfo.output_height * 3 * (bitDepth == 8 ? 1 : 2)
		);
		timer.measure([&] {while (cinfo.output_scanline < cinfo.output_height) {
			if (bitDepth == 8) {
				JSAMPROW row = rgb.data() + static_cast<std::size_t>(cinfo.output_scanline) * cinfo.output_width * 3;
				jpeg_read_scanlines(&cinfo, &row, 1);
			} else {
				auto row = reinterpret_cast<J12SAMPROW>(
					rgb.data() + static_cast<std::size_t>(cinfo.output_scanline) * cinfo.output_width * 3 * 2
				);
				jpeg12_read_scanlines(&cinfo, &row, 1);
			}
		}});
		const int width = static_cast<int>(cinfo.output_width);
		const int height = static_cast<int>(cinfo.output_height);
		timer.measure([&] {jpeg_finish_decompress(&cinfo);});
		jpeg_destroy_decompress(&cinfo);
		auto image = jxl_pixels_to_preview(
			rgb, width, height, bitDepth, 3, rgb_preview_color(encoded),
			static_cast<double>((1u << bitDepth) - 1u)
		);
        image->pixelAspectRatio = sar;
        return {.image=std::move(image), .error={}, .codecSeconds=timer.seconds()};
	} catch (const std::exception& e) {
		jpeg_destroy_decompress(&cinfo);
		return {.image=nullptr, .error=e.what()};
	}
}

DecodeResult decode_png_preview(const EncodedImage& encoded) {
	if (encoded.hevcAnnexB.empty()) {
		return {.image=nullptr, .error="PNG encoded byte buffer is empty"};
	}
	png_structp png = png_create_read_struct(PNG_LIBPNG_VER_STRING, nullptr, nullptr, nullptr);
	if (!png) return {.image=nullptr, .error="png_create_read_struct failed"};
	png_infop info = png_create_info_struct(png);
	if (!info) {
		png_destroy_read_struct(&png, nullptr, nullptr);
		return {.image=nullptr, .error="png_create_info_struct failed"};
	}
	try {
		if (setjmp(png_jmpbuf(png))) {
			throw std::runtime_error("libpng decode failed");
		}
		const std::vector<uint8_t> bytes = bytes_to_u8(encoded.hevcAnnexB);
		PngMemoryReader reader{.data=bytes.data(), .size=bytes.size(), .offset=0};
		png_set_read_fn(png, &reader, png_read_callback);
		CodecTimer timer;
		timer.measure([&] {png_read_info(png, info);});
		png_uint_32 densityX = 0, densityY = 0; int unit = 0;
		png_get_pHYs(png, info, &densityX, &densityY, &unit);
		PixelAspectRatio sar;
		if (densityX && densityY) {
            const auto divisor = std::gcd(densityX, densityY);
            if (densityX/divisor <= 65535 && densityY/divisor <= 65535) sar = PixelAspectRatio(densityY/divisor, densityX/divisor);
        }
		const int width = static_cast<int>(png_get_image_width(png, info));
		const int height = static_cast<int>(png_get_image_height(png, info));
		const int colorType = png_get_color_type(png, info);
		const int bitDepth = png_get_bit_depth(png, info);
		if (bitDepth != 8 && bitDepth != 16) throw std::runtime_error("unsupported PNG preview precision");
		if (bitDepth == 16) png_set_swap(png);
		if (colorType == PNG_COLOR_TYPE_PALETTE) png_set_palette_to_rgb(png);
		if (colorType == PNG_COLOR_TYPE_GRAY && bitDepth < 8) png_set_expand_gray_1_2_4_to_8(png);
		if (png_get_valid(png, info, PNG_INFO_tRNS)) png_set_tRNS_to_alpha(png);
		if (colorType == PNG_COLOR_TYPE_GRAY || colorType == PNG_COLOR_TYPE_GRAY_ALPHA) png_set_gray_to_rgb(png);
		if (colorType & PNG_COLOR_MASK_ALPHA) png_set_strip_alpha(png);
		png_read_update_info(png, info);
		const int sampleBytes = bitDepth == 8 ? 1 : 2;
		std::vector<uint8_t> rgb(static_cast<std::size_t>(width) * height * 3 * sampleBytes);
		std::vector<png_bytep> rows(static_cast<std::size_t>(height));
		for (int y = 0; y < height; ++y) {
			rows[y] = rgb.data() + static_cast<std::size_t>(y) * width * 3 * sampleBytes;
		}
		timer.measure([&] {png_read_image(png, rows.data());});
		png_destroy_read_struct(&png, &info, nullptr);
		auto image = jxl_pixels_to_preview(
			rgb, width, height, bitDepth, 3, rgb_preview_color(encoded),
			static_cast<double>((1u << bitDepth) - 1u)
		);
        image->pixelAspectRatio = sar;
        return {.image=std::move(image), .error={}, .codecSeconds=timer.seconds()};
	} catch (const std::exception& e) {
		png_destroy_read_struct(&png, &info, nullptr);
		return {.image=nullptr, .error=e.what()};
	}
}

static DecodeResult decode_jpeg2000_image(const EncodedImage& encoded, bool source) {
	if (encoded.hevcAnnexB.empty()) {
		return {.image=nullptr, .error="JPEG 2000 encoded byte buffer is empty"};
	}
	try {
		const bool raw=encoded.hevcAnnexB.size()>=2 && encoded.hevcAnnexB[0]==std::byte{0xff} && encoded.hevcAnnexB[1]==std::byte{0x4f};
		std::unique_ptr<opj_codec_t, decltype(&opj_destroy_codec)> const codec(opj_create_decompress(raw ? OPJ_CODEC_J2K : OPJ_CODEC_JP2), opj_destroy_codec);
		if (!codec) throw std::runtime_error("OpenJPEG decoder allocation failed");
		opj_dparameters_t params{};
		opj_set_default_decoder_parameters(&params);
		if (!opj_setup_decoder(codec.get(), &params)) throw std::runtime_error("OpenJPEG decoder setup failed");
		std::unique_ptr<opj_stream_t, decltype(&opj_stream_destroy)> const stream(opj_stream_default_create(OPJ_TRUE), opj_stream_destroy);
		if (!stream) throw std::runtime_error("OpenJPEG memory stream allocation failed");
		struct Reader {const std::vector<std::byte>& bytes; std::size_t pos=0;} reader{encoded.hevcAnnexB};
		opj_stream_set_user_data(stream.get(),&reader,nullptr);
		opj_stream_set_user_data_length(stream.get(),reader.bytes.size());
		opj_stream_set_read_function(stream.get(),[](void* dst,OPJ_SIZE_T n,void* user)->OPJ_SIZE_T {
			auto& r=*static_cast<Reader*>(user); n=std::min(n,r.bytes.size()-r.pos);
			if(n==0) return OPJ_SIZE_T(-1);
			std::memcpy(dst,r.bytes.data()+r.pos,n); r.pos+=n; return n;
		});
		opj_stream_set_skip_function(stream.get(),[](OPJ_OFF_T n,void* user)->OPJ_OFF_T {
			auto& r=*static_cast<Reader*>(user);
			if(n < -static_cast<OPJ_OFF_T>(r.pos) || (n>=0 && uint64_t(n)>r.bytes.size()-r.pos)) return -1;
			r.pos=static_cast<OPJ_OFF_T>(r.pos)+n; return n;
		});
		opj_stream_set_seek_function(stream.get(),[](OPJ_OFF_T at,void* user)->OPJ_BOOL {
			auto& r=*static_cast<Reader*>(user); if(at<0 || uint64_t(at)>r.bytes.size()) return OPJ_FALSE;
			r.pos=at; return OPJ_TRUE;
		});
		opj_image_t* rawImage = nullptr;
		if (!opj_read_header(stream.get(), codec.get(), &rawImage)) throw std::runtime_error("OpenJPEG read header failed");
		std::unique_ptr<opj_image_t, decltype(&opj_image_destroy)> image(rawImage, opj_image_destroy);
		CodecTimer timer;
		if (!timer.measure([&] {return opj_decode(codec.get(), stream.get(), image.get()) && opj_end_decompress(codec.get(), stream.get());})) {
			throw std::runtime_error("OpenJPEG decode failed");
		}
		const int channels = static_cast<int>(image->numcomps);
		if (channels != 1 && channels != 3) throw std::runtime_error("unsupported JPEG 2000 component count");
		if(source && image->color_space!=OPJ_CLRSPC_SRGB && image->color_space!=OPJ_CLRSPC_GRAY &&
		   image->color_space!=OPJ_CLRSPC_UNKNOWN && image->color_space!=OPJ_CLRSPC_UNSPECIFIED)
			throw std::runtime_error("JPEG 2000 source requires RGB or grayscale components; YCbCr/CMYK import is not implemented");
		const int width = static_cast<int>(image->comps[0].w);
		const int height = static_cast<int>(image->comps[0].h);
		const int bitDepth = static_cast<int>(image->comps[0].prec);
		if (bitDepth < 2 || bitDepth > 16) throw std::runtime_error("unsupported JPEG 2000 preview precision");
		for (int component = 0; component < channels; ++component) {
			if (image->comps[component].w != image->comps[0].w ||
			    image->comps[component].h != image->comps[0].h ||
			    image->comps[component].prec != image->comps[0].prec ||
			    image->comps[component].sgnd != 0 || image->comps[component].data==nullptr) {
				throw std::runtime_error("unsupported JPEG 2000 component layout");
			}
		}
		const int sampleBytes = bitDepth <= 8 ? 1 : 2;
		const uint32_t maximum = (1u << bitDepth) - 1u;
		std::vector<uint8_t> rgb(static_cast<std::size_t>(width) * height * channels * sampleBytes);
		auto store = [&](std::size_t sample, int value) {
			const uint16_t clamped = static_cast<uint16_t>(std::clamp(value, 0, static_cast<int>(maximum)));
			if (sampleBytes == 1) {
				rgb[sample] = static_cast<uint8_t>(clamped);
			} else {
				rgb[sample * 2] = static_cast<uint8_t>(clamped & 0xffu);
				rgb[sample * 2 + 1] = static_cast<uint8_t>(clamped >> 8u);
			}
		};
		for (int y = 0; y < height; ++y) {
			for (int x = 0; x < width; ++x) {
				const std::size_t i = static_cast<std::size_t>(y) * width + x;
				for (int c = 0; c < channels; ++c) store(i * channels + c, image->comps[c].data[i]);
			}
		}
		DecodeResult result{.image=decoded_rgb_pixels(rgb,width,height,bitDepth,channels,encoded,source),.error={},.codecSeconds=timer.seconds()};
		if(source && image->icc_profile_buf && image->icc_profile_len)
			result.iccProfile.assign(image->icc_profile_buf,image->icc_profile_buf+image->icc_profile_len);
		return result;
	} catch (const std::exception& e) {
		return {.image=nullptr, .error=e.what()};
	}
}

DecodeResult decode_jpeg2000_preview(const EncodedImage& encoded) {return decode_jpeg2000_image(encoded,false);}

DecodeResult decode_jpegxl_preview(const EncodedImage& encoded) {
	if (encoded.hevcAnnexB.empty()) {
		return {.image=nullptr, .error="JPEG XL encoded byte buffer is empty"};
	}
	try {
		std::unique_ptr<JxlDecoder, decltype(&JxlDecoderDestroy)> const dec(JxlDecoderCreate(nullptr), JxlDecoderDestroy);
		if (!dec) throw std::runtime_error("JxlDecoderCreate failed");
		if (JxlDecoderSubscribeEvents(dec.get(), JXL_DEC_BASIC_INFO | JXL_DEC_COLOR_ENCODING | JXL_DEC_FULL_IMAGE) != JXL_DEC_SUCCESS) {
			throw std::runtime_error("JxlDecoderSubscribeEvents failed");
		}
		if (JxlDecoderSetInput(dec.get(), reinterpret_cast<const uint8_t*>(encoded.hevcAnnexB.data()), encoded.hevcAnnexB.size()) != JXL_DEC_SUCCESS) {
			throw std::runtime_error("JxlDecoderSetInput failed");
		}
		JxlDecoderCloseInput(dec.get());
		JxlBasicInfo info{};
		JxlColorEncoding originalProfile{};
		ColorDescription decodedColor;
		bool haveColor = false;
		std::vector<uint8_t> pixels;
		JxlPixelFormat format{.num_channels=3, .data_type=JXL_TYPE_UINT8, .endianness=JXL_LITTLE_ENDIAN, .align=0};
		CodecTimer timer;
		for (;;) {
			const JxlDecoderStatus status = timer.measure([&] {return JxlDecoderProcessInput(dec.get());});
			if (status == JXL_DEC_BASIC_INFO) {
				if (JxlDecoderGetBasicInfo(dec.get(), &info) != JXL_DEC_SUCCESS) {
					throw std::runtime_error("JxlDecoderGetBasicInfo failed");
				}
				if (info.exponent_bits_per_sample != 0 ||
				    (info.bits_per_sample != 8 && info.bits_per_sample != 10 && info.bits_per_sample != 12 &&
				     info.bits_per_sample != 14 && info.bits_per_sample != 16)) {
					throw std::runtime_error("JPEG XL preview supports integer 8-, 10-, 12-, 14-, and 16-bit codestreams; got " + std::to_string(info.bits_per_sample) + "-bit with " + std::to_string(info.exponent_bits_per_sample) + " exponent bits");
				}
				if (info.num_color_channels != 1 && info.num_color_channels != 3) {
					throw std::runtime_error("unsupported JPEG XL color channel count: " + std::to_string(info.num_color_channels));
				}
				format.num_channels = info.num_color_channels;
				format.data_type = info.bits_per_sample == 8 ? JXL_TYPE_UINT8 : JXL_TYPE_UINT16;
			} else if (status == JXL_DEC_COLOR_ENCODING) {
				if (JxlDecoderGetColorAsEncodedProfile(dec.get(), JXL_COLOR_PROFILE_TARGET_ORIGINAL, &originalProfile) != JXL_DEC_SUCCESS) {
					throw std::runtime_error("JPEG XL codestream has no usable structured color profile");
				}
				decodedColor = color_description_from_jxl(originalProfile);
				// Matrix/range describe our RGB-to-YUV reconstruction, not the
				// codestream's primaries/transfer. Never override those observations.
				if (encoded.codedColor) {
					decodedColor.matrix = encoded.codedColor->matrix;
					decodedColor.range = encoded.codedColor->range;
				}
				haveColor = true;
				if (info.uses_original_profile == JXL_FALSE &&
				    JxlDecoderSetPreferredColorProfile(dec.get(), &originalProfile) != JXL_DEC_SUCCESS) {
					throw std::runtime_error("JPEG XL decoder could not convert its reconstruction to the codestream's original color profile");
				}
			} else if (status == JXL_DEC_NEED_IMAGE_OUT_BUFFER) {
				if (info.xsize == 0 || info.ysize == 0) throw std::runtime_error("JPEG XL decoder requested an output buffer before reporting dimensions");
				size_t outSize = 0;
				if (JxlDecoderImageOutBufferSize(dec.get(), &format, &outSize) != JXL_DEC_SUCCESS) {
					throw std::runtime_error("JxlDecoderImageOutBufferSize failed");
				}
				pixels.resize(outSize);
				if (JxlDecoderSetImageOutBuffer(dec.get(), &format, pixels.data(), pixels.size()) != JXL_DEC_SUCCESS) {
					throw std::runtime_error("JxlDecoderSetImageOutBuffer failed");
				}
			} else if (status == JXL_DEC_FULL_IMAGE || status == JXL_DEC_SUCCESS) {
				if (pixels.empty() || info.xsize == 0 || info.ysize == 0) {
					throw std::runtime_error("JPEG XL decoder produced no image");
				}
				if (!haveColor) throw std::runtime_error("JPEG XL decoder produced pixels without color metadata");
				return {.image=jxl_pixels_to_preview(
					pixels, static_cast<int>(info.xsize), static_cast<int>(info.ysize),
					static_cast<int>(info.bits_per_sample), info.num_color_channels, decodedColor,
					info.bits_per_sample == 8 ? 255.0 : 65535.0
				), .error={}, .codecSeconds=timer.seconds()};
			} else if (status == JXL_DEC_NEED_MORE_INPUT) {
				throw std::runtime_error("JPEG XL decoder requested more input");
			} else if (status == JXL_DEC_ERROR) {
				throw std::runtime_error("JPEG XL decode failed");
			}
		}
	} catch (const std::exception& e) {
		return {.image=nullptr, .error=e.what()};
	}
}

static DecodeResult decode_jpegxr_image(const EncodedImage& encoded, bool source) {
	if (encoded.hevcAnnexB.empty()) {
		return {.image=nullptr, .error="JPEG XR encoded byte buffer is empty"};
	}
	try {
		WMPStream* stream = nullptr;
		// jxrlib's stream API is mutable, so give it an owned memory buffer.
		auto bytes=bytes_to_u8(encoded.hevcAnnexB);
		if (CreateWS_Memory(&stream, bytes.data(), bytes.size()) != 0) throw std::runtime_error("jxrlib CreateWS_Memory failed");
		std::unique_ptr<WMPStream, void (*)(WMPStream*)> streamGuard(stream, [](WMPStream* s) {
			WMPStream* tmp = s;
			CloseWS_Memory(&tmp);
		});
		PKImageDecode* dec = nullptr;
		if (PKImageDecode_Create_WMP(&dec) != 0) throw std::runtime_error("jxrlib decoder allocation failed");
		std::unique_ptr<PKImageDecode, void (*)(PKImageDecode*)> decGuard(dec, [](PKImageDecode* d) {
			PKImageDecode* tmp = d;
			d->Release(&tmp);
		});
		const auto initError=dec->Initialize(dec, stream);
		if (initError != 0) throw std::runtime_error("jxrlib decoder initialize failed: " + std::to_string(initError));
		dec->fStreamOwner=FALSE; // streamGuard owns the memory stream on every path.
		I32 width = 0;
		I32 height = 0;
		if (dec->GetSize(dec, &width, &height) != 0 || width <= 0 || height <= 0) throw std::runtime_error("jxrlib GetSize failed");
		PKPixelFormatGUID pixelFormat{};
		if (dec->GetPixelFormat(dec, &pixelFormat) != 0) throw std::runtime_error("jxrlib GetPixelFormat failed");
		const int channels = IsEqualGUID(pixelFormat, GUID_PKPixelFormat8bppGray) ? 1 : 3;
		const int bitDepth = (channels == 1 || IsEqualGUID(pixelFormat, GUID_PKPixelFormat24bppRGB)) ? 8 :
			(IsEqualGUID(pixelFormat, GUID_PKPixelFormat48bppRGB) ? 16 : 0);
		if (bitDepth == 0) throw std::runtime_error("unsupported JPEG XR preview pixel format");
		const int sampleBytes = bitDepth == 8 ? 1 : 2;
		std::vector<uint8_t> rgb(static_cast<std::size_t>(width) * height * channels * sampleBytes);
		PKRect const rect{.X=0, .Y=0, .Width=width, .Height=height};
		CodecTimer timer;
		if (timer.measure([&] {return dec->Copy(dec, &rect, rgb.data(), static_cast<U32>(width * channels * sampleBytes));}) != 0) throw std::runtime_error("jxrlib Copy failed");
		DecodeResult result{.image=decoded_rgb_pixels(rgb,width,height,bitDepth,channels,encoded,source),.error={},.codecSeconds=timer.seconds()};
		if(source) {
			U32 size=0;
			if(dec->GetColorContext(dec,nullptr,&size)!=0) throw std::runtime_error("JPEG XR color profile read failed");
			if(size) {
				if(size>bytes.size()) throw std::runtime_error("JPEG XR color profile exceeds input size");
				result.iccProfile.resize(size);
				if(dec->GetColorContext(dec,result.iccProfile.data(),&size)!=0) throw std::runtime_error("JPEG XR color profile read failed");
			}
		}
		return result;
	} catch (const std::exception& e) {
		return {.image=nullptr, .error=e.what()};
	}
}

DecodeResult decode_jpegxr_preview(const EncodedImage& encoded) {return decode_jpegxr_image(encoded,false);}

DecodeResult decode_rgb_source(const EncodedImage& encoded, RgbSourceCodec codec) {
	switch(codec) {
		case RgbSourceCodec::JpegLs: return decode_jpegls_image(encoded,true);
		case RgbSourceCodec::Jpeg2000: return decode_jpeg2000_image(encoded,true);
		case RgbSourceCodec::JpegXr: return decode_jpegxr_image(encoded,true);
	}
	return {.image=nullptr,.error="Unknown RGB source decoder"};
}

static DecodeResult decode_ffmpeg_preview(const EncodedImage& encoded, AVCodecID codecId, const char* hwDevice = nullptr, bool qsv = false, const char* decoderName = nullptr, AVHWDeviceType hwType = AV_HWDEVICE_TYPE_VAAPI,bool reuseDevice=false) {
	if (encoded.hevcAnnexB.empty()) {
		return {.image=nullptr, .error="video encoded byte buffer is empty"};
	}
	auto ffmpeg_error = [](int error) {
		char text[AV_ERROR_MAX_STRING_SIZE]{};
		av_strerror(error, text, sizeof(text));
		return std::string{text};
	};
	auto format_for = [](AVPixelFormat format) {
        const auto* d=av_pix_fmt_desc_get(format);
        if(!d || d->flags & (AV_PIX_FMT_FLAG_BE|AV_PIX_FMT_FLAG_BITSTREAM|AV_PIX_FMT_FLAG_FLOAT)) throw std::runtime_error("Unsupported decoder pixel storage");
        const int bits=d->comp[0].depth;
        if(d->flags & AV_PIX_FMT_FLAG_RGB) {
            if(bits==8) return PixelFormat::RGBP8;
            if(bits==16) return PixelFormat::RGBP16LE;
            throw std::runtime_error("Unsupported RGB precision; refusing to quantize");
        }
        if(d->nb_components==1) {
            switch(bits) {case 8:return PixelFormat::Gray8;case 10:return PixelFormat::Gray10LE;case 12:return PixelFormat::Gray12LE;case 14:return PixelFormat::Gray14LE;}
        }
        if(d->log2_chroma_w==1 && d->log2_chroma_h==1) {
            switch(bits) {case 8:return PixelFormat::YUV420P8;case 10:return PixelFormat::YUV420P10LE;case 12:return PixelFormat::YUV420P12LE;case 14:return PixelFormat::YUV420P14LE;}
        }
        if(d->log2_chroma_w==1 && d->log2_chroma_h==0) {
            switch(bits) {case 8:return PixelFormat::YUV422P8;case 10:return PixelFormat::YUV422P10LE;case 12:return PixelFormat::YUV422P12LE;case 14:return PixelFormat::YUV422P14LE;}
        }
        if(d->log2_chroma_w==0 && d->log2_chroma_h==0) {
            switch(bits) {case 8:return PixelFormat::YUV444P8;case 10:return PixelFormat::YUV444P10LE;case 12:return PixelFormat::YUV444P12LE;case 14:return PixelFormat::YUV444P14LE;case 16:return PixelFormat::YUV444P16LE;}
        }
        throw std::runtime_error("Unsupported decoder pixel format; refusing to change chroma or precision");
    };
	const AVCodec* codec = decoderName ? avcodec_find_decoder_by_name(decoderName) : (qsv ? avcodec_find_decoder_by_name("hevc_qsv") : avcodec_find_decoder(codecId));
	if (!codec) return {.image=nullptr, .error="libavcodec video decoder is unavailable"};
	using CodecContextPtr = std::unique_ptr<AVCodecContext, void (*)(AVCodecContext*)>;
	using PacketPtr = std::unique_ptr<AVPacket, void (*)(AVPacket*)>;
	using FramePtr = std::unique_ptr<AVFrame, void (*)(AVFrame*)>;
	CodecContextPtr const decoder(avcodec_alloc_context3(codec), [](AVCodecContext* p) { avcodec_free_context(&p); });
	PacketPtr packet(av_packet_alloc(), [](AVPacket* p) { av_packet_free(&p); });
	FramePtr frame(av_frame_alloc(), [](AVFrame* p) { av_frame_free(&p); });
	if (!decoder || !packet || !frame) return {.image=nullptr, .error="libavcodec video decoder allocation failed"};
	try {
		if (hwDevice) {
			int error=0;
            if(reuseDevice && !qsv) {
                struct DeviceCache {AVBufferRef* device=nullptr;AVHWDeviceType type=AV_HWDEVICE_TYPE_NONE;std::string path;
                    ~DeviceCache(){av_buffer_unref(&device);}};
                thread_local DeviceCache cache;
                if(!cache.device || cache.type!=hwType || cache.path!=hwDevice) {
                    av_buffer_unref(&cache.device);
                    error=av_hwdevice_ctx_create(&cache.device,hwType,*hwDevice?hwDevice:nullptr,nullptr,0);
                    if(error>=0){cache.type=hwType;cache.path=hwDevice;}
                }
                if(cache.device)decoder->hw_device_ctx=av_buffer_ref(cache.device);
                if(error>=0 && !decoder->hw_device_ctx)error=AVERROR(ENOMEM);
            }else error=av_hwdevice_ctx_create(&decoder->hw_device_ctx, hwType, *hwDevice ? hwDevice : nullptr, nullptr, 0);
			if (error < 0) throw std::runtime_error("Hardware decoder device could not be created: " + ffmpeg_error(error));
			if (qsv) {
				AVBufferRef* derived = nullptr;
				error = av_hwdevice_ctx_create_derived(&derived, AV_HWDEVICE_TYPE_QSV, decoder->hw_device_ctx, 0);
				if (error < 0) throw std::runtime_error("HEVC QSV decoder device failed: " + ffmpeg_error(error));
				av_buffer_unref(&decoder->hw_device_ctx);
				decoder->hw_device_ctx = derived;
			}
			const auto target=qsv ? AV_PIX_FMT_QSV : hwType==AV_HWDEVICE_TYPE_VULKAN ? AV_PIX_FMT_VULKAN : AV_PIX_FMT_VAAPI;
            decoder->opaque=reinterpret_cast<void*>(static_cast<intptr_t>(target));
            decoder->get_format = [](AVCodecContext* ctx, const AVPixelFormat* formats) {
                const auto wanted=static_cast<AVPixelFormat>(reinterpret_cast<intptr_t>(ctx->opaque));
                for (; *formats != AV_PIX_FMT_NONE; ++formats) if (*formats == wanted) return *formats;
				return AV_PIX_FMT_NONE;
			};
			decoder->thread_count = 1;
			decoder->err_recognition = AV_EF_EXPLODE | AV_EF_BITSTREAM | AV_EF_BUFFER;
		}
        if(!hwDevice)decoder->get_format=[](AVCodecContext*,const AVPixelFormat* formats){
            for(;*formats!=AV_PIX_FMT_NONE;++formats){const auto* d=av_pix_fmt_desc_get(*formats);if(d && !(d->flags&AV_PIX_FMT_FLAG_HWACCEL))return *formats;}
            return AV_PIX_FMT_NONE;
        };
		int error = avcodec_open2(decoder.get(), codec, nullptr);
		if (error < 0) {
			throw std::runtime_error("libavcodec video decoder open failed: " + ffmpeg_error(error));
		}
		if (encoded.hevcAnnexB.size() > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
			throw std::runtime_error("video byte buffer is too large for libavcodec");
		}
		error = av_new_packet(packet.get(), static_cast<int>(encoded.hevcAnnexB.size()));
		if (error < 0) throw std::runtime_error("libavcodec packet allocation failed: " + ffmpeg_error(error));
		std::memcpy(packet->data, encoded.hevcAnnexB.data(), encoded.hevcAnnexB.size());
        if(hwDevice && codec->id==AV_CODEC_ID_VVC) {
            // Raw Annex-B packets bypass libavformat's parser. VVC's VA-API
            // setup needs the real SPS profile before hardware format selection.
            using ParserPtr=std::unique_ptr<AVCodecParserContext,void(*)(AVCodecParserContext*)>;
            ParserPtr parser(av_parser_init(AV_CODEC_ID_VVC),av_parser_close);
            if(!parser)throw std::runtime_error("VVC bitstream parser is unavailable");
            parser->flags|=PARSER_FLAG_COMPLETE_FRAMES;
            uint8_t* parsed=nullptr;int parsedSize=0;
            const int consumed=av_parser_parse2(parser.get(),decoder.get(),&parsed,&parsedSize,packet->data,packet->size,AV_NOPTS_VALUE,AV_NOPTS_VALUE,0);
            if(consumed<0 || decoder->profile==AV_PROFILE_UNKNOWN)throw std::runtime_error("Cannot determine VVC stream profile for decoder setup");
            // VVenC signals Main 10 Still Picture (65). VA-API exposes the
            // same coding tools through VVC Main10, while libavcodec's profile
            // table only accepts profile 1. Select that hardware configuration;
            // the original SPS, bit depth, chroma and limits remain unchanged.
            if(hwType==AV_HWDEVICE_TYPE_VAAPI && decoder->profile==65)
                decoder->profile=AV_PROFILE_VVC_MAIN_10;

        }
		const auto codecStart=std::chrono::steady_clock::now();
		error = avcodec_send_packet(decoder.get(), packet.get());
		if (error < 0) throw std::runtime_error("libavcodec rejected video stream: " + ffmpeg_error(error));
		error = avcodec_receive_frame(decoder.get(), frame.get());
		if (error == AVERROR(EAGAIN)) {
			error = avcodec_send_packet(decoder.get(), nullptr);
			if (error < 0) throw std::runtime_error("libavcodec video flush failed: " + ffmpeg_error(error));
			error = avcodec_receive_frame(decoder.get(), frame.get());
		}
		if (error < 0) throw std::runtime_error("libavcodec produced no video preview frame: " + ffmpeg_error(error));

		if (frame->flags & AV_FRAME_FLAG_CORRUPT || frame->decode_error_flags) throw std::runtime_error("video decoder reported a damaged frame");
        // receive_frame may return before GPU completion. Fence the actual
        // surface/timeline before reporting codec time, then measure readback
        // separately as part of the outer CPU-image interval.
        if(frame->format==AV_PIX_FMT_VAAPI) {
            auto* frames=reinterpret_cast<AVHWFramesContext*>(frame->hw_frames_ctx->data);
            auto* device=reinterpret_cast<AVVAAPIDeviceContext*>(frames->device_ctx->hwctx);
            if(vaSyncSurface(device->display,static_cast<VASurfaceID>(reinterpret_cast<uintptr_t>(frame->data[3])))!=VA_STATUS_SUCCESS)throw std::runtime_error("VA-API decode completion wait failed");
        }
        if(frame->format==AV_PIX_FMT_VULKAN) {
            auto* frames=reinterpret_cast<AVHWFramesContext*>(frame->hw_frames_ctx->data);
            auto* device=reinterpret_cast<AVVulkanDeviceContext*>(frames->device_ctx->hwctx);
            auto* frameContext=reinterpret_cast<AVVulkanFramesContext*>(frames->hwctx);
            auto* vkframe=reinterpret_cast<AVVkFrame*>(frame->data[0]);
            std::vector<VkSemaphore> semaphores;std::vector<uint64_t> values;
            if(frameContext->lock_frame)frameContext->lock_frame(frames,vkframe);
            for(int i=0;i<AV_NUM_DATA_POINTERS;++i)if(vkframe->sem[i]) {
                auto existing=std::find(semaphores.begin(),semaphores.end(),vkframe->sem[i]);
                if(existing==semaphores.end()){semaphores.push_back(vkframe->sem[i]);values.push_back(vkframe->sem_value[i]);}
                else values[std::size_t(existing-semaphores.begin())]=std::max(values[std::size_t(existing-semaphores.begin())],vkframe->sem_value[i]);
            }
            if(frameContext->unlock_frame)frameContext->unlock_frame(frames,vkframe);
            VkSemaphoreWaitInfo wait{};wait.sType=VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO;wait.semaphoreCount=semaphores.size();wait.pSemaphores=semaphores.data();wait.pValues=values.data();
            if(semaphores.empty() || vkWaitSemaphores(device->act_dev,&wait,UINT64_MAX)!=VK_SUCCESS)throw std::runtime_error("Vulkan Video decode completion wait failed");
        }
        const double codecSeconds=std::chrono::duration<double>(std::chrono::steady_clock::now()-codecStart).count();
		if (hwDevice) {
			if (frame->format != AV_PIX_FMT_VAAPI && frame->format != AV_PIX_FMT_QSV && frame->format != AV_PIX_FMT_VULKAN) throw std::runtime_error("Hardware decoder did not use hardware decoding");
			FramePtr downloaded(av_frame_alloc(), [](AVFrame* p) { av_frame_free(&p); });
			if (!downloaded) throw std::runtime_error("Hardware decoder frame allocation failed");
			error = av_hwframe_transfer_data(downloaded.get(), frame.get(), 0);
			if (error < 0) throw std::runtime_error("Hardware decoder surface download failed: " + ffmpeg_error(error));
			error = av_frame_copy_props(downloaded.get(), frame.get());
			if (error < 0) throw std::runtime_error("Hardware decoder metadata copy failed: " + ffmpeg_error(error));
			frame.swap(downloaded);
		}
		auto image = std::make_shared<RawImage>();
		image->width = frame->width;
        if (frame->sample_aspect_ratio.num > 0 && frame->sample_aspect_ratio.den > 0)
            image->pixelAspectRatio = PixelAspectRatio(frame->sample_aspect_ratio.num, frame->sample_aspect_ratio.den);
		image->height = frame->height;
		image->format = format_for(static_cast<AVPixelFormat>(frame->format));
		image->color.primaries = static_cast<ColorPrimaries>(frame->color_primaries);
		image->color.transfer = static_cast<TransferCharacteristics>(frame->color_trc);
		image->color.matrix = static_cast<MatrixCoefficients>(frame->colorspace);
		image->color.range = frame->color_range == AVCOL_RANGE_JPEG ? ColorRange::Full : ColorRange::Limited;
		if (frame->chroma_location == AVCHROMA_LOC_LEFT) image->color.chroma420Location = Chroma420SampleLocation::LeftCenter;
		if (frame->chroma_location == AVCHROMA_LOC_CENTER) image->color.chroma420Location = Chroma420SampleLocation::Center;
		if (frame->chroma_location == AVCHROMA_LOC_TOPLEFT) image->color.chroma420Location = Chroma420SampleLocation::TopLeft;
		const bool chroma420 = is_420(image->format);
		const bool chroma422 = is_422(image->format);
        const int bytesPerSample = bytes_per_sample(image->format);
        if(is_rgb(image->format)) {image->color.matrix=MatrixCoefficients::Identity;image->color.range=ColorRange::Full;}
        if(encoded.codedColor) {
            if(image->color.primaries==ColorPrimaries::Unspecified) image->color.primaries=encoded.codedColor->primaries;
            if(image->color.transfer==TransferCharacteristics::Unspecified) image->color.transfer=encoded.codedColor->transfer;
            if(image->color.matrix==MatrixCoefficients::Unspecified) image->color.matrix=encoded.codedColor->matrix;
        }
        for (int plane = 0; plane < plane_count(image->format); ++plane) {
			const int width = plane == 0 || (!chroma420 && !chroma422) ? image->width : (image->width + 1) / 2;
			const int height = plane == 0 || !chroma420 ? image->height : (image->height + 1) / 2;
			const int rowBytes = width * bytesPerSample;
			image->planes[plane].strideBytes = rowBytes;
			image->planes[plane].bytes.resize(static_cast<std::size_t>(rowBytes) * height);
			const auto* descriptor = av_pix_fmt_desc_get(static_cast<AVPixelFormat>(frame->format));
			if (!descriptor || descriptor->nb_components < plane_count(image->format) ||
			    (descriptor->flags & (AV_PIX_FMT_FLAG_BE | AV_PIX_FMT_FLAG_BITSTREAM))) {
				throw std::runtime_error("decoder returned an unsupported pixel storage description");
			}
			const auto& component = descriptor->comp[plane];
			if (!frame->data[component.plane]) throw std::runtime_error("decoder returned an incomplete frame");
			const int storageBytes = (component.shift + component.depth + 7) / 8;
			if (storageBytes > 4) throw std::runtime_error("decoder returned an oversized pixel component");
            // Common planar and NV12/P010 layouts need row copies or fixed
            // deinterleaving, not a variable-width word reader per sample.
            if(component.step==bytesPerSample && component.shift==0){
                for(int y=0;y<height;++y)std::memcpy(image->planes[plane].bytes.data()+std::size_t(y)*rowBytes,
                    frame->data[component.plane]+std::ptrdiff_t(y)*frame->linesize[component.plane]+component.offset,rowBytes);
                continue;
            }
            if(bytesPerSample==1 && component.step==2 && component.shift==0){
                for(int y=0;y<height;++y){auto* dst=image->planes[plane].bytes.data()+std::size_t(y)*rowBytes;
                    auto* src=frame->data[component.plane]+std::ptrdiff_t(y)*frame->linesize[component.plane]+component.offset;
                    for(int x=0;x<width;++x)dst[x]=src[x*2];}
                continue;
            }
            if(bytesPerSample==2 && storageBytes==2 && component.shift==6 && (component.step==2 || component.step==4)){
                for(int y=0;y<height;++y){auto* dst=image->planes[plane].bytes.data()+std::size_t(y)*rowBytes;
                    auto* src=frame->data[component.plane]+std::ptrdiff_t(y)*frame->linesize[component.plane]+component.offset;
                    if(component.step==2)for(int x=0;x<width;++x){uint16_t v;std::memcpy(&v,src+x*2,2);v>>=6;std::memcpy(dst+x*2,&v,2);}
                    else for(int x=0;x<width;++x){uint16_t v;std::memcpy(&v,src+x*4,2);v>>=6;std::memcpy(dst+x*2,&v,2);}}
                continue;
            }
			for (int y=0;y<height;++y) for(int x=0;x<width;++x) {
				const auto* value = frame->data[component.plane] + static_cast<std::ptrdiff_t>(y)*frame->linesize[component.plane]
				                  + static_cast<std::ptrdiff_t>(x)*component.step + component.offset;
				uint32_t word = 0;
				for (int byte=0;byte<storageBytes;++byte) word |= uint32_t(value[byte]) << (byte*8);
				const uint16_t sample = (word >> component.shift) & ((1u << component.depth)-1);
				std::memcpy(image->planes[plane].bytes.data()+static_cast<std::size_t>(y)*rowBytes+x*bytesPerSample,
				            &sample,bytesPerSample);
			}
		}
		return {.image=std::move(image), .error={}, .codecSeconds=codecSeconds};
	} catch (const std::exception& e) {
		return {.image=nullptr, .error=e.what()};
	}
}

DecodeResult decode_libavcodec(const EncodedImage& encoded, const std::string& name, const std::string& hardware, const std::string& device,bool reuseDevice) {
    if(name=="hevc" && hevc_is_scc(encoded)) {
        if(hardware=="vulkan") return decode_scc_vulkan(encoded,device);
        return {.image=nullptr,.error="HEVC SCC must use the native SCC decoder; libavcodec SCC decoding is disabled"};
    }
    EncodedImage packet; packet.codedColor=encoded.codedColor;
    const auto& bytes=encoded.hevcAnnexB;
    if(bytes.size()>=4 && std::memcmp(bytes.data(),"DKIF",4)==0) {
        if(bytes.size()<44) return {.image=nullptr,.error="Truncated IVF frame"};
        const auto* b=reinterpret_cast<const uint8_t*>(bytes.data());
        const std::size_t header=std::size_t(b[6])|(std::size_t(b[7])<<8);
        if(header<32 || header>bytes.size()-12) return {.image=nullptr,.error="Invalid IVF header"};
        uint32_t count=0;for(int i=0;i<4;++i) count|=uint32_t(b[header+i])<<(8*i);
        if(count>bytes.size()-header-12) return {.image=nullptr,.error="Truncated IVF payload"};
        packet.hevcAnnexB.assign(bytes.begin()+header+12,bytes.begin()+header+12+count);
    } else packet.hevcAnnexB=bytes;
    return decode_ffmpeg_preview(packet,AV_CODEC_ID_NONE,hardware.empty()?nullptr:device.c_str(),false,name.c_str(),hardware=="vulkan"?AV_HWDEVICE_TYPE_VULKAN:AV_HWDEVICE_TYPE_VAAPI,reuseDevice);
}

DecodeResult decode_h264_preview(const EncodedImage& encoded) {
	return decode_ffmpeg_preview(encoded, AV_CODEC_ID_H264);
}

DecodeResult decode_hevc_vaapi_preview(const EncodedImage& encoded) {
	std::string errors;
	for (const auto& device : discover_vaapi_devices()) {
		if (!device.initialized) continue;
		auto decoded = decode_ffmpeg_preview(encoded, AV_CODEC_ID_HEVC, device.path.c_str());
		if (decoded.image) return decoded;
		errors += device.path + ": " + decoded.error + "; ";
	}
	return {.image=nullptr, .error="HEVC VA-API decoding failed: " + errors};
}

DecodeResult decode_hevc_qsv_preview(const EncodedImage& encoded) {
	std::string errors;
	for (const auto& device : discover_vaapi_devices()) {
		if (!device.initialized) continue;
		auto decoded = decode_ffmpeg_preview(encoded, AV_CODEC_ID_HEVC, device.path.c_str(), true);
		if (decoded.image) return decoded;
		errors += device.path + ": " + decoded.error + "; ";
	}
	return {.image=nullptr, .error="HEVC QSV decoding failed: " + errors};
}

DecodeResult decode_hevc_preview(const EncodedImage& encoded) {
	// SCC is profile 9 in the SPS profile_tier_level. libde265 and FFmpeg's
	// native CPU decoder cannot decode its palette/IBC coding tools.
	for (std::size_t i = 0; i + 6 < encoded.hevcAnnexB.size(); ++i) {
		const auto* bytes = reinterpret_cast<const uint8_t*>(encoded.hevcAnnexB.data());
		if (bytes[i] || bytes[i+1] || bytes[i+2] != 1 || ((bytes[i+3] >> 1) & 63) != 33) continue;
		if ((bytes[i+6] & 31) != 9) break;
		return decode_scc_vulkan(encoded);
	}
	if (encoded.hevcAnnexB.empty()) {
		return {.image=nullptr, .error="HEVC encoded byte buffer is empty"};
	}
	std::unique_ptr<de265_decoder_context, decltype(&de265_free_decoder)> const decoder(de265_new_decoder(), de265_free_decoder);
	if (!decoder) {
		return {.image=nullptr, .error="libde265 decoder allocation failed"};
	}
	try {
		if (encoded.hevcAnnexB.size() > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
			throw std::runtime_error("HEVC byte buffer is too large for libde265 push API");
		}
		// Large still images can legitimately contain a slice larger than the
		// default 16 MiB NAL limit. Bound it by the bytes already supplied by
		// the caller; keep the independent image-size and SEI limits intact.
		const auto sar = hevc_pixel_aspect_ratio(encoded);
		auto limits = *de265_get_security_limits(decoder.get());
		limits.max_NAL_size_bytes = std::max(limits.max_NAL_size_bytes,
			static_cast<uint32_t>(encoded.hevcAnnexB.size()));
		de265_set_security_limits(decoder.get(), &limits);
		const auto codecStart=std::chrono::steady_clock::now();
		checked_de265(
			de265_push_data(decoder.get(), encoded.hevcAnnexB.data(), static_cast<int>(encoded.hevcAnnexB.size()), 0, nullptr),
			"de265_push_data"
		);
		de265_push_end_of_frame(decoder.get());
		checked_de265(de265_flush_data(decoder.get()), "de265_flush_data");
		for (int i = 0; i < 128; ++i) {
			int more = 0;
			const de265_error err = de265_decode(decoder.get(), &more);
			if (err == DE265_ERROR_WAITING_FOR_INPUT_DATA && more == 0) {
				break;
			}
			if (err == DE265_ERROR_IMAGE_BUFFER_FULL) {
				if (const de265_image* picture = de265_get_next_picture(decoder.get())) {
					const double seconds=std::chrono::duration<double>(std::chrono::steady_clock::now()-codecStart).count();
					return {.image=copy_de265_picture(picture,encoded.codedColor,sar), .error={}, .codecSeconds=seconds};
				}
			} else {
				checked_de265(err, "de265_decode");
			}
			if (const de265_image* picture = de265_get_next_picture(decoder.get())) {
				const double seconds=std::chrono::duration<double>(std::chrono::steady_clock::now()-codecStart).count();
				return {.image=copy_de265_picture(picture,encoded.codedColor,sar), .error={}, .codecSeconds=seconds};
			}
			if (more == 0) {
				break;
			}
		}
		return {.image=nullptr, .error="libde265 produced no preview picture"};
	} catch (const std::exception& e) {
		return {.image=nullptr, .error=e.what()};
	}
}

DecodeResult decode_av1_preview(const EncodedImage& encoded) {
	if (encoded.hevcAnnexB.empty()) {
		return {.image=nullptr, .error="AV1 encoded byte buffer is empty"};
	}
	Dav1dSettings settings;
	dav1d_default_settings(&settings);
	settings.n_threads = 0;
	settings.max_frame_delay = 1;
	settings.apply_grain = 0;
	settings.strict_std_compliance = 1;
	Dav1dContext* rawContext = nullptr;
	const int openErr = dav1d_open(&rawContext, &settings);
	if (openErr < 0) {
		return {.image=nullptr, .error=dav1d_error(openErr, "dav1d_open")};
	}
	std::unique_ptr<Dav1dContext, void (*)(Dav1dContext*)> const decoder(rawContext, [](Dav1dContext* ctx) {
		dav1d_close(&ctx);
	});

	try {
		const IvfFrame frame = first_ivf_frame(encoded.hevcAnnexB);
		Dav1dData data{};
		uint8_t* dst = dav1d_data_create(&data, frame.size);
		if (dst == nullptr) {
			throw std::runtime_error("dav1d input allocation failed");
		}
		std::memcpy(dst, frame.data, frame.size);

		const auto codecStart=std::chrono::steady_clock::now();
		int err = 0;
		while (data.sz > 0) {
			err = dav1d_send_data(decoder.get(), &data);
			if (err == DAV1D_ERR(EAGAIN)) {
				Dav1dPicture pending{};
				const int picErr = dav1d_get_picture(decoder.get(), &pending);
				if (picErr == 0) {
					const double seconds=std::chrono::duration<double>(std::chrono::steady_clock::now()-codecStart).count();
					auto image = copy_dav1d_picture(pending);
					dav1d_picture_unref(&pending);
					dav1d_data_unref(&data);
					return {.image=std::move(image), .error={}, .codecSeconds=seconds};
				}
				if (picErr != DAV1D_ERR(EAGAIN)) {
					dav1d_data_unref(&data);
					throw std::runtime_error(dav1d_error(picErr, "dav1d_get_picture"));
				}
				continue;
			}
			if (err < 0) {
				dav1d_data_unref(&data);
				throw std::runtime_error(dav1d_error(err, "dav1d_send_data"));
			}
		}

		for (int i = 0; i < 128; ++i) {
			Dav1dPicture picture{};
			err = dav1d_get_picture(decoder.get(), &picture);
			if (err == 0) {
				const double seconds=std::chrono::duration<double>(std::chrono::steady_clock::now()-codecStart).count();
				auto image = copy_dav1d_picture(picture);
				dav1d_picture_unref(&picture);
				return {.image=std::move(image), .error={}, .codecSeconds=seconds};
			}
			if (err == DAV1D_ERR(EAGAIN)) {
				break;
			}
			throw std::runtime_error(dav1d_error(err, "dav1d_get_picture"));
		}
		return {.image=nullptr, .error="dav1d produced no preview picture"};
	} catch (const std::exception& e) {
		return {.image=nullptr, .error=e.what()};
	}
}

DecodeResult decode_av2_preview(const EncodedImage& encoded) {
    if (encoded.hevcAnnexB.empty()) return {.image=nullptr, .error="AV2 encoded byte buffer is empty"};
    try {
        const IvfFrame frame = first_ivf_frame(encoded.hevcAnnexB, "AV02", "AV2");
        // The installed dav2d crashes on valid monochrome flat/edge streams
        // that the matching AVM decoder accepts. Decode coded bytes with AVM
        // directly; a fallback cannot recover from a process-level crash.
        double seconds;
        auto image=decode_av2_reference_frame({reinterpret_cast<const std::byte*>(frame.data), frame.size},&seconds);
        return {.image=std::move(image), .error={}, .codecSeconds=seconds};
    } catch (const std::exception& error) {
        return {.image=nullptr, .error=error.what()};
    }
}

DecodeResult decode_vvc_preview(const EncodedImage& encoded) {
	if (encoded.hevcAnnexB.empty()) {
		return {.image=nullptr, .error="VVC encoded byte buffer is empty"};
	}
	if (encoded.hevcAnnexB.size() > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
		return {.image=nullptr, .error="VVC byte buffer is too large for vvdec access unit API"};
	}

	vvdecParams params;
	vvdec_params_default(&params);
	params.logLevel = VVDEC_SILENT;
	params.threads = -1;
	params.parseDelay = 0;
	params.simd = VVDEC_SIMD_DEFAULT;
	params.verifyPictureHash = false;

	std::unique_ptr<vvdecDecoder, decltype(&vvdec_decoder_close)> const decoder(vvdec_decoder_open(&params), vvdec_decoder_close);
	if (!decoder) {
		return {.image=nullptr, .error="vvdec decoder allocation failed"};
	}
	std::unique_ptr<vvdecAccessUnit, decltype(&vvdec_accessUnit_free)> accessUnit(vvdec_accessUnit_alloc(), vvdec_accessUnit_free);
	if (!accessUnit) {
		return {.image=nullptr, .error="vvdec access unit allocation failed"};
	}
	auto take_frame = [&](vvdecFrame* frame) {
		const bool hasColor = frame->picAttributes && frame->picAttributes->vui &&
			frame->picAttributes->vui->colourDescriptionPresentFlag;
		auto image = copy_vvdec_frame(frame);
		vvdec_frame_unref(decoder.get(), frame);
		if (encoded.requiredStreamColor) {
			const auto& expected = *encoded.requiredStreamColor;
			const auto& actual = image->color;
			if (!hasColor || actual.primaries != expected.primaries || actual.transfer != expected.transfer ||
				actual.matrix != expected.matrix || actual.range != expected.range ||
				(is_420(image->format) && actual.chroma420Location != expected.chroma420Location))
				throw std::runtime_error("VVC stream color verification failed: decoded VUI differs from the encoder input");
		}
		return image;
	};

	try {
		vvdec_accessUnit_alloc_payload(accessUnit.get(), static_cast<int>(encoded.hevcAnnexB.size()));
		if (accessUnit->payload == nullptr) {
			throw std::runtime_error("vvdec access unit payload allocation failed");
		}
		std::memcpy(accessUnit->payload, encoded.hevcAnnexB.data(), encoded.hevcAnnexB.size());
		accessUnit->payloadUsedSize = static_cast<int>(encoded.hevcAnnexB.size());
		accessUnit->rap = true;

		const auto codecStart=std::chrono::steady_clock::now();
		vvdecFrame* frame = nullptr;
		int ret = vvdec_decode(decoder.get(), accessUnit.get(), &frame);
		if (ret != VVDEC_OK && ret != VVDEC_TRY_AGAIN) {
			throw std::runtime_error(vvdec_error(decoder.get(), ret, "vvdec_decode"));
		}
		if (frame != nullptr) {
			const double seconds=std::chrono::duration<double>(std::chrono::steady_clock::now()-codecStart).count();
			auto image = take_frame(frame);
			return {.image=std::move(image), .error={}, .codecSeconds=seconds};
		}

		for (int i = 0; i < 128; ++i) {
			frame = nullptr;
			ret = vvdec_flush(decoder.get(), &frame);
			if (ret == VVDEC_EOF) {
				break;
			}
			if (ret != VVDEC_OK && ret != VVDEC_TRY_AGAIN) {
				throw std::runtime_error(vvdec_error(decoder.get(), ret, "vvdec_flush"));
			}
			if (frame != nullptr) {
				const double seconds=std::chrono::duration<double>(std::chrono::steady_clock::now()-codecStart).count();
				auto image = take_frame(frame);
				return {.image=std::move(image), .error={}, .codecSeconds=seconds};
			}
			if (ret == VVDEC_TRY_AGAIN) {
				break;
			}
		}
		return {.image=nullptr, .error="vvdec produced no preview picture"};
	} catch (const std::exception& e) {
		return {.image=nullptr, .error=e.what()};
	}
}

} // namespace codec_gui::gui
