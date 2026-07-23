#include "encoder_backends.hpp"
#include "preview_decoders.hpp"
#include "raw_image_conversion.hpp"
#include "raw_image_utils.hpp"

#include <algorithm>
#include <filesystem>
#include <sstream>
#include <stdexcept>
#include <type_traits>

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
		out.push_back({param.name, param_value_to_string(param.value)});
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
		{{{"8", "8-bit"}, {"10", "10-bit"}, {"12", "12-bit"}}, "12",
		 {{"source", "Source"}, {"420", "4:2:0"}, {"422", "4:2:2"}, {"444", "4:4:4"}, {"400", "Monochrome"}}, "source",
		 true, true, true, true, true}
	);
}

CapabilityResult query_vvenc_backend() {
	return query_software_capabilities<query_vvenc_parameters>(
		"VVenC",
		{{{"8", "8-bit"}, {"10", "10-bit"}}, "10",
		 {{"420", "4:2:0"}, {"400", "Monochrome"}}, "420"}
	);
}

CapabilityResult query_svt_av1_backend() {
	return query_software_capabilities<query_svt_av1_parameters>(
		"SVT-AV1",
		{{{"8", "8-bit"}, {"10", "10-bit"}}, "10",
		 {{"source", "Source"}, {"420", "4:2:0"}, {"422", "4:2:2"}, {"444", "4:4:4"}, {"400", "Monochrome"}}, "source",
		 true, true, true, true, true}
	);
}

CapabilityResult query_uvg266_backend() {
	return query_software_capabilities<query_uvg266_parameters>(
		"uvg266",
		{{}, "",
		 {{"420", "4:2:0"}, {"400", "Monochrome"}}, "420",
		 true, true, true, true, true}
	);
}

CapabilityResult query_av2_backend() {
	return query_software_capabilities<query_av2_parameters>(
		"AVM AV2",
		{{{"8", "8-bit"}, {"10", "10-bit"}}, "10",
		 {{"source", "Source"}, {"420", "4:2:0"}, {"422", "4:2:2"}, {"444", "4:4:4"}, {"400", "Monochrome"}}, "source",
		 true, true, true, true, true}
	);
}

CapabilityResult query_jpegls_backend() {
	return query_software_capabilities<query_jpegls_parameters>(
		"CharLS JPEG-LS", {{{"8", "8-bit"}}, "8"}
	);
}

CapabilityResult query_jpeg_backend() {
	return query_software_capabilities<query_jpeg_parameters>(
		"libjpeg", {{{"8", "8-bit"}}, "8"}
	);
}

CapabilityResult query_jpeg2000_backend() {
	return query_software_capabilities<query_jpeg2000_parameters>(
		"OpenJPEG", {{{"8", "8-bit"}}, "8"}
	);
}

CapabilityResult query_jpegxl_backend() {
	return query_software_capabilities<query_jpegxl_parameters>(
		"libjxl",
		{{{"source", "Source"}, {"8", "8-bit"}, {"10", "10-bit"}, {"12", "12-bit"}, {"14", "14-bit"}}, "source",
		 {}, "", true, true, false, false, true}
	);
}

CapabilityResult query_jpegxr_backend() {
	return query_software_capabilities<query_jpegxr_parameters>(
		"jxrlib", {{{"8", "8-bit"}}, "8"}
	);
}

CapabilityResult query_png_backend() {
	return query_software_capabilities<query_png_parameters>(
		"libpng", {{{"8", "8-bit"}}, "8"}
	);
}

