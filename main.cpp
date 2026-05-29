#include "codec_gui_x265.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <csetjmp>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cctype>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

#include <libraw/libraw.h>

extern "C" {
#include <jpeglib.h>
}

namespace {

struct JpegError {
	jpeg_error_mgr pub;
	jmp_buf        jump;
	char           message[JMSG_LENGTH_MAX]{};
};

void jpeg_error_exit(j_common_ptr cinfo) {
	auto *err = reinterpret_cast<JpegError *>(cinfo->err);
	(*cinfo->err->format_message)(cinfo, err->message);
	longjmp(err->jump, 1);
}

void dump_to_file(const std::filesystem::path &path, const std::vector<std::byte> &data) {
	std::filesystem::remove(path);
	std::ofstream out(path, std::ios::binary);
	if (!out) throw std::runtime_error("failed to open file");

	out.write(reinterpret_cast<const char *>(data.data()), static_cast<std::streamsize>(data.size()));

	if (!out) throw std::runtime_error("failed to write file");
}

std::vector<uint8_t> read_file(const std::filesystem::path &path) {
	std::ifstream in(path, std::ios::binary);
	if (!in) throw std::runtime_error("failed to open input file: " + path.string());

	return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

struct RgbImage {
	int                  width  = 0;
	int                  height = 0;
	std::vector<uint8_t> rgb;
};

struct Rgb16Image {
	int                   width  = 0;
	int                   height = 0;
	std::vector<uint16_t> rgb;
};

RgbImage load_jpeg_rgb(const std::filesystem::path &path) {
	const std::vector<uint8_t> bytes = read_file(path);
	if (bytes.empty()) throw std::runtime_error("empty JPEG file: " + path.string());

	jpeg_decompress_struct cinfo{};
	JpegError              jerr{};

	cinfo.err           = jpeg_std_error(&jerr.pub);
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
	image.width  = static_cast<int>(cinfo.output_width);
	image.height = static_cast<int>(cinfo.output_height);
	image.rgb.resize(static_cast<std::size_t>(image.width) * static_cast<std::size_t>(image.height) * 3);

	while (cinfo.output_scanline < cinfo.output_height) {
		uint8_t *row = image.rgb.data() +
		               static_cast<std::size_t>(cinfo.output_scanline) *
		                   static_cast<std::size_t>(image.width) * 3;
		JSAMPROW rows[] = {row};
		jpeg_read_scanlines(&cinfo, rows, 1);
	}

	jpeg_finish_decompress(&cinfo);
	jpeg_destroy_decompress(&cinfo);

	return image;
}

void check_libraw(const int rc, const std::string &operation) {
	if (rc != LIBRAW_SUCCESS) {
		throw std::runtime_error("LibRaw " + operation + " failed: " + LibRaw::strerror(rc));
	}
}

Rgb16Image load_nef_rgb16(const std::filesystem::path &path) {
	LibRaw raw;

	check_libraw(raw.open_file(path.c_str()), "open_file");
	check_libraw(raw.unpack(), "unpack");

	raw.imgdata.params.use_camera_wb  = 1;
	raw.imgdata.params.output_color   = 1; // sRGB
	raw.imgdata.params.output_bps     = 16;
	raw.imgdata.params.no_auto_bright = 1;

	check_libraw(raw.dcraw_process(), "dcraw_process");

	int err = LIBRAW_SUCCESS;
	libraw_processed_image_t *processed = raw.dcraw_make_mem_image(&err);
	check_libraw(err, "dcraw_make_mem_image");
	if (processed == nullptr) {
		throw std::runtime_error("LibRaw dcraw_make_mem_image returned no image");
	}

	struct ProcessedImageDeleter {
		void operator()(libraw_processed_image_t *image) const noexcept {
			LibRaw::dcraw_clear_mem(image);
		}
	};
	std::unique_ptr<libraw_processed_image_t, ProcessedImageDeleter> imageGuard{processed};

	if (processed->type != LIBRAW_IMAGE_BITMAP || processed->colors != 3 || processed->bits != 16) {
		throw std::runtime_error("LibRaw produced an unsupported image format");
	}

	Rgb16Image image;
	image.width  = processed->width;
	image.height = processed->height;

	const std::size_t expectedSize =
			static_cast<std::size_t>(image.width) * static_cast<std::size_t>(image.height) * 3;
	if (processed->data_size != expectedSize * sizeof(uint16_t)) {
		throw std::runtime_error("LibRaw produced an unexpected RGB buffer size");
	}

	image.rgb.resize(expectedSize);
	std::memcpy(image.rgb.data(), processed->data, processed->data_size);

	return image;
}

bool has_extension(const std::filesystem::path &path, const std::string &extension) {
	std::string ext = path.extension().string();
	std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) {
		return static_cast<char>(std::tolower(c));
	});
	return ext == extension;
}

