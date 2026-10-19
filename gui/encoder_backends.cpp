#include "encoder_backends.hpp"
#include "../codec_gui_x265.hpp"
#include "../codec_gui_jpegxs.hpp"
#include "../codec_gui_extra.hpp"
#include "app_state.hpp"
#include "preview_decoders.hpp"
#include "scc_decode.hpp"
#include "raw_image_conversion.hpp"
#include "raw_image_utils.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <exception>
#include <filesystem>
#include <memory>
#include <span>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

namespace codec_gui::gui {
namespace {

constexpr BackendId X265_HEVC{1};
constexpr BackendId VVENC_VVC{2};
constexpr BackendId SVT_AV1{3};
constexpr BackendId VAAPI_HEVC{4};
constexpr BackendId VAAPI_AV1{5};
constexpr BackendId UVG266_VVC{6};
constexpr BackendId AV2{7};
constexpr BackendId JPEGLS{8};
constexpr BackendId JPEG{9};
constexpr BackendId JPEG2000{10};
constexpr BackendId JPEGXL{11};
constexpr BackendId JPEGXR{12};
constexpr BackendId PNG{13};
constexpr BackendId X264_H264_INTRA{14};
constexpr BackendId JPEGXS{15};
constexpr BackendId KVAZAAR{16};
constexpr BackendId WEBP{17};
constexpr BackendId VPX_VP8{18};
constexpr BackendId VPX_VP9{19};
constexpr BackendId SVT_VP9{20};
constexpr BackendId RAV1E{21};

RawImage pad_subsampled_input(RawImage image) {
	const int paddedWidth = (is_420(image.format) || is_422(image.format)) ? (image.width + 1) & ~1 : image.width;
	const int paddedHeight = is_420(image.format) ? (image.height + 1) & ~1 : image.height;
	if (paddedWidth == image.width && paddedHeight == image.height) return image;
	RawImage padded;
	padded.width = paddedWidth;
	padded.height = paddedHeight;
	padded.format = image.format;
	padded.color = image.color;
	padded.nominalPeakNits = image.nominalPeakNits;
	const int sampleBytes = bytes_per_sample(image.format);
	for (int plane = 0; plane < plane_count(image.format); ++plane) {
		const int sourceWidth = plane_width(image, plane);
		const int sourceHeight = plane_height(image, plane);
		const int targetWidth = plane_width(padded, plane);
		const int targetHeight = plane_height(padded, plane);
		padded.planes[plane].strideBytes = targetWidth * sampleBytes;
		padded.planes[plane].bytes.resize(static_cast<std::size_t>(padded.planes[plane].strideBytes) * targetHeight);
		for (int y = 0; y < targetHeight; ++y) {
			for (int x = 0; x < targetWidth; ++x) {
				const auto* source = image.planes[plane].bytes.data() +
					static_cast<std::size_t>(std::min(y, sourceHeight - 1)) * image.planes[plane].strideBytes +
					static_cast<std::size_t>(std::min(x, sourceWidth - 1)) * sampleBytes;
				auto* target = padded.planes[plane].bytes.data() +
					static_cast<std::size_t>(y) * padded.planes[plane].strideBytes +
					static_cast<std::size_t>(x) * sampleBytes;
				std::memcpy(target, source, static_cast<std::size_t>(sampleBytes));
			}
		}
	}
	return padded;
}

struct OutputControlPolicy {
	std::vector<EnumValue> bitDepths;
	std::string defaultBitDepth;
	std::vector<EnumValue> chromaFormats;
	std::string defaultChroma;
	bool colorPrimaries = false;
	bool transfer = false;
	bool matrix = false;
	bool range = false;
	bool toneMapping = false;
};

void append_output_controls(std::vector<EncoderParamInfo>& params, const OutputControlPolicy& policy);
PixelFormat output_format(PixelFormat source, int depth, std::string_view chroma);

std::string param_value_to_string(const ParamValue& value) {
	return std::visit(
		[](const auto& v) -> std::string {
			using T = std::decay_t<decltype(v)>;
			if constexpr (std::is_same_v<T, bool>) {
				return v ? "on" : "off";
			} else if constexpr (std::is_same_v<T, int64_t>) {
				return std::to_string(v);
			} else if constexpr (std::is_same_v<T, double>) {
				std::ostringstream oss;
				oss.precision(8);
				oss << v;
				return oss.str();
			} else {
				return v;
			}
		},
		value
	);
}

std::vector<ParamSummary> summarize_generic(std::span<const EncoderParam> params) {
	std::vector<ParamSummary> out;
	out.reserve(params.size());
	for (const EncoderParam& param : params) {
		out.push_back({.name=param.name, .value=param_value_to_string(param.value)});
	}
	return out;
}

std::vector<std::string> capability_details(const std::vector<EncoderParamInfo>& params) {
	std::vector<std::string> out;
	for (const EncoderParamInfo& param : params) {
		if (!param.relevantForStillImage) {
			continue;
		}
		std::string line = param.group.empty() ? "Parameters" : param.group;
		line += ": " + param.name;
		if (param.kind == ParamKind::Enum && !param.enumValues.empty()) {
			line += " =";
			for (const EnumValue& value : param.enumValues) {
				line += " " + value.value;
			}
		} else if (param.kind == ParamKind::Int && param.intRange) {
			line += " [" + std::to_string(param.intRange->min) + ", " + std::to_string(param.intRange->max) + "]";
		} else if (param.kind == ParamKind::Float && param.floatRange) {
			std::ostringstream range;
			range << " [" << param.floatRange->min << ", " << param.floatRange->max << "]";
			line += range.str();
		}
		if (!param.help.empty()) {
			line += " - " + param.help;
		}
		out.push_back(std::move(line));
		if (out.size() >= 32) {
			break;
		}
	}
	return out;
}

template <auto QueryFn>
CapabilityResult query_software_capabilities(std::string implementation, const OutputControlPolicy& policy) {
	CapabilityResult result;
	result.snapshot.implementation = std::move(implementation);
	result.snapshot.available = true;
	result.params = QueryFn();
	append_output_controls(result.params, policy);
	result.snapshot.details = capability_details(result.params);
	return result;
}

CapabilityResult query_x265_backend() {
	return query_software_capabilities<query_x265_parameters>(
		"x265",
		{.bitDepths={{.value="8", .label="8-bit"}, {.value="10", .label="10-bit"}, {.value="12", .label="12-bit"}}, .defaultBitDepth="12",
		 .chromaFormats={{.value="source", .label="Source"}, {.value="420", .label="4:2:0"}, {.value="422", .label="4:2:2"}, {.value="444", .label="4:4:4"}, {.value="400", .label="Monochrome"}}, .defaultChroma="source",
		 .colorPrimaries=true, .transfer=true, .matrix=true, .range=true, .toneMapping=true}
	);
}

CapabilityResult query_vvenc_backend() {
	return query_software_capabilities<query_vvenc_parameters>(
		"VVenC",
		{.bitDepths={{.value="8", .label="8-bit"}, {.value="10", .label="10-bit"}}, .defaultBitDepth="10",
		 .chromaFormats={{.value="420", .label="4:2:0"}, {.value="400", .label="Monochrome"}}, .defaultChroma="420"}
	);
}

CapabilityResult query_svt_av1_backend() {
	return query_software_capabilities<query_svt_av1_parameters>(
		"SVT-AV1",
		{.bitDepths={{.value="8", .label="8-bit"}, {.value="10", .label="10-bit"}}, .defaultBitDepth="10",
		 .chromaFormats={{.value="420", .label="4:2:0"}}, .defaultChroma="420",
		 .colorPrimaries=true, .transfer=true, .matrix=true, .range=true, .toneMapping=true}
	);
}

CapabilityResult query_uvg266_backend() {
	return query_software_capabilities<query_uvg266_parameters>(
		"uvg266",
		{.bitDepths={{.value="8", .label="8-bit"}}, .defaultBitDepth="8",
		 .chromaFormats={{.value="420", .label="4:2:0"}, {.value="400", .label="Monochrome"}}, .defaultChroma="420",
		 .colorPrimaries=true, .transfer=true, .matrix=true, .range=true, .toneMapping=true}
	);
}

CapabilityResult query_av2_backend() {
	if (const std::string error = av2_unavailable_reason(); !error.empty()) {
		CapabilityResult result;
		result.snapshot.implementation = "AVM AV2";
		result.snapshot.available = false;
		result.snapshot.error = error;
		return result;
	}
	return query_software_capabilities<query_av2_parameters>(
		"AVM AV2",
		{.bitDepths={{.value="8", .label="8-bit"}, {.value="10", .label="10-bit"}}, .defaultBitDepth="10",
		 .chromaFormats={{.value="source", .label="Source"}, {.value="420", .label="4:2:0"}, {.value="422", .label="4:2:2"}, {.value="444", .label="4:4:4"}, {.value="400", .label="Monochrome"}}, .defaultChroma="source",
		 .colorPrimaries=true, .transfer=true, .matrix=true, .range=true, .toneMapping=true}
	);
}

CapabilityResult query_jpegls_backend() {
	return query_software_capabilities<query_jpegls_parameters>(
		"CharLS JPEG-LS",
		{.bitDepths={{.value="source", .label="Source"}, {.value="8", .label="8-bit"}, {.value="10", .label="10-bit"}, {.value="12", .label="12-bit"}, {.value="14", .label="14-bit"}, {.value="16", .label="16-bit"}}, .defaultBitDepth="source"}
	);
}

CapabilityResult query_jpeg_backend() {
	return query_software_capabilities<query_jpeg_parameters>(
		"libjpeg", {.bitDepths={{.value="8", .label="8-bit"}, {.value="12", .label="12-bit"}}, .defaultBitDepth="12"}
	);
}

CapabilityResult query_jpeg2000_backend() {
	return query_software_capabilities<query_jpeg2000_parameters>(
		"OpenJPEG",
		{.bitDepths={{.value="source", .label="Source"}, {.value="8", .label="8-bit"}, {.value="10", .label="10-bit"}, {.value="12", .label="12-bit"}, {.value="14", .label="14-bit"}, {.value="16", .label="16-bit"}}, .defaultBitDepth="source"}
	);
}

CapabilityResult query_jpegxl_backend() {
	return query_software_capabilities<query_jpegxl_parameters>(
		"libjxl",
		{.bitDepths={{.value="source", .label="Source"}, {.value="8", .label="8-bit"}, {.value="10", .label="10-bit"}, {.value="12", .label="12-bit"}, {.value="14", .label="14-bit"}, {.value="16", .label="16-bit"}}, .defaultBitDepth="source",
		 .chromaFormats={}, .defaultChroma="", .colorPrimaries=true, .transfer=true, .matrix=false, .range=false, .toneMapping=true}
	);
}

CapabilityResult query_jpegxr_backend() {
	return query_software_capabilities<query_jpegxr_parameters>(
		"jxrlib", {.bitDepths={{.value="8", .label="8-bit"}, {.value="16", .label="16-bit"}}, .defaultBitDepth="16"}
	);
}

CapabilityResult query_png_backend() {
	return query_software_capabilities<query_png_parameters>(
		"libpng", {.bitDepths={{.value="8", .label="8-bit"}, {.value="16", .label="16-bit"}}, .defaultBitDepth="16"}
	);
}

CapabilityResult query_x264_backend() {
	return query_software_capabilities<query_x264_parameters>(
		"x264",
		{.bitDepths={{.value="8", .label="8-bit"}, {.value="10", .label="10-bit"}}, .defaultBitDepth="8",
		 .chromaFormats={{.value="420", .label="4:2:0"}, {.value="422", .label="4:2:2"}, {.value="444", .label="4:4:4"}}, .defaultChroma="420",
		 .colorPrimaries=true, .transfer=true, .matrix=true, .range=true, .toneMapping=true}
	);
}

CapabilityResult query_kvazaar_backend() {
	auto result=query_software_capabilities<query_kvazaar_parameters>("Kvazaar",
		{.bitDepths={{.value="8",.label="8-bit"}},.defaultBitDepth="8",
		 .chromaFormats={{.value="420",.label="4:2:0"},{.value="400",.label="Monochrome"}},.defaultChroma="420",
		 .colorPrimaries=true,.transfer=true,.matrix=true,.range=true,.toneMapping=true});
	result.snapshot.details.push_back("Installed 8-bit Kvazaar: 4:2:0 or monochrome, dimensions divisible by 8. Unsupported sizes are rejected before calling the library.");
	return result;
}
CapabilityResult query_webp_backend() {
	auto result=query_software_capabilities<query_webp_parameters>("libwebp",
		{.bitDepths={{.value="8",.label="8-bit"}},.defaultBitDepth="8",.chromaFormats={},.defaultChroma="",
		 .colorPrimaries=true,.transfer=true,.toneMapping=true});
	for(auto& p:result.params) {
		if(p.name=="color-primaries") p.enumValues={{.value="source",.label="Source"},{.value="bt709",.label="BT.709"}};
		if(p.name=="transfer") p.enumValues={{.value="source",.label="Source"},{.value="srgb",.label="sRGB"}};
	}
	result.snapshot.details.push_back("8-bit sRGB RGB input. Lossless preserves RGB; lossy WebP uses internal 4:2:0. HDR/wide-gamut sources require explicit BT.709 / sRGB target and tone mapping.");
	return result;
}
CapabilityResult query_vpx_vp8_backend() {
	auto result=query_software_capabilities<query_vpx_vp8_parameters>("libvpx VP8",
		{.bitDepths={{.value="8",.label="8-bit"}},.defaultBitDepth="8",
		 .chromaFormats={{.value="420",.label="4:2:0"}},.defaultChroma="420",
		 .colorPrimaries=true,.transfer=true,.matrix=true,.range=true,.toneMapping=true});
	result.snapshot.details.push_back("VP8 IVF. Complete color description is retained in the application; VP8 cannot signal CICP.");
	return result;
}
CapabilityResult query_vpx_vp9_backend() {
	auto result=query_software_capabilities<query_vpx_vp9_parameters>("libvpx VP9",
		{.bitDepths={{.value="8",.label="8-bit"},{.value="10",.label="10-bit"},{.value="12",.label="12-bit"}},.defaultBitDepth="10",
		 .chromaFormats={{.value="420",.label="4:2:0"},{.value="422",.label="4:2:2"},{.value="444",.label="4:4:4"}},.defaultChroma="420",
		 .colorPrimaries=true,.transfer=true,.matrix=true,.range=true,.toneMapping=true});
	result.snapshot.details.push_back("VP9 IVF. Full primaries/transfer/chroma-position context is retained in the application because VP9 has no complete CICP description.");
	return result;
}
CapabilityResult query_svt_vp9_backend() {
	auto result=query_software_capabilities<query_svt_vp9_parameters>("SVT-VP9",
		{.bitDepths={{.value="8",.label="8-bit"}},.defaultBitDepth="8",
		 .chromaFormats={{.value="420",.label="4:2:0"}},.defaultChroma="420",
		 .colorPrimaries=true,.transfer=true,.matrix=true,.toneMapping=true});
	result.params.push_back({.name="range",.label="Signal range",.group="Color Transform",.kind=ParamKind::Enum,
		.defaultValue=std::string("limited"),.enumValues={{.value="limited",.label="Limited"}},.help="SVT-VP9 only writes limited-range VP9 streams."});
	result.snapshot.details.push_back("8-bit 4:2:0 limited range; width 64..3840, height 64..2160, both divisible by 8. Larger inputs are rejected because this SVT-VP9 build corrupts memory on large stills; use libvpx VP9 instead. Color context is retained in the application.");
	return result;
}
CapabilityResult query_rav1e_backend() {
	return query_software_capabilities<query_rav1e_parameters>("librav1e",
		{.bitDepths={{.value="8",.label="8-bit"},{.value="10",.label="10-bit"},{.value="12",.label="12-bit"}},.defaultBitDepth="10",
		 .chromaFormats={{.value="420",.label="4:2:0"},{.value="422",.label="4:2:2"},{.value="444",.label="4:4:4"},{.value="400",.label="Monochrome"}},.defaultChroma="420",
		 .colorPrimaries=true,.transfer=true,.matrix=true,.range=true,.toneMapping=true});
}

CapabilityResult query_jpegxs_backend() {
    auto result = query_software_capabilities<query_jpegxs_parameters>(
        "SVT-JPEG-XS",
        {.bitDepths={{.value="8",.label="8-bit"},{.value="10",.label="10-bit"},{.value="12",.label="12-bit"}},.defaultBitDepth="10",
         .chromaFormats={{.value="420",.label="4:2:0"},{.value="422",.label="4:2:2"},{.value="444",.label="4:4:4"}},.defaultChroma="422",
         .colorPrimaries=true,.transfer=true,.matrix=true,.range=true,.toneMapping=true}
    );
    result.snapshot.details.push_back("Raw .jxs codestream; color context is retained in the app, not embedded as CICP in the file.");
    return result;
}

std::string vaapi_backend_name(std::string_view codec, const VaapiDeviceInfo& device) {
	const std::string node = std::filesystem::path(device.path).filename().string();
	return "VA-API " + std::string(codec) + " - " +
	       (device.vendor.empty() ? std::string{"unknown driver"} : device.vendor) +
	       (node.empty() ? std::string{} : " [" + node + "]");
}

CapabilityResult query_vaapi_backend(const VaapiDeviceInfo& device, bool av1) {
	CapabilityResult result;
	result.snapshot.implementation = vaapi_backend_name(av1 ? "AV1" : "HEVC", device);
	try {
		result.params = av1 ? query_vaapi_av1_parameters(device.path) : query_vaapi_hevc_parameters(device.path);
		append_output_controls(
			result.params,
			{.bitDepths={}, .defaultBitDepth="", .chromaFormats={}, .defaultChroma="", .colorPrimaries=true, .transfer=true, .matrix=true, .range=true, .toneMapping=true}
		);
		result.snapshot.available = true;
		result.snapshot.details = capability_details(result.params);
		result.snapshot.details.insert(result.snapshot.details.begin(), "Render node: " + device.path);
	} catch (const std::exception& e) {
		result.snapshot.available = false;
		result.snapshot.error = e.what();
	}
	return result;
}

std::string vaapi_discovery_error(std::span<const VaapiDeviceInfo> devices, std::string_view codec) {
	if (devices.empty()) return "No DRM render nodes were found under /dev/dri; VA-API " + std::string(codec) + " is unavailable.";
	std::string error = "No usable VA-API " + std::string(codec) + " encoder was found.";
	for (const VaapiDeviceInfo& device : devices) {
		if (!device.error.empty()) {
			error += " " + device.path + ": " + device.error + ".";
			continue;
		}
		const bool supported = codec == "AV1" ? device.supportsAv1Encode : device.supportsHevcEncode;
		if (!supported) {
			error += " " + device.path + " (" + (device.vendor.empty() ? std::string{"unknown driver"} : device.vendor) +
			         ") does not advertise an " + std::string(codec) + " EncSlice entrypoint.";
		}
	}
	return error;
}

EncoderBackend unavailable_vaapi_backend(BackendId id, const std::string& codec, const std::string& error) {
	const std::string name = "VA-API " + codec;
	return {
		.id=id,
		.name=name,
		.codec=codec,
		.kind=BackendKind::Hardware,
		.queryCapabilities=[name, error] {
			CapabilityResult result;
			result.snapshot.implementation = name;
			result.snapshot.available = false;
			result.snapshot.error = error;
			return result;
		},
		.encode=[error](const RawImage&, std::span<const EncoderParam>) -> EncodedImage { throw std::runtime_error(error); },
		.decodePreview=codec == "AV1" ? decode_av1_preview : decode_hevc_preview,
		.summarizeParams=summarize_generic,
	};
}

EncodedMetadata metadata_for_backend(const EncoderBackend& backend, const EncodedImage& encoded, std::span<const EncoderParam> params) {
	EncodedMetadata metadata;
	metadata.backend = backend.id;
	metadata.codecName = backend.codec;
	metadata.backendName = backend.name;
	metadata.bytes = encoded.hevcAnnexB;
	metadata.byteSize = metadata.bytes.size();
	metadata.keyParams = backend.summarizeParams(params);
	return metadata;
}

} // namespace

std::vector<EncoderBackend> initial_encoder_backends(std::span<const VaapiDeviceInfo> vaapiDevices) {
	std::vector<EncoderBackend> backends{
		{.id=X265_HEVC, .name="x265 HEVC", .codec="HEVC", .kind=BackendKind::Software, .queryCapabilities=query_x265_backend, .encode=encode_x265_still_image, .decodePreview=decode_hevc_preview, .summarizeParams=summarize_generic, .inputGeometry=InputGeometryPolicy::PadToChromaAlignment},
		{.id=VVENC_VVC, .name="VVenC VVC", .codec="VVC", .kind=BackendKind::Software, .queryCapabilities=query_vvenc_backend, .encode=encode_vvenc_still_image, .decodePreview=decode_vvc_preview, .summarizeParams=summarize_generic, .inputGeometry=InputGeometryPolicy::PadToChromaAlignment},
		{.id=SVT_AV1, .name="SVT-AV1", .codec="AV1", .kind=BackendKind::Software, .queryCapabilities=query_svt_av1_backend, .encode=encode_svt_av1_still_image, .decodePreview=decode_av1_preview, .summarizeParams=summarize_generic, .inputGeometry=InputGeometryPolicy::PadToChromaAlignment},
	};
	bool assignedPrimaryHevc = false;
	bool assignedPrimaryAv1 = false;
	for (std::size_t index = 0; index < vaapiDevices.size(); ++index) {
		const VaapiDeviceInfo& device = vaapiDevices[index];
		if (!device.initialized) continue;
		if (device.supportsHevcEncode) {
			const BackendId id = assignedPrimaryHevc ? BackendId{10000u + static_cast<uint64_t>(index) * 2u} : VAAPI_HEVC;
			assignedPrimaryHevc = true;
			backends.push_back({
				.id=id,
				.name=vaapi_backend_name("HEVC", device),
				.codec="HEVC",
				.kind=BackendKind::Hardware,
				.queryCapabilities=[device] { return query_vaapi_backend(device, false); },
				.encode=[path = device.path](const RawImage& image, std::span<const EncoderParam> params) {
					return encode_vaapi_hevc_still_image(image, params, path);
				},
				.decodePreview=decode_hevc_preview,
				.summarizeParams=summarize_generic,
				.inputGeometry=InputGeometryPolicy::PadToChromaAlignment,
				.device=device.path,
			});
		}
		if (device.supportsAv1Encode) {
			const BackendId id = assignedPrimaryAv1 ? BackendId{10001u + static_cast<uint64_t>(index) * 2u} : VAAPI_AV1;
			assignedPrimaryAv1 = true;
			backends.push_back({
				.id=id,
				.name=vaapi_backend_name("AV1", device),
				.codec="AV1",
				.kind=BackendKind::Hardware,
				.queryCapabilities=[device] { return query_vaapi_backend(device, true); },
				.encode=[path = device.path](const RawImage& image, std::span<const EncoderParam> params) {
					return encode_vaapi_av1_still_image(image, params, path);
				},
				.decodePreview=decode_av1_preview,
				.summarizeParams=summarize_generic,
				.inputGeometry=InputGeometryPolicy::PadToChromaAlignment,
				.device=device.path,
			});
		}
	}
	if (!assignedPrimaryHevc) {
		backends.push_back(unavailable_vaapi_backend(VAAPI_HEVC, "HEVC", vaapi_discovery_error(vaapiDevices, "HEVC")));
	}
	if (!assignedPrimaryAv1) {
		backends.push_back(unavailable_vaapi_backend(VAAPI_AV1, "AV1", vaapi_discovery_error(vaapiDevices, "AV1")));
	}
	backends.insert(backends.end(), {
		{.id=UVG266_VVC, .name="uvg266 VVC", .codec="VVC", .kind=BackendKind::Software, .queryCapabilities=query_uvg266_backend, .encode=encode_uvg266_still_image, .decodePreview=decode_vvc_preview, .summarizeParams=summarize_generic, .inputGeometry=InputGeometryPolicy::PadToChromaAlignment},
		{.id=AV2, .name="AVM AV2", .codec="AV2", .kind=BackendKind::Software, .queryCapabilities=query_av2_backend, .encode=encode_av2_still_image, .decodePreview=decode_av2_preview, .summarizeParams=summarize_generic, .inputGeometry=InputGeometryPolicy::PadToChromaAlignment},
		{.id=JPEGLS, .name="CharLS JPEG-LS", .codec="JPEG-LS", .kind=BackendKind::Software, .queryCapabilities=query_jpegls_backend, .encode=encode_jpegls_still_image, .decodePreview=decode_jpegls_preview, .summarizeParams=summarize_generic},
		{.id=JPEG, .name="libjpeg JPEG", .codec="JPEG", .kind=BackendKind::Software, .queryCapabilities=query_jpeg_backend, .encode=encode_jpeg_still_image, .decodePreview=decode_jpeg_preview, .summarizeParams=summarize_generic},
		{.id=JPEG2000, .name="OpenJPEG JPEG 2000", .codec="JPEG 2000", .kind=BackendKind::Software, .queryCapabilities=query_jpeg2000_backend, .encode=encode_jpeg2000_still_image, .decodePreview=decode_jpeg2000_preview, .summarizeParams=summarize_generic},
		{.id=JPEGXL, .name="libjxl JPEG XL", .codec="JPEG XL", .kind=BackendKind::Software, .queryCapabilities=query_jpegxl_backend, .encode=encode_jpegxl_still_image, .decodePreview=decode_jpegxl_preview, .summarizeParams=summarize_generic},
		{.id=JPEGXR, .name="jxrlib JPEG XR", .codec="JPEG XR", .kind=BackendKind::Software, .queryCapabilities=query_jpegxr_backend, .encode=encode_jpegxr_still_image, .decodePreview=decode_jpegxr_preview, .summarizeParams=summarize_generic},
		{.id=PNG, .name="libpng PNG", .codec="PNG", .kind=BackendKind::Software, .queryCapabilities=query_png_backend, .encode=encode_png_still_image, .decodePreview=decode_png_preview, .summarizeParams=summarize_generic},
		{.id=X264_H264_INTRA, .name="x264 H.264 Intra", .codec="H.264", .kind=BackendKind::Software, .queryCapabilities=query_x264_backend, .encode=encode_x264_intra_still_image, .decodePreview=decode_h264_preview, .summarizeParams=summarize_generic, .inputGeometry=InputGeometryPolicy::PadToChromaAlignment},
        {.id=JPEGXS,.name="SVT-JPEG-XS",.codec="JPEG XS",.kind=BackendKind::Software,.queryCapabilities=query_jpegxs_backend,.encode=encode_jpegxs_still_image,.decodePreview=decode_jpegxs_preview,.summarizeParams=summarize_generic,.inputGeometry=InputGeometryPolicy::PadToChromaAlignment},
		{.id=KVAZAAR,.name="Kvazaar HEVC",.codec="HEVC",.kind=BackendKind::Software,.queryCapabilities=query_kvazaar_backend,.encode=encode_kvazaar_still_image,.decodePreview=decode_hevc_preview,.summarizeParams=summarize_generic},
		{.id=WEBP,.name="libwebp",.codec="WebP",.kind=BackendKind::Software,.queryCapabilities=query_webp_backend,.encode=encode_webp_still_image,.decodePreview=decode_webp_preview,.summarizeParams=summarize_generic},
		{.id=VPX_VP8,.name="libvpx VP8",.codec="VP8",.kind=BackendKind::Software,.queryCapabilities=query_vpx_vp8_backend,.encode=encode_vpx_vp8_still_image,.decodePreview=decode_vp8_preview,.summarizeParams=summarize_generic,.inputGeometry=InputGeometryPolicy::PadToChromaAlignment},
		{.id=VPX_VP9,.name="libvpx VP9",.codec="VP9",.kind=BackendKind::Software,.queryCapabilities=query_vpx_vp9_backend,.encode=encode_vpx_vp9_still_image,.decodePreview=decode_vp9_preview,.summarizeParams=summarize_generic,.inputGeometry=InputGeometryPolicy::PadToChromaAlignment},
		{.id=SVT_VP9,.name="SVT-VP9",.codec="VP9",.kind=BackendKind::Software,.queryCapabilities=query_svt_vp9_backend,.encode=encode_svt_vp9_still_image,.decodePreview=decode_vp9_preview,.summarizeParams=summarize_generic},
		{.id=RAV1E,.name="librav1e AV1",.codec="AV1",.kind=BackendKind::Software,.queryCapabilities=query_rav1e_backend,.encode=encode_rav1e_still_image,.decodePreview=decode_av1_preview,.summarizeParams=summarize_generic,.inputGeometry=InputGeometryPolicy::PadToChromaAlignment},
	});
	return backends;
}

std::vector<EncoderBackend> initial_encoder_backends() {
	return initial_encoder_backends(discover_vaapi_devices());
}

const EncoderBackend* find_backend(std::span<const EncoderBackend> backends, BackendId id) {
	const auto it = std::ranges::find_if(backends, [id](const EncoderBackend& backend) {
		return backend.id == id;
	});
	return it == backends.end() ? nullptr : &*it;
}

std::vector<BackendInfo> query_backend_infos(std::span<const EncoderBackend> backends) {
	std::vector<BackendInfo> infos;
	infos.reserve(backends.size());
	for (const EncoderBackend& backend : backends) {
		CapabilityResult caps = backend.queryCapabilities();
		BackendInfo info;
		info.id = backend.id;
		info.name = backend.name;
		info.codec = backend.codec;
		info.kind = backend.kind;
		info.capabilities = std::move(caps.snapshot);
		info.params = std::move(caps.params);
		infos.push_back(std::move(info));
	}
	return infos;
}

static EncodeResult run_backend_encode_impl(const EncoderBackend& backend, const RawImage& image, std::span<const EncoderParam> params,
    std::shared_ptr<const RawImage> owner, EncodePreparationCache* cache) {
	const auto preparationStart=std::chrono::steady_clock::now();
	if (backend.encode == nullptr) {
		throw std::runtime_error("backend has no encode function");
	}
	std::string bitDepthValue = "source";
	std::string chroma = "source";
	std::string primaries = "source";
	std::string transfer = "source";
	std::string matrix = "source";
	std::string range = "source";
	std::string toneMap = "none";
	double sourcePeak = 1000.0;
	double targetPeak = 203.0;
	std::vector<EncoderParam> backendParams;
	std::vector<std::pair<std::string,ParamValue>> conversionKey;
	for (const EncoderParam& param : params) {
		const auto codecParamCount=backendParams.size();
		const std::string value = param_value_to_string(param.value);
		if (param.name == "bit-depth") bitDepthValue = value;
		else if (param.name == "chroma-subsampling") chroma = value;
		else if (param.name == "color-primaries") primaries = value;
		else if (param.name == "transfer") transfer = value;
		else if (param.name == "matrix") matrix = value;
		else if (param.name == "range") range = value;
		else if (param.name == "tone-map") toneMap = value;
		else if (param.name == "source-peak-nits") sourcePeak = std::get<double>(param.value);
		else if (param.name == "target-peak-nits") targetPeak = std::get<double>(param.value);
		else backendParams.push_back(param);
		if (backendParams.size()==codecParamCount)conversionKey.emplace_back(param.name,param.value);
	}
	std::sort(conversionKey.begin(),conversionKey.end(),[](const auto& a,const auto& b){return a.first<b.first;});
	const int targetDepth = bitDepthValue == "source" ? bit_depth(image.format) : std::stoi(bitDepthValue);
	const PixelFormat targetFormat = output_format(image.format, targetDepth, backend.id == WEBP ? "444" : chroma);
	ColorTransformOptions transform;
	transform.target = image.color;
	if (is_rgb(image.format)) {
		transform.target.range = ColorRange::Limited;
	}
	if (primaries == "bt709") transform.target.primaries = ColorPrimaries::BT709;
	else if (primaries == "display-p3") transform.target.primaries = ColorPrimaries::DisplayP3;
	else if (primaries == "bt2020") transform.target.primaries = ColorPrimaries::BT2020;
	else if (primaries != "source") throw std::invalid_argument("unsupported target primaries: " + primaries);
	if (is_rgb(image.format) && matrix == "source") {
		transform.target.matrix = transform.target.primaries == ColorPrimaries::BT2020
			? MatrixCoefficients::BT2020NonConstant
			: MatrixCoefficients::BT709;
	}
	if (transfer == "srgb") transform.target.transfer = TransferCharacteristics::SRGB;
	else if (transfer == "bt709") transform.target.transfer = TransferCharacteristics::BT709;
	else if (transfer == "linear") transform.target.transfer = TransferCharacteristics::Linear;
	else if (transfer == "pq") transform.target.transfer = TransferCharacteristics::PQ;
	else if (transfer == "hlg") transform.target.transfer = TransferCharacteristics::HLG;
	else if (transfer != "source") throw std::invalid_argument("unsupported target transfer: " + transfer);
	if (matrix == "bt709") transform.target.matrix = MatrixCoefficients::BT709;
	else if (matrix == "bt2020nc") transform.target.matrix = MatrixCoefficients::BT2020NonConstant;
	else if (matrix != "source") throw std::invalid_argument("unsupported target matrix: " + matrix);
	if (range == "limited") transform.target.range = ColorRange::Limited;
	else if (range == "full") transform.target.range = ColorRange::Full;
	else if (range != "source") throw std::invalid_argument("unsupported target range: " + range);
	if (toneMap == "clip") transform.toneMap = ToneMapMode::Clip;
	else if (toneMap == "reinhard") transform.toneMap = ToneMapMode::Reinhard;
	else if (toneMap != "none") throw std::invalid_argument("unsupported tone-map mode: " + toneMap);
	transform.sourcePeakNits = sourcePeak;
	transform.targetPeakNits = targetPeak;
	const bool cacheHit=cache && cache->source==owner && cache->backend==backend.id && cache->conversionKey==conversionKey && cache->prepared;
	std::shared_ptr<const RawImage> prepared=cacheHit?cache->prepared:nullptr;
	if(!prepared) {
	RawImage transformed;
	const bool directWebpRgb = backend.id == WEBP && image.format == PixelFormat::RGBP8 &&
		image.color.range == ColorRange::Full && image.color.primaries == ColorPrimaries::BT709 &&
		image.color.transfer == TransferCharacteristics::SRGB && targetDepth == 8 &&
		transform.target.primaries == ColorPrimaries::BT709 && transform.target.transfer == TransferCharacteristics::SRGB &&
		transform.toneMap == ToneMapMode::None;
	const bool unchanged=!is_rgb(image.format) && image.format==targetFormat &&
        image.color.primaries==transform.target.primaries && image.color.transfer==transform.target.transfer &&
        image.color.matrix==transform.target.matrix && image.color.range==transform.target.range &&
        image.color.chroma420Location==transform.target.chroma420Location && transform.toneMap==ToneMapMode::None &&
        (backend.inputGeometry!=InputGeometryPolicy::PadToChromaAlignment ||
          ((!is_420(image.format)&&!is_422(image.format)) || (image.width%2==0 && (!is_420(image.format)||image.height%2==0))));
	if(owner && unchanged)prepared=owner;
	else {
	if (directWebpRgb) transformed = image;
	else transformed = transform_raw_image(image, targetFormat, transform);
	if (backend.id == WEBP) {
		if(targetDepth != 8 || transformed.color.primaries != ColorPrimaries::BT709 || transformed.color.transfer != TransferCharacteristics::SRGB)
			throw std::invalid_argument("WebP requires 8-bit sRGB / BT.709; explicitly select those targets and tone mapping for HDR or wide-gamut sources");
		if(!directWebpRgb) {
			const auto rgba=raw_image_to_rgba8(transformed);
			RawImage rgb; rgb.width=image.width; rgb.height=image.height; rgb.format=PixelFormat::RGBP8;
			for(auto& p:rgb.planes) {p.strideBytes=image.width; p.bytes.resize(std::size_t(image.width)*image.height);}
			for(std::size_t i=0;i<std::size_t(image.width)*image.height;++i) for(int p=0;p<3;++p) rgb.planes[p].bytes[i]=rgba[i*4+p];
			transformed=std::move(rgb);
		}
		transformed.color={.primaries=ColorPrimaries::BT709,.transfer=TransferCharacteristics::SRGB,
			.matrix=MatrixCoefficients::Identity,.range=ColorRange::Full};
	}
	if (backend.inputGeometry == InputGeometryPolicy::PadToChromaAlignment) {
		transformed = pad_subsampled_input(std::move(transformed));
	}
	prepared=std::make_shared<RawImage>(std::move(transformed));
	}
	if(cache){cache->source=std::move(owner);cache->prepared=prepared;cache->backend=backend.id;cache->conversionKey=std::move(conversionKey);}
	}
	const RawImage& transformed=*prepared;
	const bool rgbStillImplementation =
		backend.id.value >= JPEGLS.value && backend.id.value <= PNG.value && backend.id != JPEGXL;
	if (rgbStillImplementation &&
	    (transformed.color.primaries != ColorPrimaries::BT709 || transformed.color.matrix != MatrixCoefficients::BT709 ||
	     (transformed.color.transfer != TransferCharacteristics::SRGB && transformed.color.transfer != TransferCharacteristics::BT709))) {
		throw std::invalid_argument(backend.name + " uses a BT.709 RGB path; explicitly transform primaries/matrix to BT.709 and transfer to sRGB or BT.709");
	}
	if (backend.id == VVENC_VVC) {
		std::erase_if(backendParams, [](const EncoderParam& param) { return param.name == "hdr" || param.name == "sdr" || param.name == "range"; });
		std::string colorOption;
		std::string colorMode;
		if (transformed.color.primaries == ColorPrimaries::BT2020 && transformed.color.transfer == TransferCharacteristics::PQ) { colorOption = "hdr"; colorMode = "pq_2020"; }
		else if (transformed.color.primaries == ColorPrimaries::BT2020 && transformed.color.transfer == TransferCharacteristics::HLG) { colorOption = "hdr"; colorMode = "hlg_2020"; }
		else if (transformed.color.primaries == ColorPrimaries::BT2020) { colorOption = "sdr"; colorMode = "sdr_2020"; }
		else if (transformed.color.primaries == ColorPrimaries::BT709 && transformed.color.transfer != TransferCharacteristics::PQ && transformed.color.transfer != TransferCharacteristics::HLG) { colorOption = "sdr"; colorMode = "sdr_709"; }
		else throw std::invalid_argument("VVenC cannot signal the selected explicit color description through its hdr mode");
		backendParams.push_back({.name=colorOption, .value=colorMode});
		backendParams.push_back({.name="range", .value=std::string{transformed.color.range == ColorRange::Full ? "full" : "limited"}});
	}
	EncodeResult result;
	result.comparisonReference = std::move(prepared);
	const auto start = std::chrono::steady_clock::now();
	result.encoded = backend.encode(*result.comparisonReference, backendParams);
    // Retain the actual transformed input interpretation when a codec or a
    // decoder API cannot carry/report every color-description field.
    if(!result.encoded.codedColor)result.encoded.codedColor=result.comparisonReference->color;
	const auto finish = std::chrono::steady_clock::now();
	result.metadata = metadata_for_backend(backend, result.encoded, params);
	result.metadata.encoderInput = result.comparisonReference;
	result.metadata.codedPixelFormat = result.comparisonReference->format;
	result.metadata.codedColor = result.comparisonReference->color;
	result.metadata.encodeSeconds = std::chrono::duration<double>(finish - start).count();
	result.metadata.prepareSeconds=std::chrono::duration<double>(start-preparationStart).count();
	result.metadata.preparationCacheHit=cacheHit;
	return result;
}

EncodeResult run_backend_encode(const EncoderBackend& backend,const RawImage& image,std::span<const EncoderParam> params) {
    return run_backend_encode_impl(backend,image,params,{},nullptr);
}
EncodeResult run_backend_encode(const EncoderBackend& backend,std::shared_ptr<const RawImage> image,std::span<const EncoderParam> params,EncodePreparationCache& cache) {
    if(!image)throw std::invalid_argument("missing encoder source");
    return run_backend_encode_impl(backend,*image,params,image,&cache);
}

DecodeResult decode_backend_preview(const EncoderBackend& backend,const EncodedImage& encoded) {
    if(backend.codec=="HEVC" && hevc_is_scc(encoded)) return decode_scc_vulkan(encoded);
    if(backend.kind==BackendKind::Hardware && !backend.device.empty() && (backend.codec=="HEVC" || backend.codec=="AV1")) {
        auto decoded=decode_libavcodec(encoded,backend.codec=="HEVC"?"hevc":"av1","vaapi",backend.device,true);
        if(decoded.image)return decoded;
    }
    if(backend.codec=="JPEG XS") {
        try{return {.image=decode_jpegxs_image(encoded,4),.error={}};}
        catch(const std::exception& e){return {.image=nullptr,.error=e.what()};}
    }
    return backend.decodePreview(encoded);
}

} // namespace codec_gui::gui

