#include "codec_gui_image_io.hpp"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cctype>
#include <csetjmp>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>

#include <libraw/libraw.h>

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
};

struct Rgb16Image {
	int width = 0;
	int height = 0;
	std::vector<unsigned short> rgb;
};

struct PngImage {
	bool sixteenBit = false;
	RgbImage rgb8;
	Rgb16Image rgb16;
};

struct PngAllocations {
	void* pixels = nullptr;
	png_bytep* rows = nullptr;
};

PngImage load_png_rgb(const std::filesystem::path& path) {
	FILE* file = std::fopen(path.c_str(), "rb");
	if (file == nullptr) throw std::runtime_error("failed to open PNG file: " + path.string());

	png_structp png = png_create_read_struct(PNG_LIBPNG_VER_STRING, nullptr, nullptr, nullptr);
	if (png == nullptr) {
		std::fclose(file);
		throw std::runtime_error("failed to create PNG decoder");
	}
	png_infop info = png_create_info_struct(png);
	if (info == nullptr) {
		png_destroy_read_struct(&png, nullptr, nullptr);
		std::fclose(file);
		throw std::runtime_error("failed to create PNG metadata decoder");
	}

	PngAllocations* allocations = static_cast<PngAllocations*>(std::calloc(1, sizeof(PngAllocations)));
	if (allocations == nullptr) {
		png_destroy_read_struct(&png, &info, nullptr);
		std::fclose(file);
		throw std::runtime_error("out of memory creating PNG decoder");
	}
	if (setjmp(png_jmpbuf(png)) != 0) {
		std::free(allocations->rows);
		std::free(allocations->pixels);
		std::free(allocations);
		png_destroy_read_struct(&png, &info, nullptr);
		std::fclose(file);
		throw std::runtime_error("PNG decode failed: " + path.string());
	}

	png_init_io(png, file);
	png_read_info(png, info);
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
	png_read_image(png, allocations->rows);
	png_read_end(png, info);
	png_destroy_read_struct(&png, &info, nullptr);
	std::fclose(file);
	std::free(allocations->rows);

	PngImage image;
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
	return image;
}

bool has_extension(const std::filesystem::path& path, const std::string& extension) {
	std::string ext = path.extension().string();
	std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) {
		return static_cast<char>(std::tolower(c));
	});
	return ext == extension;
}

RgbImage load_jpeg_rgb(const std::filesystem::path& path) {
	const std::vector<unsigned char> bytes = read_file_bytes(path);
	if (bytes.empty()) {
		throw std::runtime_error("empty JPEG file: " + path.string());
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
	if (jpeg_read_header(&cinfo, TRUE) != JPEG_HEADER_OK) {
		throw std::runtime_error("not a JPEG image: " + path.string());
	}

	cinfo.out_color_space = JCS_RGB;
	jpeg_start_decompress(&cinfo);
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
	while (cinfo.output_scanline < cinfo.output_height) {
		unsigned char* row = image.rgb.data() +
		                     static_cast<std::size_t>(cinfo.output_scanline) *
		                         static_cast<std::size_t>(image.width) * 3;
		JSAMPROW rows[] = {row};
		jpeg_read_scanlines(&cinfo, rows, 1);
	}

	jpeg_finish_decompress(&cinfo);
	jpeg_destroy_decompress(&cinfo);
	return image;
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
	std::unique_ptr<libraw_processed_image_t, ProcessedImageDeleter> imageGuard{processed};

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
	image.color = {ColorPrimaries::BT709, TransferCharacteristics::SRGB, MatrixCoefficients::Identity, ColorRange::Full, std::nullopt};
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
	image.color = {ColorPrimaries::BT709, TransferCharacteristics::SRGB, MatrixCoefficients::Identity, ColorRange::Full, std::nullopt};
	for (ImagePlane& plane : image.planes) {
		plane.strideBytes = image.width * 2;
		plane.bytes.resize(static_cast<std::size_t>(image.width) * image.height * 2);
	}
	const uint32_t targetMaximum = (1u << sourceBitDepth) - 1u;
	for (std::size_t i = 0, pixels = static_cast<std::size_t>(image.width) * image.height; i < pixels; ++i) {
		for (int plane = 0; plane < 3; ++plane) {
			const uint32_t source = rgb.rgb[i * 3 + static_cast<std::size_t>(plane)];
			const uint16_t value = static_cast<uint16_t>((source * targetMaximum + 32767u) / 65535u);
			store_u16le(image.planes[plane].bytes, i, value);
		}
	}
	return image;
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
	if (has_extension(path, ".png")) {
		PngImage png = load_png_rgb(path);
		return png.sixteenBit
			? rgb16_to_planar_source(png.rgb16, 16)
			: rgb8_to_planar_source(png.rgb8);
	}
	if (has_extension(path, ".jpg") || has_extension(path, ".jpeg")) {
		return rgb8_to_planar_source(load_jpeg_rgb(path));
	}
	throw std::runtime_error("unsupported input format: " + path.string());
}

RawImage make_test_pattern() {
	RawImage image;
	constexpr unsigned int height = 1080;
	constexpr unsigned int width = 1920;
	image.width = width;
	image.height = height;
	image.format = PixelFormat::YUV420P8;
	image.color = {ColorPrimaries::BT709, TransferCharacteristics::SRGB, MatrixCoefficients::BT709, ColorRange::Limited, Chroma420SampleLocation::LeftCenter};
	image.planes[0].bytes.resize(height * width);
	image.planes[0].strideBytes = width;
	image.planes[1].bytes.resize(height * width / 4);
	image.planes[1].strideBytes = width / 2;
	image.planes[2].bytes.resize(height * width / 4);
	image.planes[2].strideBytes = width / 2;

	for (unsigned int y = 0; y < height; ++y) {
		for (unsigned int x = 0; x < width; ++x) {
			image.planes[0].bytes[y * width + x] =
				std::clamp(
					(std::sin(float(x) / float(height) * 4) / 2.0f + 0.5f) *
						(std::sin(float(y) / float(height) * 4) / 2.0f + 0.5f),
					0.0f,
					std::bit_cast<float>(std::bit_cast<unsigned int>(1.0f) - 1)
				) *
				256;
		}
	}

	for (unsigned int y = 0; y < height / 2; ++y) {
		for (unsigned int x = 0; x < width / 2; ++x) {
			image.planes[1].bytes[y * (width / 2) + x] =
				std::clamp(float(x) / float(width / 2), 0.0f, std::bit_cast<float>(std::bit_cast<unsigned int>(1.0f) - 1)) * 256;
			image.planes[2].bytes[y * (width / 2) + x] =
				std::clamp(float(y) / float(height / 2), 0.0f, std::bit_cast<float>(std::bit_cast<unsigned int>(1.0f) - 1)) * 256;
		}
	}
	return image;
}

} // namespace codec_gui
