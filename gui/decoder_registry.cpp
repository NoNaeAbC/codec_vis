#include "decoder_registry.hpp"
#include "../codec_gui_image_io.hpp"
#include "../third_party/native_video_decode/vaapi_render_node.hpp"
#include "../third_party/native_video_decode/vulkan_decode_devices.hpp"
#include "preview_decoders.hpp"
#include "scc_decode.hpp"
#include "raw_image_utils.hpp"
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/hwcontext.h>
}
#include <cstring>
namespace codec_gui::gui {
ImageDecodeInput image_decode_input(const ImageObject &image) {
	ImageDecodeInput result;
	result.reference = image.decoded;
	result.width = image.width;
	result.height = image.height;
	result.format = image.pixelFormat;
	if (image.encoded) {
		auto bytes = std::make_shared<EncodedImage>();
		bytes->hevcAnnexB = image.encoded->bytes;
		bytes->codedColor = image.encoded->codedColor;
		result.bytes = bytes;
		result.codec = image.encoded->codecName;
		result.format = image.encoded->codedPixelFormat;
	} else if (image.decoded) {
		result.bytes = image.sourceCompressed ? image.sourceCompressed : image.decoded->compressedSource;
		result.codec = image.sourceCodec.empty() ? image.decoded->compressedCodec : image.sourceCodec;
		result.imported = true;
	}
	return result;
}
static std::string av_name(const std::string &codec) {
	if (codec == "HEVC")
		return "hevc";
	if (codec == "H.264")
		return "h264";
	if (codec == "VVC")
		return "vvc";
	if (codec == "AV1")
		return "av1";
	if (codec == "VP8")
		return "vp8";
	if (codec == "VP9")
		return "vp9";
	return {};
}
std::vector<ImageDecoder> image_decoder_candidates(const ImageDecodeInput &input) {
	std::vector<ImageDecoder> out;
	if (!input.bytes || input.bytes->hevcAnnexB.empty())
		return out;
	const auto codec = av_name(input.codec);
    const bool scc=input.codec=="HEVC" && hevc_is_scc(*input.bytes);
	auto ff = [&](std::string name, std::string hardware, std::string device, std::string label) {
		const AVCodec *decoder = avcodec_find_decoder_by_name(name.c_str());
		if (!decoder)
			return;
		if (!hardware.empty()) {
			auto type = hardware == "vulkan" ? AV_HWDEVICE_TYPE_VULKAN : AV_HWDEVICE_TYPE_VAAPI;
			bool supported = false;
			for (int i = 0; const auto *config = avcodec_get_hw_config(decoder, i); ++i)
				if (config->device_type == type && (config->methods & AV_CODEC_HW_CONFIG_METHOD_HW_DEVICE_CTX))
					supported = true;
			if (!supported)
				return;
		}
		const auto id = "ff:" + name + ":" + hardware + ":" + device;
		out.push_back({{id, std::move(label), !hardware.empty()}, [name, hardware, device](const EncodedImage &b) {
						   return decode_libavcodec(b, name, hardware, device);
					   }});
	};
	if (!codec.empty() && !scc) {
		VkVideoCodecOperationFlagBitsKHR op = static_cast<VkVideoCodecOperationFlagBitsKHR>(0);
		if (codec == "hevc")
			op = VK_VIDEO_CODEC_OPERATION_DECODE_H265_BIT_KHR;
		if (codec == "h264")
			op = VK_VIDEO_CODEC_OPERATION_DECODE_H264_BIT_KHR;
		if (codec == "av1")
			op = VK_VIDEO_CODEC_OPERATION_DECODE_AV1_BIT_KHR;
		if (codec == "vp9")
			op = VK_VIDEO_CODEC_OPERATION_DECODE_VP9_BIT_KHR;
		if (op)
			for (const auto &device : vulkan_decode_devices())
				if (device.operations & op)
					ff(codec, "vulkan", device.index, "Vulkan Video / " + device.name);
		auto render = native_video::av1::vaapi::find_render_node();
		if (render)
			ff(codec, "vaapi", render->string(), "VA-API / " + render->filename().string());
	}
    if(scc) for(const auto& device:vulkan_decode_devices())
        if(device.operations&VK_VIDEO_CODEC_OPERATION_DECODE_H265_BIT_KHR)
            out.push_back({{"native:hevc:scc:vulkan:"+device.index,"Vulkan Video SCC / "+device.name,true},
                [index=device.index](const EncodedImage& b){return decode_scc_vulkan(b,index);}});
	auto native = [&](const char *id, const char *label, DecodeResult (*decode)(const EncodedImage &)) {
		out.push_back({{id, label, false}, decode});
	};
	if (input.codec == "HEVC") {
		if (!scc)
			native("de265", "libde265 (1 thread)", decode_hevc_preview);
	}
	if (input.codec == "VVC")
		native("vvdec", "VVdeC", decode_vvc_preview);
	if (input.codec == "AV1")
		native("dav1d", "dav1d", decode_av1_preview);
	if (input.codec == "AV2")
		native("avm", "AVM reference (1 thread)", decode_av2_preview);
	if (input.codec == "VP8")
		native("vpx8", "libvpx VP8 (1 thread)", decode_vp8_preview);
	if (input.codec == "VP9")
		native("vpx9", "libvpx VP9 (1 thread)", decode_vp9_preview);
	struct Still {
		const char *codec;
		const char *label;
		const char *extension;
		DecodeResult (*decode)(const EncodedImage &);
	};
	const Still stills[] = {{"HEIF", "libheif", ".heif", nullptr},
                            {"PNG", "libpng", ".png", decode_png_preview},
							{"JPEG", "libjpeg", ".jpg", decode_jpeg_preview},
							{"JPEG XL", "libjxl", ".jxl", decode_jpegxl_preview},
							{"JPEG-LS", "CharLS", ".jls", decode_jpegls_preview},
							{"JPEG 2000", "OpenJPEG", ".jp2", decode_jpeg2000_preview},
							{"JPEG XR", "jxrlib", ".jxr", decode_jpegxr_preview},
							{"JPEG XS", "SVT-JPEG-XS", ".jxs", decode_jpegxs_preview},
							{"WebP", "libwebp", ".webp", decode_webp_preview}};
	for (const auto &s : stills)
		if (input.codec == s.codec) {
			if (input.imported)
				out.push_back(
					{{s.label, s.label, false},
					 [extension = std::string(s.extension)](const EncodedImage &b) -> DecodeResult {
						 try {
							 std::optional<double> codecSeconds;
							 auto image = decode_input_image(
								 {reinterpret_cast<const unsigned char *>(b.hevcAnnexB.data()), b.hevcAnnexB.size()},
								 extension, &codecSeconds);
							 image.compressedSource.reset();
							 return {.image = std::make_shared<RawImage>(std::move(image)), .error = {}, .codecSeconds = codecSeconds};
						 } catch (const std::exception &e) {
							 return {.image = nullptr, .error = e.what()};
						 }
					 }});
            else if(s.decode)
				native(s.label, s.label, s.decode);
		}
	if (!codec.empty() && !scc) {
		ff(codec, "", "", "libavcodec / " + codec);
		if (codec == "av1") {
			ff("libaom-av1", "", "", "libaom AV1");
			ff("libdav1d", "", "", "libavcodec / dav1d");
		}
		if (codec == "vp8" || codec == "vp9")
			ff("libvpx-" + codec, "", "", "libavcodec / libvpx " + codec);
	}
	return out;
}
bool decoder_output_matches(const ImageDecodeInput &input, const DecodeResult &output) {
	if (!output.image || !output.error.empty())
		return false;
	const auto &image = *output.image;
	if (input.width > 0 && (image.width != input.width || image.height != input.height))
		return false;
	if (input.width <= 0)
		return true;
	if (bit_depth(input.format) != bit_depth(image.format))
		return false;
	if (is_420(input.format) != is_420(image.format) || is_422(input.format) != is_422(image.format) ||
		is_gray(input.format) != is_gray(image.format) || is_rgb(input.format) != is_rgb(image.format))
		return false;
	// Reject successful submissions that produce incorrect visible samples.
	// Some drivers advertise profiles whose actual output is corrupt. Permit
	// one code value of decoder rounding, never depth/chroma conversion.
	if (input.reference && !av_name(input.codec).empty()) {
		const auto &ref = *input.reference;
		if (ref.format != image.format)
			return false;
		const int bps = bytes_per_sample(image.format);
        const int tolerance=input.codec=="VVC" ? 0 : 1;
		for (int p = 0; p < plane_count(image.format); ++p)
			for (int y = 0; y < plane_height(image, p); ++y)
				for (int x = 0; x < plane_width(image, p); ++x)
					if (std::abs(int(sample_at(ref.planes[p], x, y, bps)) -
								 int(sample_at(image.planes[p], x, y, bps))) > tolerance)
						return false;
	}
	return true;
}
} // namespace codec_gui::gui