uint8_t clamp_u8(const double value) {
	const long rounded = std::lround(value);
	return static_cast<uint8_t>(std::clamp<long>(rounded, 0, 255));
}

uint16_t clamp_u10(const double value) {
	const long rounded = std::lround(value);
	return static_cast<uint16_t>(std::clamp<long>(rounded, 0, 1023));
}

struct YCbCr {
	uint8_t y;
	uint8_t cb;
	uint8_t cr;
};

struct YCbCr10 {
	uint16_t y;
	uint16_t cb;
	uint16_t cr;
};

YCbCr rgb_to_bt709_limited(const double r, const double g, const double b) {
	const double yFull = 0.2126 * r + 0.7152 * g + 0.0722 * b;
	const double y     = 16.0 + 219.0 * yFull / 255.0;
	const double cb    = 128.0 + 224.0 * (b - yFull) / (2.0 * (255.0 - 255.0 * 0.0722));
	const double cr    = 128.0 + 224.0 * (r - yFull) / (2.0 * (255.0 - 255.0 * 0.2126));

	return {clamp_u8(y), clamp_u8(cb), clamp_u8(cr)};
}

YCbCr10 rgb16_to_bt709_limited_10(const double r, const double g, const double b) {
	const double yFull = 0.2126 * r + 0.7152 * g + 0.0722 * b;
	const double y     = 64.0 + 876.0 * yFull / 65535.0;
	const double cb    = 512.0 + 896.0 * (b - yFull) / (2.0 * (65535.0 - 65535.0 * 0.0722));
	const double cr    = 512.0 + 896.0 * (r - yFull) / (2.0 * (65535.0 - 65535.0 * 0.2126));

	return {clamp_u10(y), clamp_u10(cb), clamp_u10(cr)};
}

void store_u16le(std::vector<uint8_t> &bytes, const std::size_t sampleIndex, const uint16_t value) {
	bytes[sampleIndex * 2 + 0] = static_cast<uint8_t>(value & 0xffu);
	bytes[sampleIndex * 2 + 1] = static_cast<uint8_t>(value >> 8u);
}

codec_gui::RawImage rgb_to_yuv420_bt709_limited(const RgbImage &rgb) {
	if (rgb.width <= 0 || rgb.height <= 0) {
		throw std::runtime_error("JPEG dimensions must be positive");
	}

	const int width  = (rgb.width + 1) & ~1;
	const int height = (rgb.height + 1) & ~1;

	codec_gui::RawImage image;
	image.width  = width;
	image.height = height;
	image.format = codec_gui::PixelFormat::YUV420P8;

	image.planes[0].strideBytes = width;
	image.planes[1].strideBytes = width / 2;
	image.planes[2].strideBytes = width / 2;
	image.planes[0].bytes.resize(static_cast<std::size_t>(width) * static_cast<std::size_t>(height));
	image.planes[1].bytes.resize(static_cast<std::size_t>(width / 2) * static_cast<std::size_t>(height / 2));
	image.planes[2].bytes.resize(static_cast<std::size_t>(width / 2) * static_cast<std::size_t>(height / 2));

	auto rgb_at = [&](const int x, const int y) -> const uint8_t * {
		const int sx = std::min(x, rgb.width - 1);
		const int sy = std::min(y, rgb.height - 1);
		return rgb.rgb.data() + (static_cast<std::size_t>(sy) * rgb.width + sx) * 3;
	};

	for (int y = 0; y < height; ++y) {
		for (int x = 0; x < width; ++x) {
			const uint8_t *p = rgb_at(x, y);
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
					const uint8_t *p = rgb_at(x * 2 + dx, y * 2 + dy);
					r += p[0];
					g += p[1];
					b += p[2];
				}
			}

			const YCbCr c = rgb_to_bt709_limited(r * 0.25, g * 0.25, b * 0.25);
			const auto  i = static_cast<std::size_t>(y) * (width / 2) + x;
			image.planes[1].bytes[i] = c.cb;
			image.planes[2].bytes[i] = c.cr;
		}
	}

	return image;
}

