#include "codec_gui_image_io.hpp"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cctype>
#include <csetjmp>
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

unsigned char clamp_u8(const double value) {
	const long rounded = std::lround(value);
	return static_cast<unsigned char>(std::clamp<long>(rounded, 0, 255));
}

unsigned short clamp_u10(const double value) {
	const long rounded = std::lround(value);
	return static_cast<unsigned short>(std::clamp<long>(rounded, 0, 1023));
}

struct YCbCr {
	unsigned char y;
	unsigned char cb;
	unsigned char cr;
};

struct YCbCr10 {
	unsigned short y;
	unsigned short cb;
	unsigned short cr;
};

YCbCr rgb_to_bt709_limited(const double r, const double g, const double b) {
	const double yFull = 0.2126 * r + 0.7152 * g + 0.0722 * b;
	const double y = 16.0 + 219.0 * yFull / 255.0;
	const double cb = 128.0 + 224.0 * (b - yFull) / (2.0 * (255.0 - 255.0 * 0.0722));
	const double cr = 128.0 + 224.0 * (r - yFull) / (2.0 * (255.0 - 255.0 * 0.2126));
	return {clamp_u8(y), clamp_u8(cb), clamp_u8(cr)};
}

YCbCr10 rgb16_to_bt709_limited_10(const double r, const double g, const double b) {
	const double yFull = 0.2126 * r + 0.7152 * g + 0.0722 * b;
	const double y = 64.0 + 876.0 * yFull / 65535.0;
	const double cb = 512.0 + 896.0 * (b - yFull) / (2.0 * (65535.0 - 65535.0 * 0.0722));
	const double cr = 512.0 + 896.0 * (r - yFull) / (2.0 * (65535.0 - 65535.0 * 0.2126));
	return {clamp_u10(y), clamp_u10(cb), clamp_u10(cr)};
}

void store_u16le(std::vector<unsigned char>& bytes, const std::size_t sampleIndex, const unsigned short value) {
	bytes[sampleIndex * 2 + 0] = static_cast<unsigned char>(value & 0xffu);
	bytes[sampleIndex * 2 + 1] = static_cast<unsigned char>(value >> 8u);
}

RawImage rgb_to_yuv420_bt709_limited(const RgbImage& rgb) {
	if (rgb.width <= 0 || rgb.height <= 0) {
		throw std::runtime_error("JPEG dimensions must be positive");
	}

	const int width = (rgb.width + 1) & ~1;
	const int height = (rgb.height + 1) & ~1;
	RawImage image;
	image.width = width;
	image.height = height;
	image.format = PixelFormat::YUV420P8;
	image.color = {ColorPrimaries::BT709, TransferCharacteristics::SRGB, MatrixCoefficients::BT709, ColorRange::Limited, Chroma420SampleLocation::LeftCenter};
	image.planes[0].strideBytes = width;
	image.planes[1].strideBytes = width / 2;
	image.planes[2].strideBytes = width / 2;
	image.planes[0].bytes.resize(static_cast<std::size_t>(width) * static_cast<std::size_t>(height));
	image.planes[1].bytes.resize(static_cast<std::size_t>(width / 2) * static_cast<std::size_t>(height / 2));
	image.planes[2].bytes.resize(static_cast<std::size_t>(width / 2) * static_cast<std::size_t>(height / 2));

	auto rgb_at = [&](const int x, const int y) -> const unsigned char* {
		const int sx = std::min(x, rgb.width - 1);
		const int sy = std::min(y, rgb.height - 1);
		return rgb.rgb.data() + (static_cast<std::size_t>(sy) * rgb.width + sx) * 3;
	};

	for (int y = 0; y < height; ++y) {
		for (int x = 0; x < width; ++x) {
			const unsigned char* p = rgb_at(x, y);
			image.planes[0].bytes[static_cast<std::size_t>(y) * width + x] =
				rgb_to_bt709_limited(p[0], p[1], p[2]).y;
		}
	}

	for (int y = 0; y < height / 2; ++y) {
		for (int x = 0; x < width / 2; ++x) {
			double r = 0.0;
			double g = 0.0;
			double b = 0.0;
			for (int dy = 0; dy < 2; ++dy) {
				for (int dx = 0; dx < 2; ++dx) {
					const unsigned char* p = rgb_at(x * 2 + dx, y * 2 + dy);
					r += p[0];
					g += p[1];
					b += p[2];
				}
			}
			const YCbCr c = rgb_to_bt709_limited(r * 0.25, g * 0.25, b * 0.25);
			const auto i = static_cast<std::size_t>(y) * (width / 2) + x;
			image.planes[1].bytes[i] = c.cb;
			image.planes[2].bytes[i] = c.cr;
		}
	}
	return image;
}

RawImage rgb16_to_yuv420p10_bt709_limited(const Rgb16Image& rgb) {
	if (rgb.width <= 0 || rgb.height <= 0) {
		throw std::runtime_error("RAW dimensions must be positive");
	}

	const int width = (rgb.width + 1) & ~1;
	const int height = (rgb.height + 1) & ~1;
	RawImage image;
	image.width = width;
	image.height = height;
	image.format = PixelFormat::YUV420P10LE;
	image.color = {ColorPrimaries::BT709, TransferCharacteristics::SRGB, MatrixCoefficients::BT709, ColorRange::Limited, Chroma420SampleLocation::LeftCenter};
	image.planes[0].strideBytes = width * 2;
	image.planes[1].strideBytes = width;
	image.planes[2].strideBytes = width;
	image.planes[0].bytes.resize(static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 2);
	image.planes[1].bytes.resize(static_cast<std::size_t>(width / 2) * static_cast<std::size_t>(height / 2) * 2);
	image.planes[2].bytes.resize(static_cast<std::size_t>(width / 2) * static_cast<std::size_t>(height / 2) * 2);

	auto rgb_at = [&](const int x, const int y) -> const unsigned short* {
		const int sx = std::min(x, rgb.width - 1);
		const int sy = std::min(y, rgb.height - 1);
		return rgb.rgb.data() + (static_cast<std::size_t>(sy) * rgb.width + sx) * 3;
	};

	for (int y = 0; y < height; ++y) {
		for (int x = 0; x < width; ++x) {
			const unsigned short* p = rgb_at(x, y);
			store_u16le(
				image.planes[0].bytes,
				static_cast<std::size_t>(y) * width + x,
				rgb16_to_bt709_limited_10(p[0], p[1], p[2]).y
			);
		}
	}

	for (int y = 0; y < height / 2; ++y) {
		for (int x = 0; x < width / 2; ++x) {
			double r = 0.0;
			double g = 0.0;
			double b = 0.0;
			for (int dy = 0; dy < 2; ++dy) {
				for (int dx = 0; dx < 2; ++dx) {
					const unsigned short* p = rgb_at(x * 2 + dx, y * 2 + dy);
					r += p[0];
					g += p[1];
					b += p[2];
				}
			}
			const YCbCr10 c = rgb16_to_bt709_limited_10(r * 0.25, g * 0.25, b * 0.25);
			const auto i = static_cast<std::size_t>(y) * (width / 2) + x;
			store_u16le(image.planes[1].bytes, i, c.cb);
			store_u16le(image.planes[2].bytes, i, c.cr);
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
		return rgb16_to_yuv420p10_bt709_limited(load_nef_rgb16(path));
	}
	if (has_extension(path, ".jpg") || has_extension(path, ".jpeg")) {
		return rgb_to_yuv420_bt709_limited(load_jpeg_rgb(path));
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
