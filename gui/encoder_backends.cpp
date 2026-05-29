#include "encoder_backends.hpp"
#include "preview_decoders.hpp"

#include <algorithm>
#include <fcntl.h>
#include <sstream>
#include <stdexcept>
#include <type_traits>
#include <unistd.h>

extern "C" {
#include <va/va.h>
#include <va/va_drm.h>
}

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
CapabilityResult query_software_capabilities(std::string implementation) {
	CapabilityResult result;
	result.snapshot.implementation = std::move(implementation);
	result.snapshot.available = true;
	result.params = QueryFn();
	result.snapshot.details = capability_details(result.params);
	return result;
}

template <auto QueryFn>
CapabilityResult query_hardware_capabilities(std::string implementation) {
	CapabilityResult result;
	try {
		result.params = QueryFn();
		result.snapshot.implementation = std::move(implementation);
		result.snapshot.available = true;
		result.snapshot.details = capability_details(result.params);
	} catch (const std::exception& e) {
		result.snapshot.implementation = std::move(implementation);
		result.snapshot.available = false;
		result.snapshot.error = e.what();
	}
	return result;
}

CapabilityResult query_x265_backend() {
	return query_software_capabilities<query_x265_parameters>("x265");
}

CapabilityResult query_vvenc_backend() {
	return query_software_capabilities<query_vvenc_parameters>("VVenC");
}

CapabilityResult query_svt_av1_backend() {
	return query_software_capabilities<query_svt_av1_parameters>("SVT-AV1");
}

CapabilityResult query_uvg266_backend() {
	return query_software_capabilities<query_uvg266_parameters>("uvg266");
}

CapabilityResult query_av2_backend() {
	return query_software_capabilities<query_av2_parameters>("AVM AV2");
}

CapabilityResult query_jpegls_backend() {
	return query_software_capabilities<query_jpegls_parameters>("CharLS JPEG-LS");
}

CapabilityResult query_jpeg_backend() {
	return query_software_capabilities<query_jpeg_parameters>("libjpeg");
}

CapabilityResult query_jpeg2000_backend() {
	return query_software_capabilities<query_jpeg2000_parameters>("OpenJPEG");
}

CapabilityResult query_jpegxl_backend() {
	return query_software_capabilities<query_jpegxl_parameters>("libjxl");
}

CapabilityResult query_jpegxr_backend() {
	return query_software_capabilities<query_jpegxr_parameters>("jxrlib");
}

CapabilityResult query_png_backend() {
	return query_software_capabilities<query_png_parameters>("libpng");
}

CapabilityResult query_x264_backend() {
	return query_software_capabilities<query_x264_parameters>("x264");
}

std::string vaapi_hevc_backend_name() {
	const char* device = "/dev/dri/renderD128";
	const int fd = open(device, O_RDWR | O_CLOEXEC);
	if (fd < 0) {
		return "VA-API HEVC";
	}
	struct FdGuard {
		int fd = -1;
		~FdGuard() {
			if (fd >= 0) close(fd);
		}
	} fdGuard{fd};

	VADisplay dpy = vaGetDisplayDRM(fd);
	if (dpy == nullptr) {
		return "VA-API HEVC";
	}
	int major = 0;
	int minor = 0;
	if (vaInitialize(dpy, &major, &minor) != VA_STATUS_SUCCESS) {
		return "VA-API HEVC";
	}
	struct DisplayGuard {
		VADisplay dpy = nullptr;
		~DisplayGuard() {
			if (dpy != nullptr) vaTerminate(dpy);
		}
	} displayGuard{dpy};

	const char* vendor = vaQueryVendorString(dpy);
	if (vendor == nullptr || vendor[0] == '\0') {
		return "VA-API HEVC";
	}
	return std::string{"VA-API HEVC - "} + vendor;
}

bool vaapi_render_node_available() {
	const char* device = "/dev/dri/renderD128";
	const int fd = open(device, O_RDWR | O_CLOEXEC);
	if (fd < 0) {
		return false;
	}
	close(fd);
	return true;
}