codec_gui::RawImage rgb16_to_yuv420p10_bt709_limited(const Rgb16Image &rgb) {
	if (rgb.width <= 0 || rgb.height <= 0) {
		throw std::runtime_error("RAW dimensions must be positive");
	}

	const int width  = (rgb.width + 1) & ~1;
	const int height = (rgb.height + 1) & ~1;

	codec_gui::RawImage image;
	image.width  = width;
	image.height = height;
	image.format = codec_gui::PixelFormat::YUV420P10LE;

	image.planes[0].strideBytes = width * 2;
	image.planes[1].strideBytes = width;
	image.planes[2].strideBytes = width;
	image.planes[0].bytes.resize(static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 2);
	image.planes[1].bytes.resize(static_cast<std::size_t>(width / 2) * static_cast<std::size_t>(height / 2) * 2);
	image.planes[2].bytes.resize(static_cast<std::size_t>(width / 2) * static_cast<std::size_t>(height / 2) * 2);

	auto rgb_at = [&](const int x, const int y) -> const uint16_t * {
		const int sx = std::min(x, rgb.width - 1);
		const int sy = std::min(y, rgb.height - 1);
		return rgb.rgb.data() + (static_cast<std::size_t>(sy) * rgb.width + sx) * 3;
	};

	for (int y = 0; y < height; ++y) {
		for (int x = 0; x < width; ++x) {
			const uint16_t *p = rgb_at(x, y);
			store_u16le(
					image.planes[0].bytes,
					static_cast<std::size_t>(y) * width + x,
					rgb16_to_bt709_limited_10(p[0], p[1], p[2]).y);
		}
	}

	for (int y = 0; y < height / 2; ++y) {
		for (int x = 0; x < width / 2; ++x) {
			double r = 0.0;
			double g = 0.0;
			double b = 0.0;

			for (int dy = 0; dy < 2; ++dy) {
				for (int dx = 0; dx < 2; ++dx) {
					const uint16_t *p = rgb_at(x * 2 + dx, y * 2 + dy);
					r += p[0];
					g += p[1];
					b += p[2];
				}
			}

			const YCbCr10 c = rgb16_to_bt709_limited_10(r * 0.25, g * 0.25, b * 0.25);
			const auto    i = static_cast<std::size_t>(y) * (width / 2) + x;
			store_u16le(image.planes[1].bytes, i, c.cb);
			store_u16le(image.planes[2].bytes, i, c.cr);
		}
	}

	return image;
}

codec_gui::RawImage load_input_image(const std::filesystem::path &path) {
	if (has_extension(path, ".nef")) {
		return rgb16_to_yuv420p10_bt709_limited(load_nef_rgb16(path));
	}

	if (has_extension(path, ".jpg") || has_extension(path, ".jpeg")) {
		return rgb_to_yuv420_bt709_limited(load_jpeg_rgb(path));
	}

	throw std::runtime_error("unsupported input format: " + path.string());
}

codec_gui::RawImage make_test_pattern() {
	codec_gui::RawImage image;

	constexpr uint32_t HEIGHT = 1080;
	constexpr uint32_t WIDTH  = 1920;

	image.width  = WIDTH;
	image.height = HEIGHT;

	image.planes[0].bytes.resize(HEIGHT * WIDTH);
	image.planes[0].strideBytes = WIDTH;
	image.planes[1].bytes.resize(HEIGHT * WIDTH / 4);
	image.planes[1].strideBytes = WIDTH / 2;
	image.planes[2].bytes.resize(HEIGHT * WIDTH / 4);
	image.planes[2].strideBytes = WIDTH / 2;

	for (uint32_t y = 0; y < HEIGHT; y++) {
		for (uint32_t x = 0; x < WIDTH; x++) {
			image.planes[0].bytes[y * WIDTH + x] =
					std::clamp((std::sin(float(x) / float(HEIGHT) * 4) / 2.0f + 0.5f) *
									   (std::sin(float(y) / float(HEIGHT) * 4) / 2.0f + 0.5f),
							   0.0f, std::bit_cast<float>(std::bit_cast<uint32_t>(1.0f) - 1)) *
					256;
		}
	}

	for (uint32_t y = 0; y < HEIGHT / 2; y++) {
		for (uint32_t x = 0; x < WIDTH / 2; x++) {
			image.planes[1].bytes[y * (WIDTH / 2) + x] =
					std::clamp(float(x) / float(WIDTH / 2), 0.0f,
							   std::bit_cast<float>(std::bit_cast<uint32_t>(1.0f) - 1)) *
					256;
			image.planes[2].bytes[y * (WIDTH / 2) + x] =
					std::clamp(float(y) / float(HEIGHT / 2), 0.0f,
							   std::bit_cast<float>(std::bit_cast<uint32_t>(1.0f) - 1)) *
					256;
		}
	}

	return image;
}

} // namespace