CapabilityResult query_x264_backend() {
	return query_software_capabilities<query_x264_parameters>(
		"x264", {{{"8", "8-bit"}}, "8", {{"420", "4:2:0"}}, "420"}
	);
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
			{{}, "", {}, "", true, true, true, true, true}
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

EncoderBackend unavailable_vaapi_backend(BackendId id, std::string codec, std::string error) {
	const std::string name = "VA-API " + codec;
	return {
		id,
		name,
		codec,
		BackendKind::Hardware,
		[name, error] {
			CapabilityResult result;
			result.snapshot.implementation = name;
			result.snapshot.available = false;
			result.snapshot.error = error;
			return result;
		},
		[error](const RawImage&, std::span<const EncoderParam>) -> EncodedImage { throw std::runtime_error(error); },
		codec == "AV1" ? decode_av1_preview : decode_hevc_preview,
		summarize_generic,
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
		{X265_HEVC, "x265 HEVC", "HEVC", BackendKind::Software, query_x265_backend, encode_x265_still_image, decode_hevc_preview, summarize_generic},
		{VVENC_VVC, "VVenC VVC", "VVC", BackendKind::Software, query_vvenc_backend, encode_vvenc_still_image, decode_vvc_preview, summarize_generic},
		{SVT_AV1, "SVT-AV1", "AV1", BackendKind::Software, query_svt_av1_backend, encode_svt_av1_still_image, decode_av1_preview, summarize_generic},
	};
	bool assignedPrimaryHevc = false;
	bool assignedPrimaryAv1 = false;
	for (std::size_t index = 0; index < vaapiDevices.size(); ++index) {
		const VaapiDeviceInfo device = vaapiDevices[index];
		if (!device.initialized) continue;
		if (device.supportsHevcEncode) {
			const BackendId id = assignedPrimaryHevc ? BackendId{10000u + static_cast<uint64_t>(index) * 2u} : VAAPI_HEVC;
			assignedPrimaryHevc = true;
			backends.push_back({
				id,
				vaapi_backend_name("HEVC", device),
				"HEVC",
				BackendKind::Hardware,
				[device] { return query_vaapi_backend(device, false); },
				[path = device.path](const RawImage& image, std::span<const EncoderParam> params) {
					return encode_vaapi_hevc_still_image(image, params, path);
				},
				decode_hevc_preview,
				summarize_generic,
			});
		}
		if (device.supportsAv1Encode) {
			const BackendId id = assignedPrimaryAv1 ? BackendId{10001u + static_cast<uint64_t>(index) * 2u} : VAAPI_AV1;
			assignedPrimaryAv1 = true;
			backends.push_back({
				id,
				vaapi_backend_name("AV1", device),
				"AV1",
				BackendKind::Hardware,
				[device] { return query_vaapi_backend(device, true); },
				[path = device.path](const RawImage& image, std::span<const EncoderParam> params) {
					return encode_vaapi_av1_still_image(image, params, path);
				},
				decode_av1_preview,
				summarize_generic,
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
		{UVG266_VVC, "uvg266 VVC", "VVC", BackendKind::Software, query_uvg266_backend, encode_uvg266_still_image, decode_vvc_preview, summarize_generic},
		{AV2, "AVM AV2", "AV2", BackendKind::Software, query_av2_backend, encode_av2_still_image, decode_av2_preview, summarize_generic},
		{JPEGLS, "CharLS JPEG-LS", "JPEG-LS", BackendKind::Software, query_jpegls_backend, encode_jpegls_still_image, decode_jpegls_preview, summarize_generic},
		{JPEG, "libjpeg JPEG", "JPEG", BackendKind::Software, query_jpeg_backend, encode_jpeg_still_image, decode_jpeg_preview, summarize_generic},
		{JPEG2000, "OpenJPEG JPEG 2000", "JPEG 2000", BackendKind::Software, query_jpeg2000_backend, encode_jpeg2000_still_image, decode_jpeg2000_preview, summarize_generic},
		{JPEGXL, "libjxl JPEG XL", "JPEG XL", BackendKind::Software, query_jpegxl_backend, encode_jpegxl_still_image, decode_jpegxl_preview, summarize_generic},
		{JPEGXR, "jxrlib JPEG XR", "JPEG XR", BackendKind::Software, query_jpegxr_backend, encode_jpegxr_still_image, decode_jpegxr_preview, summarize_generic},
		{PNG, "libpng PNG", "PNG", BackendKind::Software, query_png_backend, encode_png_still_image, decode_png_preview, summarize_generic},
		{X264_H264_INTRA, "x264 H.264 Intra", "H.264", BackendKind::Software, query_x264_backend, encode_x264_intra_still_image, decode_h264_preview, summarize_generic},
	});
	return backends;
}

std::vector<EncoderBackend> initial_encoder_backends() {
	return initial_encoder_backends(discover_vaapi_devices());
}

const EncoderBackend* find_backend(std::span<const EncoderBackend> backends, BackendId id) {
	const auto it = std::find_if(backends.begin(), backends.end(), [id](const EncoderBackend& backend) {
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

EncodeResult run_backend_encode(const EncoderBackend& backend, const RawImage& image, std::span<const EncoderParam> params) {
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
	for (const EncoderParam& param : params) {
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
	}
	const int targetDepth = bitDepthValue == "source" ? bit_depth(image.format) : std::stoi(bitDepthValue);
	const PixelFormat targetFormat = output_format(image.format, targetDepth, chroma);
	const bool rgb8StillImplementation = backend.id.value >= JPEGLS.value && backend.id.value <= X264_H264_INTRA.value && backend.id != JPEGXL;
	if (rgb8StillImplementation && targetDepth > 8) {
		throw std::invalid_argument(backend.name + " currently has an 8-bit RGB implementation; refusing an implicit high-bit-depth reduction");
	}
	if (targetDepth > 10 && backend.id != X265_HEVC && backend.id != JPEGXL) {
		throw std::invalid_argument(backend.name + " does not support " + std::to_string(targetDepth) + "-bit input in this implementation");
	}
	ColorTransformOptions transform;
	transform.target = image.color;
	if (primaries == "bt709") transform.target.primaries = ColorPrimaries::BT709;
	else if (primaries == "display-p3") transform.target.primaries = ColorPrimaries::DisplayP3;
	else if (primaries == "bt2020") transform.target.primaries = ColorPrimaries::BT2020;
	else if (primaries != "source") throw std::invalid_argument("unsupported target primaries: " + primaries);
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
	RawImage transformed = transform_raw_image(image, targetFormat, transform);
	if (rgb8StillImplementation &&
	    (transformed.color.primaries != ColorPrimaries::BT709 || transformed.color.matrix != MatrixCoefficients::BT709 ||
	     (transformed.color.transfer != TransferCharacteristics::SRGB && transformed.color.transfer != TransferCharacteristics::BT709))) {
		throw std::invalid_argument(backend.name + " uses an 8-bit BT.709 RGB path; explicitly transform primaries/matrix to BT.709 and transfer to sRGB or BT.709");
	}
	if (backend.id == VVENC_VVC) {
		backendParams.erase(std::remove_if(backendParams.begin(), backendParams.end(), [](const EncoderParam& param) { return param.name == "hdr" || param.name == "sdr" || param.name == "range"; }), backendParams.end());
		std::string colorOption;
		std::string colorMode;
		if (transformed.color.primaries == ColorPrimaries::BT2020 && transformed.color.transfer == TransferCharacteristics::PQ) { colorOption = "hdr"; colorMode = "pq_2020"; }
		else if (transformed.color.primaries == ColorPrimaries::BT2020 && transformed.color.transfer == TransferCharacteristics::HLG) { colorOption = "hdr"; colorMode = "hlg_2020"; }
		else if (transformed.color.primaries == ColorPrimaries::BT2020) { colorOption = "sdr"; colorMode = "sdr_2020"; }
		else if (transformed.color.primaries == ColorPrimaries::BT709 && transformed.color.transfer != TransferCharacteristics::PQ && transformed.color.transfer != TransferCharacteristics::HLG) { colorOption = "sdr"; colorMode = "sdr_709"; }
		else throw std::invalid_argument("VVenC cannot signal the selected explicit color description through its hdr mode");
		backendParams.push_back({colorOption, colorMode});
		backendParams.push_back({"range", std::string{transformed.color.range == ColorRange::Full ? "full" : "limited"}});
	}
	EncodeResult result;
	result.comparisonReference = std::make_shared<RawImage>(std::move(transformed));
	const auto start = std::chrono::steady_clock::now();
	result.encoded = backend.encode(*result.comparisonReference, backendParams);
	const auto finish = std::chrono::steady_clock::now();
	result.metadata = metadata_for_backend(backend, result.encoded, params);
	result.metadata.codedPixelFormat = result.comparisonReference->format;
	result.metadata.codedColor = result.comparisonReference->color;
	result.metadata.encodeSeconds = std::chrono::duration<double>(finish - start).count();
	return result;
}

} // namespace codec_gui::gui

namespace codec_gui::gui {
namespace {

void append_output_controls(std::vector<EncoderParamInfo>& params, const OutputControlPolicy& policy) {
	auto has = [&](std::string_view name) {
		return std::any_of(params.begin(), params.end(), [&](const EncoderParamInfo& param) { return param.name == name; });
	};
	if (!has("bit-depth") && !policy.bitDepths.empty()) params.push_back({.name="bit-depth", .label="Output bit depth", .group="Output Format", .kind=ParamKind::Enum, .defaultValue=policy.defaultBitDepth, .enumValues=policy.bitDepths, .help="Explicit coded sample precision supported by this encoder implementation."});
	if (!has("chroma-subsampling") && !policy.chromaFormats.empty()) params.push_back({.name="chroma-subsampling", .label="Chroma subsampling", .group="Output Format", .kind=ParamKind::Enum, .defaultValue=policy.defaultChroma, .enumValues=policy.chromaFormats, .help="Explicit output chroma layout supported by this encoder implementation."});
	if (policy.colorPrimaries && !has("color-primaries")) params.push_back({.name="color-primaries", .label="Target primaries", .group="Color Transform", .kind=ParamKind::Enum, .defaultValue=std::string{"source"}, .enumValues={{"source","Source"},{"bt709","BT.709"},{"display-p3","Display P3"},{"bt2020","BT.2020"}}, .help="H.273 colour_primaries for the target plus a real linear-light gamut conversion; this is not metadata-only signaling."});
	if (policy.transfer && !has("transfer")) params.push_back({.name="transfer", .label="Target transfer / EOTF", .group="Color Transform", .kind=ParamKind::Enum, .defaultValue=std::string{"source"}, .enumValues={{"source","Source"},{"srgb","sRGB"},{"bt709","BT.709"},{"linear","Linear"},{"pq","PQ (H.273 16)"},{"hlg","HLG (H.273 18)"}}, .help="Explicit H.273 transfer characteristics. PQ is defined by H.273 for 10-, 12-, 14-, and 16-bit systems."});
	if (policy.matrix && !has("matrix")) params.push_back({.name="matrix", .label="Target matrix", .group="Color Transform", .kind=ParamKind::Enum, .defaultValue=std::string{"source"}, .enumValues={{"source","Source"},{"bt709","BT.709"},{"bt2020nc","BT.2020 non-constant"}}, .help="Explicit H.273 matrix coefficients used for the actual RGB/Y'CbCr conversion and bitstream signaling."});
	if (policy.range && !has("range")) params.push_back({.name="range", .label="Signal range", .group="Color Transform", .kind=ParamKind::Enum, .defaultValue=std::string{"source"}, .enumValues={{"source","Source"},{"limited","Limited"},{"full","Full"}}, .help="Explicit full or limited range. Limited-range code values follow the bit-depth-generic H.273 equations."});
	if (policy.toneMapping && !has("tone-map")) params.push_back({.name="tone-map", .label="Tone / gamut mapping", .group="Color Transform", .kind=ParamKind::Enum, .defaultValue=std::string{"none"}, .enumValues={{"none","None (reject narrowing)"},{"clip","Explicit clip"},{"reinhard","Reinhard"}}, .help="Required when converting HDR to SDR or a wider gamut to BT.709. None rejects the conversion instead of applying an implicit mapping."});
	if (policy.toneMapping && !has("source-peak-nits")) params.push_back({.name="source-peak-nits", .label="Source peak luminance", .group="Color Transform", .kind=ParamKind::Float, .defaultValue=1000.0, .floatRange=FloatRange{80.0,10000.0,10.0}, .help="Explicit source peak luminance used by tone and gamut mapping, in cd/m²."});
	if (policy.toneMapping && !has("target-peak-nits")) params.push_back({.name="target-peak-nits", .label="Target peak luminance", .group="Color Transform", .kind=ParamKind::Float, .defaultValue=203.0, .floatRange=FloatRange{80.0,10000.0,10.0}, .help="Explicit target peak luminance used by tone and gamut mapping, in cd/m²."});
}

PixelFormat output_format(PixelFormat source, int depth, std::string_view chroma) {
	if (chroma == "source") {
		if (is_gray(source)) chroma = "400";
		else if (is_420(source)) chroma = "420";
		else if (is_422(source)) chroma = "422";
		else chroma = "444";
	}
	if (depth != 8 && depth != 10 && depth != 12 && depth != 14) throw std::invalid_argument("output bit depth must be 8, 10, 12, or 14");
	if (chroma == "420") return depth == 8 ? PixelFormat::YUV420P8 : depth == 10 ? PixelFormat::YUV420P10LE : depth == 12 ? PixelFormat::YUV420P12LE : PixelFormat::YUV420P14LE;
	if (chroma == "422") return depth == 8 ? PixelFormat::YUV422P8 : depth == 10 ? PixelFormat::YUV422P10LE : depth == 12 ? PixelFormat::YUV422P12LE : PixelFormat::YUV422P14LE;
	if (chroma == "444") return depth == 8 ? PixelFormat::YUV444P8 : depth == 10 ? PixelFormat::YUV444P10LE : depth == 12 ? PixelFormat::YUV444P12LE : PixelFormat::YUV444P14LE;
	if (chroma == "400") return depth == 8 ? PixelFormat::Gray8 : depth == 10 ? PixelFormat::Gray10LE : depth == 12 ? PixelFormat::Gray12LE : PixelFormat::Gray14LE;
	throw std::invalid_argument("unsupported chroma subsampling: " + std::string{chroma});
}

} // namespace
} // namespace codec_gui::gui