namespace codec_gui::gui {
namespace {

void append_output_controls(std::vector<EncoderParamInfo>& params, const OutputControlPolicy& policy) {
	auto has = [&](std::string_view name) {
		return std::ranges::any_of(params, [&](const EncoderParamInfo& param) { return param.name == name; });
	};
	if (!has("bit-depth") && !policy.bitDepths.empty()) params.push_back({.name="bit-depth", .label="Output bit depth", .group="Output Format", .kind=ParamKind::Enum, .defaultValue=policy.defaultBitDepth, .enumValues=policy.bitDepths, .help="Explicit coded sample precision supported by this encoder implementation."});
	if (!has("chroma-subsampling") && !policy.chromaFormats.empty()) params.push_back({.name="chroma-subsampling", .label="Chroma subsampling", .group="Output Format", .kind=ParamKind::Enum, .defaultValue=policy.defaultChroma, .enumValues=policy.chromaFormats, .help="Explicit output chroma layout supported by this encoder implementation."});
	if (policy.colorPrimaries && !has("color-primaries")) params.push_back({.name="color-primaries", .label="Target primaries", .group="Color Transform", .kind=ParamKind::Enum, .defaultValue=std::string{"source"}, .enumValues={{.value="source",.label="Source"},{.value="bt709",.label="BT.709"},{.value="display-p3",.label="Display P3"},{.value="bt2020",.label="BT.2020"}}, .help="H.273 colour_primaries for the target plus a real linear-light gamut conversion; this is not metadata-only signaling."});
	if (policy.transfer && !has("transfer")) params.push_back({.name="transfer", .label="Target transfer / EOTF", .group="Color Transform", .kind=ParamKind::Enum, .defaultValue=std::string{"source"}, .enumValues={{.value="source",.label="Source"},{.value="srgb",.label="sRGB"},{.value="bt709",.label="BT.709"},{.value="linear",.label="Linear"},{.value="pq",.label="PQ (H.273 16)"},{.value="hlg",.label="HLG (H.273 18)"}}, .help="Explicit H.273 transfer characteristics. PQ is defined by H.273 for 10-, 12-, 14-, and 16-bit systems."});
	if (policy.matrix && !has("matrix")) params.push_back({.name="matrix", .label="Target matrix", .group="Color Transform", .kind=ParamKind::Enum, .defaultValue=std::string{"source"}, .enumValues={{.value="source",.label="Source"},{.value="bt709",.label="BT.709"},{.value="bt2020nc",.label="BT.2020 non-constant"}}, .help="Explicit H.273 matrix coefficients used for the actual RGB/Y'CbCr conversion and bitstream signaling."});
	if (policy.range && !has("range")) params.push_back({.name="range", .label="Signal range", .group="Color Transform", .kind=ParamKind::Enum, .defaultValue=std::string{"source"}, .enumValues={{.value="source",.label="Source"},{.value="limited",.label="Limited"},{.value="full",.label="Full"}}, .help="Explicit full or limited range. Limited-range code values follow the bit-depth-generic H.273 equations."});
	if (policy.toneMapping && !has("tone-map")) params.push_back({.name="tone-map", .label="Tone / gamut mapping", .group="Color Transform", .kind=ParamKind::Enum, .defaultValue=std::string{"none"}, .enumValues={{.value="none",.label="None (reject narrowing)"},{.value="clip",.label="Explicit clip"},{.value="reinhard",.label="Reinhard"}}, .help="Required when converting HDR to SDR or a wider gamut to BT.709. None rejects the conversion instead of applying an implicit mapping."});
	if (policy.toneMapping && !has("source-peak-nits")) params.push_back({.name="source-peak-nits", .label="Source peak luminance", .group="Color Transform", .kind=ParamKind::Float, .defaultValue=1000.0, .floatRange=FloatRange{.min=80.0,.max=10000.0,.step=10.0}, .help="Explicit source peak luminance used by tone and gamut mapping, in cd/m²."});
	if (policy.toneMapping && !has("target-peak-nits")) params.push_back({.name="target-peak-nits", .label="Target peak luminance", .group="Color Transform", .kind=ParamKind::Float, .defaultValue=203.0, .floatRange=FloatRange{.min=80.0,.max=10000.0,.step=10.0}, .help="Explicit target peak luminance used by tone and gamut mapping, in cd/m²."});
}

PixelFormat output_format(PixelFormat source, int depth, std::string_view chroma) {
	if (chroma == "source") {
		if (is_gray(source)) chroma = "400";
		else if (is_420(source)) chroma = "420";
		else if (is_422(source)) chroma = "422";
		else chroma = "444";
	}
	if (depth != 8 && depth != 10 && depth != 12 && depth != 14 && depth != 16) throw std::invalid_argument("output bit depth must be 8, 10, 12, 14, or 16");
	if (chroma == "420" && depth <= 14) return depth == 8 ? PixelFormat::YUV420P8 : depth == 10 ? PixelFormat::YUV420P10LE : depth == 12 ? PixelFormat::YUV420P12LE : PixelFormat::YUV420P14LE;
	if (chroma == "422" && depth <= 14) return depth == 8 ? PixelFormat::YUV422P8 : depth == 10 ? PixelFormat::YUV422P10LE : depth == 12 ? PixelFormat::YUV422P12LE : PixelFormat::YUV422P14LE;
	if (chroma == "444") return depth == 8 ? PixelFormat::YUV444P8 : depth == 10 ? PixelFormat::YUV444P10LE : depth == 12 ? PixelFormat::YUV444P12LE : depth == 14 ? PixelFormat::YUV444P14LE : PixelFormat::YUV444P16LE;
	if (chroma == "400" && depth <= 14) return depth == 8 ? PixelFormat::Gray8 : depth == 10 ? PixelFormat::Gray10LE : depth == 12 ? PixelFormat::Gray12LE : PixelFormat::Gray14LE;
	throw std::invalid_argument("unsupported chroma subsampling: " + std::string{chroma});
}

} // namespace
} // namespace codec_gui::gui