int main(int argc, char **argv) {
	const codec_gui::RawImage image = argc > 1
	                                      ? load_input_image(argv[1])
	                                      : make_test_pattern();

	const std::string x265Profile =
			image.format == codec_gui::PixelFormat::YUV420P10LE ? "main10" : "mainstillpicture";

	codec_gui::EncodedImage encoded = codec_gui::encode_x265_still_image(
			image, std::array{
						   codec_gui::EncoderParam{"preset", std::string{"veryslow"}},
						   codec_gui::EncoderParam{"profile", x265Profile},
						   codec_gui::EncoderParam{"colorprim", std::string{"bt709"}},
						   codec_gui::EncoderParam{"transfer", std::string{"iec61966-2-1"}},
						   codec_gui::EncoderParam{"colormatrix", std::string{"bt709"}},
						   codec_gui::EncoderParam{"range", std::string{"limited"}},
						   codec_gui::EncoderParam{"qp", int64_t{50}},
						   codec_gui::EncoderParam{"rd", int64_t{6}},
						   codec_gui::EncoderParam{"rdoq-level", int64_t{2}},
						   codec_gui::EncoderParam{"aq-mode", std::string{"3"}},
						   codec_gui::EncoderParam{"psy-rd", double{2.0}},
						   codec_gui::EncoderParam{"psy-rdoq", double{50.0}},
						   codec_gui::EncoderParam{"deblock", std::string{"0:0"}},
				   });

	dump_to_file("file.h265", encoded.hevcAnnexB);

	codec_gui::EncodedImage encoded_h266 = codec_gui::encode_vvenc_still_image(
			image, std::array{
						   codec_gui::EncoderParam{"preset", std::string{"slower"}},
						   codec_gui::EncoderParam{"qp", int64_t{60}},
				   });

	dump_to_file("file.h266", encoded_h266.hevcAnnexB);

	codec_gui::EncodedImage encoded_av1 = codec_gui::encode_svt_av1_still_image(
			image, std::array{
						   codec_gui::EncoderParam{"preset", int64_t{8}},
						   codec_gui::EncoderParam{"crf", double{35.0}},
						   codec_gui::EncoderParam{"tune", std::string{"3"}},
						   codec_gui::EncoderParam{"enable-variance-boost", true},
						   codec_gui::EncoderParam{"variance-boost-curve", std::string{"2"}},
						   codec_gui::EncoderParam{"max-tx-size", int64_t{32}},
				   });

	dump_to_file("file.av1", encoded_av1.hevcAnnexB);

	codec_gui::EncodedImage encoded_vaapi_hevc = codec_gui::encode_vaapi_hevc_still_image(
			image, std::array{
						   codec_gui::EncoderParam{"qpi", int64_t{35}},
						   codec_gui::EncoderParam{"target-usage", int64_t{4}},
				   });

	dump_to_file("file_vaapi.h265", encoded_vaapi_hevc.hevcAnnexB);

	try {
		codec_gui::EncodedImage encoded_vaapi_av1 = codec_gui::encode_vaapi_av1_still_image(
				image, std::array{
							   codec_gui::EncoderParam{"qp", int64_t{35}},
							   codec_gui::EncoderParam{"quality-level", int64_t{4}},
					   });

		dump_to_file("file_vaapi.av1", encoded_vaapi_av1.hevcAnnexB);
	} catch (const std::exception& e) {
		std::cerr << "skipping VA-API AV1: " << e.what() << '\n';
	}

	codec_gui::EncodedImage encoded_uvg266 = codec_gui::encode_uvg266_still_image(
			image, std::array{
						   codec_gui::EncoderParam{"qp", int64_t{48}},
						   codec_gui::EncoderParam{"preset", std::string{"slower"}},
						   codec_gui::EncoderParam{"rdo", int64_t{2}},
						   codec_gui::EncoderParam{"sao", true},
						   codec_gui::EncoderParam{"alf", true},
				   });

	dump_to_file("file_uvg266.h266", encoded_uvg266.hevcAnnexB);

		codec_gui::EncodedImage encoded_av2 = codec_gui::encode_av2_still_image(
				image, std::array{
							   codec_gui::EncoderParam{"cpu-used", int64_t{9}},
							   codec_gui::EncoderParam{"qp", int64_t{128}},
							   codec_gui::EncoderParam{"enable-cdef", true},
							   codec_gui::EncoderParam{"enable-restoration", true},
							   codec_gui::EncoderParam{"enable-intrabc", false},
							   codec_gui::EncoderParam{"enable-rect-partitions", false},
							   codec_gui::EncoderParam{"enable-1to4-partitions", false},
							   codec_gui::EncoderParam{"min-partition-size", int64_t{64}},
							   codec_gui::EncoderParam{"max-partition-size", int64_t{128}},
					   });

	dump_to_file("file.av2", encoded_av2.hevcAnnexB);
}
