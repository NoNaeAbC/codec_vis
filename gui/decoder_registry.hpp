#pragma once
#include "app_state.hpp"
#include "encoder_backends.hpp"
#include <functional>
namespace codec_gui::gui {
struct ImageDecodeInput {
	std::shared_ptr<const EncodedImage> bytes;
	std::shared_ptr<const RawImage> reference;
	std::string codec;
	PixelFormat format;
	int width = 0, height = 0;
	bool imported = false;
};
struct ImageDecoder {
	DecoderInfo info;
	std::function<DecodeResult(const EncodedImage &)> decode;
};
ImageDecodeInput image_decode_input(const ImageObject &);
std::vector<ImageDecoder> image_decoder_candidates(const ImageDecodeInput &);
bool decoder_output_matches(const ImageDecodeInput &, const DecodeResult &);
} // namespace codec_gui::gui
