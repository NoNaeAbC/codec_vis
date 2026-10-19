#pragma once
#include "encoder_backends.hpp"

namespace codec_gui::gui {
bool hevc_is_scc(const EncodedImage& encoded);
DecodeResult decode_scc_vulkan(const EncodedImage& encoded, const std::string& deviceIndex={});
}
