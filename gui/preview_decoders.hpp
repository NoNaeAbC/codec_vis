#pragma once

#include "encoder_backends.hpp"

namespace codec_gui::gui {

[[nodiscard]] DecodeResult decode_embedded_preview(const EncodedImage& encoded);
[[nodiscard]] DecodeResult decode_hevc_preview(const EncodedImage& encoded);
[[nodiscard]] DecodeResult decode_av1_preview(const EncodedImage& encoded);
[[nodiscard]] DecodeResult decode_av2_preview(const EncodedImage& encoded);
[[nodiscard]] DecodeResult decode_vvc_preview(const EncodedImage& encoded);
[[nodiscard]] DecodeResult decode_jpegls_preview(const EncodedImage& encoded);
[[nodiscard]] DecodeResult decode_jpeg_preview(const EncodedImage& encoded);
[[nodiscard]] DecodeResult decode_jpeg2000_preview(const EncodedImage& encoded);
[[nodiscard]] DecodeResult decode_jpegxl_preview(const EncodedImage& encoded);
[[nodiscard]] DecodeResult decode_jpegxr_preview(const EncodedImage& encoded);
[[nodiscard]] DecodeResult decode_png_preview(const EncodedImage& encoded);
[[nodiscard]] DecodeResult decode_h264_preview(const EncodedImage& encoded);

} // namespace codec_gui::gui
