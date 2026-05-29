#include "preview_decoders.hpp"

#include <algorithm>
#include <cstddef>
#include <cstring>
#include <cerrno>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
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
#include <wels/codec_api.h>
#include <wels/codec_app_def.h>
#include <wels/codec_def.h>

extern "C" {
#include <dav1d/dav1d.h>
#include <jpeglib.h>
#include <jxrlib/JXRGlue.h>
#include <jxrlib/windowsmediaphoto.h>
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

uint8_t clamp_u8(double value) {
	return static_cast<uint8_t>(std::clamp<int>(static_cast<int>(std::lround(value)), 0, 255));
}

std::shared_ptr<const RawImage> rgb8_to_yuv444_preview(const std::vector<uint8_t>& rgb, int width, int height) {
	auto image = std::make_shared<RawImage>();
	image->width = width;
	image->height = height;
	image->format = PixelFormat::YUV444P8;
	image->color.range = ColorRange::Limited;
	for (int p = 0; p < 3; ++p) {
		image->planes[p].strideBytes = width;
		image->planes[p].bytes.resize(static_cast<std::size_t>(width) * static_cast<std::size_t>(height));
	}
	for (int y = 0; y < height; ++y) {
		for (int x = 0; x < width; ++x) {
			const std::size_t i = (static_cast<std::size_t>(y) * width + x) * 3;
			const double r = rgb[i + 0];
			const double g = rgb[i + 1];
			const double b = rgb[i + 2];
			const double yy = 0.2126 * r + 0.7152 * g + 0.0722 * b;
			const std::size_t o = static_cast<std::size_t>(y) * width + x;
			image->planes[0].bytes[o] = clamp_u8(16.0 + 219.0 * yy / 255.0);
			image->planes[1].bytes[o] = clamp_u8(128.0 + 224.0 * (b - yy) / (2.0 * (255.0 - 255.0 * 0.0722)));
			image->planes[2].bytes[o] = clamp_u8(128.0 + 224.0 * (r - yy) / (2.0 * (255.0 - 255.0 * 0.2126)));
		}
	}
	return image;
}

std::shared_ptr<const RawImage> copy_i420_preview(const unsigned char* const planes[3], const int strides[2], int width, int height) {
	auto image = std::make_shared<RawImage>();
	image->width = width;
	image->height = height;
	image->format = PixelFormat::YUV420P8;
	image->color.range = ColorRange::Limited;
	image->planes[0].strideBytes = width;
	image->planes[1].strideBytes = (width + 1) / 2;
	image->planes[2].strideBytes = (width + 1) / 2;
	image->planes[0].bytes.resize(static_cast<std::size_t>(width) * height);
	image->planes[1].bytes.resize(static_cast<std::size_t>((width + 1) / 2) * ((height + 1) / 2));
	image->planes[2].bytes.resize(static_cast<std::size_t>((width + 1) / 2) * ((height + 1) / 2));
	for (int y = 0; y < height; ++y) {
		std::memcpy(
			image->planes[0].bytes.data() + static_cast<std::size_t>(y) * image->planes[0].strideBytes,
			planes[0] + static_cast<std::size_t>(y) * strides[0],
			static_cast<std::size_t>(width)
		);
	}
	const int chromaWidth = (width + 1) / 2;
	const int chromaHeight = (height + 1) / 2;
	for (int y = 0; y < chromaHeight; ++y) {
		std::memcpy(
			image->planes[1].bytes.data() + static_cast<std::size_t>(y) * image->planes[1].strideBytes,
			planes[1] + static_cast<std::size_t>(y) * strides[1],
			static_cast<std::size_t>(chromaWidth)
		);
		std::memcpy(
			image->planes[2].bytes.data() + static_cast<std::size_t>(y) * image->planes[2].strideBytes,
			planes[2] + static_cast<std::size_t>(y) * strides[1],
			static_cast<std::size_t>(chromaWidth)
		);
	}
	return image;
}

std::vector<uint8_t> bytes_to_u8(const std::vector<std::byte>& bytes) {
	std::vector<uint8_t> out(bytes.size());
	std::memcpy(out.data(), bytes.data(), bytes.size());
	return out;
}

void write_temp_file(const std::filesystem::path& path, const std::vector<std::byte>& bytes) {
	std::ofstream out(path, std::ios::binary);
	if (!out) {
		throw std::runtime_error("failed to open temporary decode file: " + path.string());
	}
	out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
	if (!out) {
		throw std::runtime_error("failed to write temporary decode file: " + path.string());
	}
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

uint8_t opj_component_u8(const opj_image_comp_t& comp, std::size_t index) {
	const int value = comp.data[index];
	const int maxValue = comp.prec <= 8 ? 255 : ((1 << std::min<int>(comp.prec, 16)) - 1);
	return clamp_u8(static_cast<double>(value) * 255.0 / static_cast<double>(std::max(1, maxValue)));
}

struct IvfFrame {
	const uint8_t* data = nullptr;
	std::size_t size = 0;
};

IvfFrame first_ivf_frame(const std::vector<std::byte>& bytes) {
	if (bytes.size() < 44) {
		throw std::runtime_error("AV1 IVF byte buffer is too small");
	}
	if (std::memcmp(bytes.data(), "DKIF", 4) != 0) {
		throw std::runtime_error("AV1 preview expects IVF data");
	}
	if (read_u16le(bytes.data() + 6) != 32) {
		throw std::runtime_error("unsupported IVF header size");
	}
	if (std::memcmp(bytes.data() + 8, "AV01", 4) != 0) {
		throw std::runtime_error("IVF stream is not AV1");
	}
	const uint32_t frameSize = read_u32le(bytes.data() + 32);
	if (frameSize == 0 || 44ull + frameSize > bytes.size()) {
		throw std::runtime_error("invalid IVF frame size");
	}
	return {reinterpret_cast<const uint8_t*>(bytes.data() + 44), frameSize};
}

std::string dav1d_error(int err, const char* operation) {
	return std::string(operation) + " failed: " + std::strerror(-err);
}

PixelFormat hevc_pixel_format(const de265_image* picture) {
	const int bits = de265_get_bits_per_pixel(picture, 0);
	const de265_chroma chroma = de265_get_chroma_format(picture);
	if (bits <= 8 && chroma == de265_chroma_420) {
		return PixelFormat::YUV420P8;
	}
	if (bits <= 10 && chroma == de265_chroma_420) {
		return PixelFormat::YUV420P10LE;
	}
	if (bits <= 8 && chroma == de265_chroma_422) {
		return PixelFormat::YUV422P8;
	}
	if (bits <= 10 && chroma == de265_chroma_422) {
		return PixelFormat::YUV422P10LE;
	}
	if (bits <= 8 && chroma == de265_chroma_444) {
		return PixelFormat::YUV444P8;
	}
	if (bits <= 10 && chroma == de265_chroma_444) {
		return PixelFormat::YUV444P10LE;
	}
	throw std::runtime_error("unsupported HEVC preview format");
}

std::shared_ptr<const RawImage> copy_de265_picture(const de265_image* picture) {
	auto image = std::make_shared<RawImage>();
	image->width = de265_get_image_width(picture, 0);
	image->height = de265_get_image_height(picture, 0);
	image->format = hevc_pixel_format(picture);
	const int bytesPerSample = de265_get_bits_per_pixel(picture, 0) <= 8 ? 1 : 2;
	for (int plane = 0; plane < 3; ++plane) {
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
	if (picture.p.bpc <= 8 && picture.p.layout == DAV1D_PIXEL_LAYOUT_I422) {
		return PixelFormat::YUV422P8;
	}
	if (picture.p.bpc <= 10 && picture.p.layout == DAV1D_PIXEL_LAYOUT_I422) {
		return PixelFormat::YUV422P10LE;
	}
	if (picture.p.bpc <= 8 && picture.p.layout == DAV1D_PIXEL_LAYOUT_I444) {
		return PixelFormat::YUV444P8;
	}
	if (picture.p.bpc <= 10 && picture.p.layout == DAV1D_PIXEL_LAYOUT_I444) {
		return PixelFormat::YUV444P10LE;
	}
	if (picture.p.bpc <= 8 && picture.p.layout == DAV1D_PIXEL_LAYOUT_I400) {
		return PixelFormat::Gray8;
	}
	if (picture.p.bpc <= 10 && picture.p.layout == DAV1D_PIXEL_LAYOUT_I400) {
		return PixelFormat::Gray10LE;
	}
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

DecodeResult no_preview_decode(const EncodedImage&) {
	return {nullptr, "preview decoder is not wired for this backend yet"};
}

DecodeResult decode_embedded_preview(const EncodedImage& encoded) {
	if (encoded.previewImage) {
		return {encoded.previewImage, {}};
	}
	return {nullptr, "encoded preview image is unavailable"};
}

DecodeResult decode_jpegls_preview(const EncodedImage& encoded) {
	if (encoded.hevcAnnexB.empty()) {
		return {nullptr, "JPEG-LS encoded byte buffer is empty"};
	}
	try {
		const std::vector<uint8_t> bytes = bytes_to_u8(encoded.hevcAnnexB);
		charls::jpegls_decoder decoder{bytes, true};
		const charls::frame_info info = decoder.frame_info();
		if (info.bits_per_sample != 8 || info.component_count != 3) {
			throw std::runtime_error("unsupported JPEG-LS preview format");
		}
		std::vector<uint8_t> rgb(static_cast<std::size_t>(info.width) * info.height * 3);
		decoder.decode(rgb, static_cast<uint32_t>(info.width * 3));
		return {rgb8_to_yuv444_preview(rgb, static_cast<int>(info.width), static_cast<int>(info.height)), {}};
	} catch (const std::exception& e) {
		return {nullptr, e.what()};
	}
}

DecodeResult decode_jpeg_preview(const EncodedImage& encoded) {
	if (encoded.hevcAnnexB.empty()) {
		return {nullptr, "JPEG encoded byte buffer is empty"};
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
		cinfo.out_color_space = JCS_RGB;
		jpeg_start_decompress(&cinfo);
		std::vector<uint8_t> rgb(static_cast<std::size_t>(cinfo.output_width) * cinfo.output_height * 3);
		while (cinfo.output_scanline < cinfo.output_height) {
			JSAMPROW row = rgb.data() + static_cast<std::size_t>(cinfo.output_scanline) * cinfo.output_width * 3;
			jpeg_read_scanlines(&cinfo, &row, 1);
		}
		const int width = static_cast<int>(cinfo.output_width);
		const int height = static_cast<int>(cinfo.output_height);
		jpeg_finish_decompress(&cinfo);
		jpeg_destroy_decompress(&cinfo);
		return {rgb8_to_yuv444_preview(rgb, width, height), {}};
	} catch (const std::exception& e) {
		jpeg_destroy_decompress(&cinfo);
		return {nullptr, e.what()};
	}
}

DecodeResult decode_png_preview(const EncodedImage& encoded) {
	if (encoded.hevcAnnexB.empty()) {
		return {nullptr, "PNG encoded byte buffer is empty"};
	}
	png_structp png = png_create_read_struct(PNG_LIBPNG_VER_STRING, nullptr, nullptr, nullptr);
	if (!png) return {nullptr, "png_create_read_struct failed"};
	png_infop info = png_create_info_struct(png);
	if (!info) {
		png_destroy_read_struct(&png, nullptr, nullptr);
		return {nullptr, "png_create_info_struct failed"};
	}
	try {
		if (setjmp(png_jmpbuf(png))) {
			throw std::runtime_error("libpng decode failed");
		}
		const std::vector<uint8_t> bytes = bytes_to_u8(encoded.hevcAnnexB);
		PngMemoryReader reader{bytes.data(), bytes.size(), 0};
		png_set_read_fn(png, &reader, png_read_callback);
		png_read_info(png, info);
		const int width = static_cast<int>(png_get_image_width(png, info));
		const int height = static_cast<int>(png_get_image_height(png, info));
		const int colorType = png_get_color_type(png, info);
		const int bitDepth = png_get_bit_depth(png, info);
		if (bitDepth == 16) png_set_strip_16(png);
		if (colorType == PNG_COLOR_TYPE_PALETTE) png_set_palette_to_rgb(png);
		if (colorType == PNG_COLOR_TYPE_GRAY && bitDepth < 8) png_set_expand_gray_1_2_4_to_8(png);
		if (png_get_valid(png, info, PNG_INFO_tRNS)) png_set_tRNS_to_alpha(png);
		if (colorType == PNG_COLOR_TYPE_GRAY || colorType == PNG_COLOR_TYPE_GRAY_ALPHA) png_set_gray_to_rgb(png);
		if (colorType & PNG_COLOR_MASK_ALPHA) png_set_strip_alpha(png);
		png_read_update_info(png, info);
		std::vector<uint8_t> rgb(static_cast<std::size_t>(width) * height * 3);
		std::vector<png_bytep> rows(static_cast<std::size_t>(height));
		for (int y = 0; y < height; ++y) {
			rows[y] = rgb.data() + static_cast<std::size_t>(y) * width * 3;
		}
		png_read_image(png, rows.data());
		png_destroy_read_struct(&png, &info, nullptr);
		return {rgb8_to_yuv444_preview(rgb, width, height), {}};
	} catch (const std::exception& e) {
		png_destroy_read_struct(&png, &info, nullptr);
		return {nullptr, e.what()};
	}
}

DecodeResult decode_jpeg2000_preview(const EncodedImage& encoded) {
	if (encoded.hevcAnnexB.empty()) {
		return {nullptr, "JPEG 2000 encoded byte buffer is empty"};
	}
	const std::filesystem::path path = std::filesystem::temp_directory_path() / "codec_vis_preview_decode.jp2";
	try {
		write_temp_file(path, encoded.hevcAnnexB);
		std::unique_ptr<opj_codec_t, decltype(&opj_destroy_codec)> codec(opj_create_decompress(OPJ_CODEC_JP2), opj_destroy_codec);
		if (!codec) throw std::runtime_error("OpenJPEG decoder allocation failed");
		opj_dparameters_t params{};
		opj_set_default_decoder_parameters(&params);
		if (!opj_setup_decoder(codec.get(), &params)) throw std::runtime_error("OpenJPEG decoder setup failed");
		std::unique_ptr<opj_stream_t, decltype(&opj_stream_destroy)> stream(opj_stream_create_default_file_stream(path.c_str(), OPJ_TRUE), opj_stream_destroy);
		if (!stream) throw std::runtime_error("OpenJPEG stream open failed");
		opj_image_t* rawImage = nullptr;
		if (!opj_read_header(stream.get(), codec.get(), &rawImage)) throw std::runtime_error("OpenJPEG read header failed");
		std::unique_ptr<opj_image_t, decltype(&opj_image_destroy)> image(rawImage, opj_image_destroy);
		if (!opj_decode(codec.get(), stream.get(), image.get()) || !opj_end_decompress(codec.get(), stream.get())) {
			throw std::runtime_error("OpenJPEG decode failed");
		}
		if (image->numcomps < 3) throw std::runtime_error("unsupported JPEG 2000 component count");
		const int width = static_cast<int>(image->comps[0].w);
		const int height = static_cast<int>(image->comps[0].h);
		std::vector<uint8_t> rgb(static_cast<std::size_t>(width) * height * 3);
		for (int y = 0; y < height; ++y) {
			for (int x = 0; x < width; ++x) {
				const std::size_t i = static_cast<std::size_t>(y) * width + x;
				rgb[i * 3 + 0] = opj_component_u8(image->comps[0], i);
				rgb[i * 3 + 1] = opj_component_u8(image->comps[1], i);
				rgb[i * 3 + 2] = opj_component_u8(image->comps[2], i);
			}
		}
		std::filesystem::remove(path);
		return {rgb8_to_yuv444_preview(rgb, width, height), {}};
	} catch (const std::exception& e) {
		std::filesystem::remove(path);
		return {nullptr, e.what()};
	}
}

DecodeResult decode_jpegxl_preview(const EncodedImage& encoded) {
	if (encoded.hevcAnnexB.empty()) {
		return {nullptr, "JPEG XL encoded byte buffer is empty"};
	}
	try {
		std::unique_ptr<JxlDecoder, decltype(&JxlDecoderDestroy)> dec(JxlDecoderCreate(nullptr), JxlDecoderDestroy);
		if (!dec) throw std::runtime_error("JxlDecoderCreate failed");
		if (JxlDecoderSubscribeEvents(dec.get(), JXL_DEC_BASIC_INFO | JXL_DEC_FULL_IMAGE) != JXL_DEC_SUCCESS) {
			throw std::runtime_error("JxlDecoderSubscribeEvents failed");
		}
		if (JxlDecoderSetInput(dec.get(), reinterpret_cast<const uint8_t*>(encoded.hevcAnnexB.data()), encoded.hevcAnnexB.size()) != JXL_DEC_SUCCESS) {
			throw std::runtime_error("JxlDecoderSetInput failed");
		}
		JxlDecoderCloseInput(dec.get());
		JxlBasicInfo info{};
		std::vector<uint8_t> rgb;
		const JxlPixelFormat format{3, JXL_TYPE_UINT8, JXL_NATIVE_ENDIAN, 0};
		for (;;) {
			const JxlDecoderStatus status = JxlDecoderProcessInput(dec.get());
			if (status == JXL_DEC_BASIC_INFO) {
				if (JxlDecoderGetBasicInfo(dec.get(), &info) != JXL_DEC_SUCCESS) {
					throw std::runtime_error("JxlDecoderGetBasicInfo failed");
				}
				size_t outSize = 0;
				if (JxlDecoderImageOutBufferSize(dec.get(), &format, &outSize) != JXL_DEC_SUCCESS) {
					outSize = static_cast<std::size_t>(info.xsize) * info.ysize * 3;
				}
				rgb.resize(outSize);
				if (JxlDecoderSetImageOutBuffer(dec.get(), &format, rgb.data(), rgb.size()) != JXL_DEC_SUCCESS) {
					throw std::runtime_error("JxlDecoderSetImageOutBuffer failed");
				}
			} else if (status == JXL_DEC_FULL_IMAGE || status == JXL_DEC_SUCCESS) {
				if (rgb.empty() || info.xsize == 0 || info.ysize == 0) {
					throw std::runtime_error("JPEG XL decoder produced no image");
				}
				return {rgb8_to_yuv444_preview(rgb, static_cast<int>(info.xsize), static_cast<int>(info.ysize)), {}};
			} else if (status == JXL_DEC_NEED_MORE_INPUT) {
				throw std::runtime_error("JPEG XL decoder requested more input");
			} else if (status == JXL_DEC_ERROR) {
				throw std::runtime_error("JPEG XL decode failed");
			}
		}
	} catch (const std::exception& e) {
		return {nullptr, e.what()};
	}
}

DecodeResult decode_jpegxr_preview(const EncodedImage& encoded) {
	if (encoded.hevcAnnexB.empty()) {
		return {nullptr, "JPEG XR encoded byte buffer is empty"};
	}
	const std::filesystem::path path = std::filesystem::temp_directory_path() / "codec_vis_preview_decode.jxr";
	try {
		write_temp_file(path, encoded.hevcAnnexB);
		WMPStream* stream = nullptr;
		if (CreateWS_File(&stream, path.c_str(), "rb") != 0) throw std::runtime_error("jxrlib CreateWS_File failed");
		std::unique_ptr<WMPStream, void (*)(WMPStream*)> streamGuard(stream, [](WMPStream* s) {
			WMPStream* tmp = s;
			CloseWS_File(&tmp);
		});
		PKImageDecode* dec = nullptr;
		if (PKImageDecode_Create_WMP(&dec) != 0) throw std::runtime_error("jxrlib decoder allocation failed");
		std::unique_ptr<PKImageDecode, void (*)(PKImageDecode*)> decGuard(dec, [](PKImageDecode* d) {
			PKImageDecode* tmp = d;
			PKImageDecode_Release(&tmp);
		});
		if (dec->Initialize(dec, stream) != 0) throw std::runtime_error("jxrlib decoder initialize failed");
		I32 width = 0;
		I32 height = 0;
		if (dec->GetSize(dec, &width, &height) != 0 || width <= 0 || height <= 0) throw std::runtime_error("jxrlib GetSize failed");
		std::vector<uint8_t> rgb(static_cast<std::size_t>(width) * height * 3);
		PKRect rect{0, 0, width, height};
		if (dec->Copy(dec, &rect, rgb.data(), static_cast<U32>(width * 3)) != 0) throw std::runtime_error("jxrlib Copy failed");
		decGuard.reset();
		streamGuard.release();
		std::filesystem::remove(path);
		return {rgb8_to_yuv444_preview(rgb, width, height), {}};
	} catch (const std::exception& e) {
		std::filesystem::remove(path);
		return {nullptr, e.what()};
	}
}

DecodeResult decode_h264_preview(const EncodedImage& encoded) {
	if (encoded.hevcAnnexB.empty()) {
		return {nullptr, "H.264 encoded byte buffer is empty"};
	}
	ISVCDecoder* rawDecoder = nullptr;
	if (WelsCreateDecoder(&rawDecoder) != 0 || rawDecoder == nullptr) {
		return {nullptr, "OpenH264 decoder allocation failed"};
	}
	std::unique_ptr<ISVCDecoder, void (*)(ISVCDecoder*)> decoder(rawDecoder, WelsDestroyDecoder);
	try {
		SDecodingParam params{};
		params.sVideoProperty.eVideoBsType = VIDEO_BITSTREAM_AVC;
		params.eEcActiveIdc = ERROR_CON_DISABLE;
		params.bParseOnly = false;
		if (decoder->Initialize(&params) != 0) {
			throw std::runtime_error("OpenH264 decoder initialize failed");
		}
		unsigned char* planes[3]{};
		SBufferInfo info{};
		const DECODING_STATE state = decoder->DecodeFrameNoDelay(
			reinterpret_cast<const unsigned char*>(encoded.hevcAnnexB.data()),
			static_cast<int>(encoded.hevcAnnexB.size()),
			planes,
			&info
		);
		if ((state & (dsBitstreamError | dsInvalidArgument | dsInitialOptExpected | dsOutOfMemory | dsDstBufNeedExpan)) != 0) {
			throw std::runtime_error("OpenH264 decode failed");
		}
		if (info.iBufferStatus != 1) {
			const DECODING_STATE flushState = decoder->FlushFrame(planes, &info);
			if ((flushState & (dsBitstreamError | dsInvalidArgument | dsInitialOptExpected | dsOutOfMemory | dsDstBufNeedExpan)) != 0) {
				throw std::runtime_error("OpenH264 flush failed");
			}
		}
		if (info.iBufferStatus != 1 || planes[0] == nullptr || planes[1] == nullptr || planes[2] == nullptr) {
			throw std::runtime_error("OpenH264 produced no preview frame");
		}
		const SSysMEMBuffer& sys = info.UsrData.sSystemBuffer;
		if (sys.iFormat != videoFormatI420) {
			throw std::runtime_error("OpenH264 produced unsupported pixel format");
		}
		const int strides[2]{sys.iStride[0], sys.iStride[1]};
		return {copy_i420_preview(planes, strides, sys.iWidth, sys.iHeight), {}};
	} catch (const std::exception& e) {
		decoder->Uninitialize();
		return {nullptr, e.what()};
	}
}

DecodeResult decode_hevc_preview(const EncodedImage& encoded) {
	if (encoded.hevcAnnexB.empty()) {
		return {nullptr, "HEVC encoded byte buffer is empty"};
	}
	std::unique_ptr<de265_decoder_context, decltype(&de265_free_decoder)> decoder(de265_new_decoder(), de265_free_decoder);
	if (!decoder) {
		return {nullptr, "libde265 decoder allocation failed"};
	}
	try {
		if (encoded.hevcAnnexB.size() > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
			throw std::runtime_error("HEVC byte buffer is too large for libde265 push API");
		}
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
					return {copy_de265_picture(picture), {}};
				}
			} else {
				checked_de265(err, "de265_decode");
			}
			if (const de265_image* picture = de265_get_next_picture(decoder.get())) {
				return {copy_de265_picture(picture), {}};
			}
			if (more == 0) {
				break;
			}
		}
		return {nullptr, "libde265 produced no preview picture"};
	} catch (const std::exception& e) {
		return {nullptr, e.what()};
	}
}

DecodeResult decode_av1_preview(const EncodedImage& encoded) {
	if (encoded.hevcAnnexB.empty()) {
		return {nullptr, "AV1 encoded byte buffer is empty"};
	}
	Dav1dSettings settings;
	dav1d_default_settings(&settings);
	settings.n_threads = 1;
	settings.max_frame_delay = 1;
	settings.apply_grain = 0;
	settings.strict_std_compliance = 1;
	Dav1dContext* rawContext = nullptr;
	const int openErr = dav1d_open(&rawContext, &settings);
	if (openErr < 0) {
		return {nullptr, dav1d_error(openErr, "dav1d_open")};
	}
	std::unique_ptr<Dav1dContext, void (*)(Dav1dContext*)> decoder(rawContext, [](Dav1dContext* ctx) {
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

		int err = 0;
		while (data.sz > 0) {
			err = dav1d_send_data(decoder.get(), &data);
			if (err == DAV1D_ERR(EAGAIN)) {
				Dav1dPicture pending{};
				const int picErr = dav1d_get_picture(decoder.get(), &pending);
				if (picErr == 0) {
					auto image = copy_dav1d_picture(pending);
					dav1d_picture_unref(&pending);
					dav1d_data_unref(&data);
					return {std::move(image), {}};
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
				auto image = copy_dav1d_picture(picture);
				dav1d_picture_unref(&picture);
				return {std::move(image), {}};
			}
			if (err == DAV1D_ERR(EAGAIN)) {
				break;
			}
			throw std::runtime_error(dav1d_error(err, "dav1d_get_picture"));
		}
		return {nullptr, "dav1d produced no preview picture"};
	} catch (const std::exception& e) {
		return {nullptr, e.what()};
	}
}

DecodeResult decode_vvc_preview(const EncodedImage& encoded) {
	if (encoded.hevcAnnexB.empty()) {
		return {nullptr, "VVC encoded byte buffer is empty"};
	}
	if (encoded.hevcAnnexB.size() > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
		return {nullptr, "VVC byte buffer is too large for vvdec access unit API"};
	}

	vvdecParams params;
	vvdec_params_default(&params);
	params.logLevel = VVDEC_SILENT;
	params.threads = 1;
	params.parseDelay = 0;
	params.simd = VVDEC_SIMD_SCALAR;
	params.verifyPictureHash = false;

	std::unique_ptr<vvdecDecoder, decltype(&vvdec_decoder_close)> decoder(vvdec_decoder_open(&params), vvdec_decoder_close);
	if (!decoder) {
		return {nullptr, "vvdec decoder allocation failed"};
	}
	std::unique_ptr<vvdecAccessUnit, decltype(&vvdec_accessUnit_free)> accessUnit(vvdec_accessUnit_alloc(), vvdec_accessUnit_free);
	if (!accessUnit) {
		return {nullptr, "vvdec access unit allocation failed"};
	}

	try {
		vvdec_accessUnit_alloc_payload(accessUnit.get(), static_cast<int>(encoded.hevcAnnexB.size()));
		if (accessUnit->payload == nullptr) {
			throw std::runtime_error("vvdec access unit payload allocation failed");
		}
		std::memcpy(accessUnit->payload, encoded.hevcAnnexB.data(), encoded.hevcAnnexB.size());
		accessUnit->payloadUsedSize = static_cast<int>(encoded.hevcAnnexB.size());
		accessUnit->rap = true;

		vvdecFrame* frame = nullptr;
		int ret = vvdec_decode(decoder.get(), accessUnit.get(), &frame);
		if (ret != VVDEC_OK && ret != VVDEC_TRY_AGAIN) {
			throw std::runtime_error(vvdec_error(decoder.get(), ret, "vvdec_decode"));
		}
		if (frame != nullptr) {
			auto image = copy_vvdec_frame(frame);
			vvdec_frame_unref(decoder.get(), frame);
			return {std::move(image), {}};
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
				auto image = copy_vvdec_frame(frame);
				vvdec_frame_unref(decoder.get(), frame);
				return {std::move(image), {}};
			}
			if (ret == VVDEC_TRY_AGAIN) {
				break;
			}
		}
		return {nullptr, "vvdec produced no preview picture"};
	} catch (const std::exception& e) {
		return {nullptr, e.what()};
	}
}

} // namespace codec_gui::gui
