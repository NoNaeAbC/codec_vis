#pragma once
#include "codec_gui_x265.hpp"

namespace codec_gui {
std::vector<EncoderParamInfo> query_kvazaar_parameters();
std::vector<EncoderParamInfo> query_webp_parameters();
std::vector<EncoderParamInfo> query_vpx_vp8_parameters();
std::vector<EncoderParamInfo> query_vpx_vp9_parameters();
std::vector<EncoderParamInfo> query_svt_vp9_parameters();
std::vector<EncoderParamInfo> query_rav1e_parameters();
EncodedImage encode_kvazaar_still_image(const RawImage&, std::span<const EncoderParam>);
EncodedImage encode_webp_still_image(const RawImage&, std::span<const EncoderParam>);
EncodedImage encode_vpx_vp8_still_image(const RawImage&, std::span<const EncoderParam>);
EncodedImage encode_vpx_vp9_still_image(const RawImage&, std::span<const EncoderParam>);
EncodedImage encode_svt_vp9_still_image(const RawImage&, std::span<const EncoderParam>);
EncodedImage encode_rav1e_still_image(const RawImage&, std::span<const EncoderParam>);
std::shared_ptr<const RawImage> decode_webp_image(const EncodedImage&, double* codecSeconds=nullptr);
std::shared_ptr<const RawImage> decode_vpx_image(const EncodedImage&, bool vp9, double* codecSeconds=nullptr);
// Standard one-frame IVF wrapper, shared by the VP8/VP9/AV1 adapters.
EncodedImage wrap_still_ivf(std::span<const uint8_t>, const RawImage&, const char (&fourcc)[5]);
} // namespace codec_gui