CapabilityResult query_vaapi_hevc_backend() {
	if (!vaapi_render_node_available()) {
		CapabilityResult result;
		result.snapshot.implementation = "VA-API HEVC";
		result.snapshot.available = false;
		result.snapshot.error = "VA-API render node is not available";
		return result;
	}
	return query_hardware_capabilities<query_vaapi_hevc_parameters>(vaapi_hevc_backend_name());
}

CapabilityResult query_vaapi_av1_backend() {
	if (!vaapi_render_node_available()) {
		CapabilityResult result;
		result.snapshot.implementation = "VA-API AV1";
		result.snapshot.available = false;
		result.snapshot.error = "VA-API render node is not available";
		return result;
	}
	return query_hardware_capabilities<query_vaapi_av1_parameters>("VA-API AV1");
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

std::vector<EncoderBackend> initial_encoder_backends() {
	return {
		{X265_HEVC, "x265 HEVC", "HEVC", BackendKind::Software, query_x265_backend, encode_x265_still_image, decode_hevc_preview, summarize_generic},
		{VVENC_VVC, "VVenC VVC", "VVC", BackendKind::Software, query_vvenc_backend, encode_vvenc_still_image, decode_vvc_preview, summarize_generic},
		{SVT_AV1, "SVT-AV1", "AV1", BackendKind::Software, query_svt_av1_backend, encode_svt_av1_still_image, decode_av1_preview, summarize_generic},
		{VAAPI_HEVC, vaapi_hevc_backend_name(), "HEVC", BackendKind::Hardware, query_vaapi_hevc_backend, encode_vaapi_hevc_still_image, decode_hevc_preview, summarize_generic},
		{VAAPI_AV1, "VA-API AV1", "AV1", BackendKind::Hardware, query_vaapi_av1_backend, encode_vaapi_av1_still_image, decode_av1_preview, summarize_generic},
		{UVG266_VVC, "uvg266 VVC", "VVC", BackendKind::Software, query_uvg266_backend, encode_uvg266_still_image, decode_vvc_preview, summarize_generic},
		{AV2, "AVM AV2", "AV2", BackendKind::Software, query_av2_backend, encode_av2_still_image, no_preview_decode, summarize_generic},
		{JPEGLS, "CharLS JPEG-LS", "JPEG-LS", BackendKind::Software, query_jpegls_backend, encode_jpegls_still_image, decode_jpegls_preview, summarize_generic},
		{JPEG, "libjpeg JPEG", "JPEG", BackendKind::Software, query_jpeg_backend, encode_jpeg_still_image, decode_jpeg_preview, summarize_generic},
		{JPEG2000, "OpenJPEG JPEG 2000", "JPEG 2000", BackendKind::Software, query_jpeg2000_backend, encode_jpeg2000_still_image, decode_jpeg2000_preview, summarize_generic},
		{JPEGXL, "libjxl JPEG XL", "JPEG XL", BackendKind::Software, query_jpegxl_backend, encode_jpegxl_still_image, decode_jpegxl_preview, summarize_generic},
		{JPEGXR, "jxrlib JPEG XR", "JPEG XR", BackendKind::Software, query_jpegxr_backend, encode_jpegxr_still_image, decode_jpegxr_preview, summarize_generic},
		{PNG, "libpng PNG", "PNG", BackendKind::Software, query_png_backend, encode_png_still_image, decode_png_preview, summarize_generic},
		{X264_H264_INTRA, "x264 H.264 Intra", "H.264", BackendKind::Software, query_x264_backend, encode_x264_intra_still_image, decode_h264_preview, summarize_generic},
	};
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
	EncodeResult result;
	const auto start = std::chrono::steady_clock::now();
	result.encoded = backend.encode(image, params);
	const auto finish = std::chrono::steady_clock::now();
	result.metadata = metadata_for_backend(backend, result.encoded, params);
	result.metadata.encodeSeconds = std::chrono::duration<double>(finish - start).count();
	return result;
}

} // namespace codec_gui::gui
