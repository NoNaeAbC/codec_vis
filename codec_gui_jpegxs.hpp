#pragma once

#include "codec_gui_x265.hpp"

namespace codec_gui {
std::vector<EncoderParamInfo> query_jpegxs_parameters();
EncodedImage encode_jpegxs_still_image(const RawImage& image, std::span<const EncoderParam> params);
std::shared_ptr<const RawImage> decode_jpegxs_image(const EncodedImage& encoded, unsigned threads=1, double* codecSeconds=nullptr);
} // namespace codec_gui
