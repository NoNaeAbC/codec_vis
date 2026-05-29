#pragma once

#include "../codec_gui_x265.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>

namespace codec_gui::gui {

[[nodiscard]] inline int plane_count(PixelFormat format) {
	switch (format) {
		case PixelFormat::Gray8:
		case PixelFormat::Gray10LE:
			return 1;
		case PixelFormat::YUV420P8:
		case PixelFormat::YUV420P10LE:
		case PixelFormat::YUV422P8:
		case PixelFormat::YUV422P10LE:
		case PixelFormat::YUV444P8:
		case PixelFormat::YUV444P10LE:
			return 3;
	}
	return 0;
}

[[nodiscard]] inline int bytes_per_sample(PixelFormat format) {
	switch (format) {
		case PixelFormat::YUV420P8:
		case PixelFormat::YUV422P8:
		case PixelFormat::YUV444P8:
		case PixelFormat::Gray8:
			return 1;
		case PixelFormat::YUV420P10LE:
		case PixelFormat::YUV422P10LE:
		case PixelFormat::YUV444P10LE:
		case PixelFormat::Gray10LE:
			return 2;
	}
	return 1;
}

[[nodiscard]] inline bool is_420(PixelFormat format) {
	return format == PixelFormat::YUV420P8 || format == PixelFormat::YUV420P10LE;
}

[[nodiscard]] inline bool is_422(PixelFormat format) {
	return format == PixelFormat::YUV422P8 || format == PixelFormat::YUV422P10LE;
}

[[nodiscard]] inline bool is_444(PixelFormat format) {
	return format == PixelFormat::YUV444P8 || format == PixelFormat::YUV444P10LE;
}

[[nodiscard]] inline bool is_gray(PixelFormat format) {
	return format == PixelFormat::Gray8 || format == PixelFormat::Gray10LE;
}

[[nodiscard]] inline bool is_10_bit(PixelFormat format) {
	switch (format) {
		case PixelFormat::YUV420P10LE:
		case PixelFormat::YUV422P10LE:
		case PixelFormat::YUV444P10LE:
		case PixelFormat::Gray10LE:
			return true;
		case PixelFormat::YUV420P8:
		case PixelFormat::YUV422P8:
		case PixelFormat::YUV444P8:
		case PixelFormat::Gray8:
			return false;
	}
	return false;
}

[[nodiscard]] inline int bit_depth(PixelFormat format) {
	return is_10_bit(format) ? 10 : 8;
}

[[nodiscard]] inline PixelFormat pixel_format_for(int bitDepth, bool yuv444) {
	if (bitDepth == 10) {
		return yuv444 ? PixelFormat::YUV444P10LE : PixelFormat::YUV420P10LE;
	}
	return yuv444 ? PixelFormat::YUV444P8 : PixelFormat::YUV420P8;
}

[[nodiscard]] inline double max_sample_value(PixelFormat format) {
	return bytes_per_sample(format) == 1 ? 255.0 : 1023.0;
}

[[nodiscard]] inline int plane_width(const RawImage& image, int plane) {
	if (plane == 0 || is_444(image.format)) {
		return image.width;
	}
	return (image.width + 1) / 2;
}

[[nodiscard]] inline int plane_height(const RawImage& image, int plane) {
	if (plane == 0 || is_444(image.format) || is_422(image.format)) {
		return image.height;
	}
	return (image.height + 1) / 2;
}

[[nodiscard]] inline bool plane_available(const RawImage& image, int plane) {
	const int height = plane_height(image, plane);
	return plane >= 0 && plane < 3 &&
	       image.planes[plane].strideBytes > 0 &&
	       image.planes[plane].bytes.size() >=
	           static_cast<std::size_t>(image.planes[plane].strideBytes) * static_cast<std::size_t>(height);
}

[[nodiscard]] inline uint32_t sample_at(const ImagePlane& plane, int x, int y, int sampleBytes) {
	const auto byteIndex =
		static_cast<std::size_t>(y) * static_cast<std::size_t>(plane.strideBytes) +
		static_cast<std::size_t>(x) * static_cast<std::size_t>(sampleBytes);
	if (sampleBytes == 1) {
		return byteIndex < plane.bytes.size() ? plane.bytes[byteIndex] : 0;
	}
	if (byteIndex + 1 >= plane.bytes.size()) {
		return 0;
	}
	return static_cast<uint32_t>(plane.bytes[byteIndex]) |
	       (static_cast<uint32_t>(plane.bytes[byteIndex + 1]) << 8u);
}

inline void put_sample(ImagePlane& plane, int x, int y, int sampleBytes, uint32_t value) {
	const auto byteIndex =
		static_cast<std::size_t>(y) * static_cast<std::size_t>(plane.strideBytes) +
		static_cast<std::size_t>(x) * static_cast<std::size_t>(sampleBytes);
	if (sampleBytes == 1) {
		if (byteIndex < plane.bytes.size()) {
			plane.bytes[byteIndex] = static_cast<uint8_t>(std::min<uint32_t>(255, value));
		}
		return;
	}
	if (byteIndex + 1 >= plane.bytes.size()) {
		return;
	}
	value = std::min<uint32_t>(1023, value);
	plane.bytes[byteIndex] = static_cast<uint8_t>(value & 0xffu);
	plane.bytes[byteIndex + 1] = static_cast<uint8_t>(value >> 8u);
}

} // namespace codec_gui::gui
