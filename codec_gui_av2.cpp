// codec_gui_av2.cpp
#include "codec_gui_x265.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

extern "C" {
#include <avm/avm_codec.h>
#include <avm/avm_encoder.h>
#include <avm/avm_image.h>
#include <avm/avmcx.h>
}

namespace codec_gui {
namespace {

struct FormatInfo {
	avm_img_fmt_t imageFormat;
	avm_bit_depth_t bitDepth;
	int planeCount;
	int widthDiv[3];
	int heightDiv[3];
	unsigned int profile;
	bool monochrome;
};

[[nodiscard]] int ceil_div(const int value, const int divisor) {
	return (value + divisor - 1) / divisor;
}

[[nodiscard]] FormatInfo format_info(const PixelFormat format) {
	switch (format) {
		case PixelFormat::YUV420P8:
			return {AVM_IMG_FMT_I420, AVM_BITS_8, 3, {1, 2, 2}, {1, 2, 2}, 0, false};
			case PixelFormat::YUV420P10LE:
				return {AVM_IMG_FMT_I42016, AVM_BITS_10, 3, {1, 2, 2}, {1, 2, 2}, 0, false};
			case PixelFormat::YUV422P8:
				return {AVM_IMG_FMT_I422, AVM_BITS_8, 3, {1, 2, 2}, {1, 1, 1}, 1, false};
			case PixelFormat::YUV422P10LE:
				return {AVM_IMG_FMT_I42216, AVM_BITS_10, 3, {1, 2, 2}, {1, 1, 1}, 2, false};
			case PixelFormat::YUV444P8:
			return {AVM_IMG_FMT_I444, AVM_BITS_8, 3, {1, 1, 1}, {1, 1, 1}, 1, false};
		case PixelFormat::YUV444P10LE:
			return {AVM_IMG_FMT_I44416, AVM_BITS_10, 3, {1, 1, 1}, {1, 1, 1}, 2, false};
		case PixelFormat::Gray8:
			return {AVM_IMG_FMT_I420, AVM_BITS_8, 1, {1, 1, 1}, {1, 1, 1}, 0, true};
		case PixelFormat::Gray10LE:
			return {AVM_IMG_FMT_I42016, AVM_BITS_10, 1, {1, 1, 1}, {1, 1, 1}, 0, true};
		case PixelFormat::YUV420P12LE: case PixelFormat::YUV420P14LE:
		case PixelFormat::YUV422P12LE: case PixelFormat::YUV422P14LE:
		case PixelFormat::YUV444P12LE: case PixelFormat::YUV444P14LE:
		case PixelFormat::Gray12LE: case PixelFormat::Gray14LE: break;
		default: break;
	}

	throw std::runtime_error("unsupported pixel format");
}

void validate_image(const RawImage& image, const FormatInfo& info) {
	if (image.width <= 0 || image.height <= 0) {
		throw std::runtime_error("image dimensions must be positive");
	}

	if (!info.monochrome && info.imageFormat != AVM_IMG_FMT_I444 && info.imageFormat != AVM_IMG_FMT_I44416 &&
	    ((image.width & 1) != 0 || (image.height & 1) != 0)) {
		throw std::runtime_error("YUV420 images require even width and height");
	}

	const int bytesPerSample = info.bitDepth == AVM_BITS_8 ? 1 : 2;
	for (int plane = 0; plane < info.planeCount; ++plane) {
		const int planeWidth  = ceil_div(image.width, info.widthDiv[plane]);
		const int planeHeight = ceil_div(image.height, info.heightDiv[plane]);
		const int minStride   = planeWidth * bytesPerSample;

		if (image.planes[plane].strideBytes < minStride) {
			throw std::runtime_error("plane stride is smaller than the required row size");
		}

		const auto requiredSize =
			static_cast<std::size_t>(image.planes[plane].strideBytes) *
			static_cast<std::size_t>(planeHeight);
		if (image.planes[plane].bytes.size() < requiredSize) {
			throw std::runtime_error("plane byte buffer is smaller than stride * height");
		}
	}
}

[[nodiscard]] std::string param_value_to_string(const ParamValue& value) {
	return std::visit(
		[](const auto& v) -> std::string {
			using T = std::decay_t<decltype(v)>;
			if constexpr (std::is_same_v<T, bool>) {
				return v ? "1" : "0";
			} else if constexpr (std::is_same_v<T, int64_t>) {
				return std::to_string(v);
			} else if constexpr (std::is_same_v<T, double>) {
				std::ostringstream oss;
				oss.precision(17);
				oss << v;
				return oss.str();
			} else {
				return v;
			}
		},
		value
	);
}

void throw_codec_error(avm_codec_ctx_t& ctx, const std::string& operation) {
	const char* detail = avm_codec_error_detail(&ctx);
	const char* error = avm_codec_error(&ctx);
	std::string message = "AV2 " + operation + " failed";
	if (error != nullptr && error[0] != '\0') {
		message += ": ";
		message += error;
	}
	if (detail != nullptr && detail[0] != '\0') {
		message += " (";
		message += detail;
		message += ")";
	}
	throw std::runtime_error(message);
}

void checked_codec_call(avm_codec_ctx_t& ctx, avm_codec_err_t error, const std::string& operation) {
	if (error != AVM_CODEC_OK) {
		throw_codec_error(ctx, operation);
	}
}

void copy_image_to_avm(const RawImage& image, const FormatInfo& info, avm_image_t& dst) {
	const int bytesPerSample = info.bitDepth == AVM_BITS_8 ? 1 : 2;
	for (int plane = 0; plane < info.planeCount; ++plane) {
		const int planeWidth  = ceil_div(image.width, info.widthDiv[plane]);
		const int planeHeight = ceil_div(image.height, info.heightDiv[plane]);
		for (int y = 0; y < planeHeight; ++y) {
			const uint8_t* srcRow =
				image.planes[plane].bytes.data() + static_cast<std::size_t>(y) * image.planes[plane].strideBytes;
			uint8_t* dstRow = dst.planes[plane] + static_cast<std::size_t>(y) * static_cast<std::size_t>(dst.stride[plane]);
			std::memcpy(dstRow, srcRow, static_cast<std::size_t>(planeWidth) * static_cast<std::size_t>(bytesPerSample));
		}
	}
}

void put_u16le(std::vector<std::byte>& out, const uint16_t value) {
	out.push_back(std::byte(value & 0xffu));
	out.push_back(std::byte(value >> 8u));
}

void put_u32le(std::vector<std::byte>& out, const uint32_t value) {
	for (int shift = 0; shift < 32; shift += 8) {
		out.push_back(std::byte((value >> shift) & 0xffu));
	}
}

void put_u64le(std::vector<std::byte>& out, const uint64_t value) {
	for (int shift = 0; shift < 64; shift += 8) {
		out.push_back(std::byte((value >> shift) & 0xffu));
	}
}

void append_ivf_header(std::vector<std::byte>& out, const int width, const int height, const uint32_t frameCount) {
	const char signature[] = {'D', 'K', 'I', 'F'};
	for (const char c : signature) {
		out.push_back(std::byte(c));
	}
	put_u16le(out, 0);
	put_u16le(out, 32);
	const char fourcc[] = {'A', 'V', '0', '2'};
	for (const char c : fourcc) {
		out.push_back(std::byte(c));
	}
	put_u16le(out, static_cast<uint16_t>(width));
	put_u16le(out, static_cast<uint16_t>(height));
	put_u32le(out, 1);
	put_u32le(out, 1);
	put_u32le(out, frameCount);
	put_u32le(out, 0);
}

void append_ivf_frame(std::vector<std::byte>& out, const std::vector<std::byte>& frame, const uint64_t pts) {
	put_u32le(out, static_cast<uint32_t>(frame.size()));
	put_u64le(out, pts);
	out.insert(out.end(), frame.begin(), frame.end());
}

[[nodiscard]] EncoderParamInfo bool_param(
	std::string name,
	std::string label,
	std::string group,
	bool        defaultValue,
	std::string help
) {
	EncoderParamInfo p;
	p.name         = std::move(name);
	p.label        = std::move(label);
	p.group        = std::move(group);
	p.kind         = ParamKind::Bool;
	p.defaultValue = defaultValue;
	p.help         = std::move(help);
	return p;
}

[[nodiscard]] EncoderParamInfo int_param(
	std::string name,
	std::string label,
	std::string group,
	int64_t     defaultValue,
	IntRange    range,
	std::string help
) {
	EncoderParamInfo p;
	p.name         = std::move(name);
	p.label        = std::move(label);
	p.group        = std::move(group);
	p.kind         = ParamKind::Int;
	p.defaultValue = defaultValue;
	p.intRange     = range;
	p.help         = std::move(help);
	return p;
}

} // namespace

std::vector<EncoderParamInfo> query_av2_parameters() {
	return {
		int_param("cpu-used", "CPU used", "Speed / Quality", 9, {0, 9, 1}, "AVM encoder speed level."),
		int_param("qp", "QP", "Rate Control", 128, {0, 255, 1}, "AV2 quantizer used in constant-quality mode."),
		bool_param("lossless", "Lossless", "Rate Control", false, "Enable lossless coding."),
		bool_param("enable-cdef", "CDEF", "Filters", true, "Enable constrained directional enhancement filtering."),
		bool_param("enable-restoration", "Loop restoration", "Filters", true, "Enable loop restoration filtering."),
		bool_param("enable-deblocking", "Deblocking", "Filters", true, "Enable deblocking filtering."),
		bool_param("enable-intrabc", "IntraBC", "Tools", false, "Enable intra block copy screen-content search."),
		bool_param("enable-rect-partitions", "Rect partitions", "Tools", false, "Enable rectangular partition search."),
		bool_param("enable-1to4-partitions", "1:4 partitions", "Tools", false, "Enable 1:4 and 4:1 partition search."),
		int_param("tile-columns", "Tile columns", "Execution", 0, {0, 6, 1}, "Log2 tile column count."),
		int_param("tile-rows", "Tile rows", "Execution", 0, {0, 6, 1}, "Log2 tile row count."),
	};
}

EncodedImage encode_av2_still_image(const RawImage& image, std::span<const EncoderParam> params) {
	const FormatInfo info = format_info(image.format);
	validate_image(image, info);

	avm_codec_iface_t* iface = avm_codec_av2_cx();
	if (iface == nullptr) {
		throw std::runtime_error("AVM AV2 encoder interface is not available");
	}

	avm_codec_enc_cfg_t cfg{};
	avm_codec_err_t err = avm_codec_enc_config_default(iface, &cfg, AVM_USAGE_GOOD_QUALITY);
	if (err != AVM_CODEC_OK) {
		throw std::runtime_error("AV2 default encoder configuration failed");
	}

	cfg.g_w                      = static_cast<unsigned int>(image.width);
	cfg.g_h                      = static_cast<unsigned int>(image.height);
	cfg.g_forced_max_frame_width = static_cast<unsigned int>(image.width);
	cfg.g_forced_max_frame_height = static_cast<unsigned int>(image.height);
	cfg.g_profile                = info.profile;
	cfg.g_bit_depth              = info.bitDepth;
	cfg.g_input_bit_depth        = static_cast<unsigned int>(info.bitDepth);
	cfg.g_timebase               = {1, 1};
	cfg.g_limit                  = 1;
	cfg.g_lag_in_frames          = 0;
	cfg.rc_end_usage             = AVM_Q;
	cfg.rc_min_quantizer         = 0;
	cfg.rc_max_quantizer         = 255;
	cfg.kf_mode                  = AVM_KF_DISABLED;
	cfg.kf_min_dist              = 0;
	cfg.kf_max_dist              = 0;
	cfg.monochrome               = info.monochrome ? 1u : 0u;
	cfg.full_still_picture_hdr   = 1;

	avm_image_t raw{};
	if (avm_img_alloc(&raw, info.imageFormat, static_cast<unsigned int>(image.width), static_cast<unsigned int>(image.height), 1) == nullptr) {
		throw std::runtime_error("AV2 image allocation failed");
	}
	struct ImageGuard {
		avm_image_t* image = nullptr;
		~ImageGuard() {
			if (image != nullptr) {
				avm_img_free(image);
			}
		}
	} imageGuard{&raw};

	raw.cp         = static_cast<avm_color_primaries_t>(image.color.primaries);
	raw.tc         = static_cast<avm_transfer_characteristics_t>(image.color.transfer);
	raw.mc         = static_cast<avm_matrix_coefficients_t>(image.color.matrix);
	raw.range      = image.color.range == ColorRange::Full ? AVM_CR_FULL_RANGE : AVM_CR_STUDIO_RANGE;
	raw.csp        = static_cast<avm_chroma_sample_position_t>(image.color.chroma420Location.value_or(Chroma420SampleLocation::LeftCenter));
	raw.monochrome = info.monochrome ? 1 : 0;

	copy_image_to_avm(image, info, raw);

	avm_codec_ctx_t ctx{};
	checked_codec_call(ctx, avm_codec_enc_init(&ctx, iface, &cfg, 0), "init");
	struct CodecGuard {
		avm_codec_ctx_t* ctx = nullptr;
		~CodecGuard() {
			if (ctx != nullptr) {
				(void)avm_codec_destroy(ctx);
			}
		}
	} codecGuard{&ctx};

	checked_codec_call(ctx, avm_codec_control(&ctx, AV2E_SET_COLOR_PRIMARIES, static_cast<int>(image.color.primaries)), "set color primaries");
	checked_codec_call(ctx, avm_codec_control(&ctx, AV2E_SET_TRANSFER_CHARACTERISTICS, static_cast<int>(image.color.transfer)), "set transfer");
	checked_codec_call(ctx, avm_codec_control(&ctx, AV2E_SET_MATRIX_COEFFICIENTS, static_cast<int>(image.color.matrix)), "set matrix");
	checked_codec_call(ctx, avm_codec_control(&ctx, AV2E_SET_COLOR_RANGE, image.color.range == ColorRange::Full ? AVM_CR_FULL_RANGE : AVM_CR_STUDIO_RANGE), "set range");
	checked_codec_call(ctx, avm_codec_control(&ctx, AV2E_SET_CHROMA_SAMPLE_POSITION, static_cast<int>(image.color.chroma420Location.value_or(Chroma420SampleLocation::LeftCenter))), "set chroma sample position");
	checked_codec_call(ctx, avm_codec_control(&ctx, AV2E_SET_ENABLE_INTRABC, 0), "disable intrabc");
	checked_codec_call(ctx, avm_codec_control(&ctx, AV2E_SET_ENABLE_RECT_PARTITIONS, 0), "disable rect partitions");
	checked_codec_call(ctx, avm_codec_control(&ctx, AV2E_SET_ENABLE_1TO4_PARTITIONS, 0), "disable 1:4 partitions");
	checked_codec_call(ctx, avm_codec_control(&ctx, AV2E_SET_MIN_PARTITION_SIZE, 64), "set min partition size");
	checked_codec_call(ctx, avm_codec_control(&ctx, AV2E_SET_MAX_PARTITION_SIZE, 128), "set max partition size");

	for (const EncoderParam& param : params) {
		const std::string value = param_value_to_string(param.value);
		if (param.name == "cpu-used") {
			checked_codec_call(ctx, avm_codec_control(&ctx, AVME_SET_CPUUSED, static_cast<int>(std::get<int64_t>(param.value))), "set cpu-used");
		} else if (param.name == "qp") {
			checked_codec_call(ctx, avm_codec_control(&ctx, AVME_SET_QP, static_cast<unsigned int>(std::get<int64_t>(param.value))), "set qp");
		} else if (param.name == "lossless") {
			checked_codec_call(ctx, avm_codec_control(&ctx, AV2E_SET_LOSSLESS, std::get<bool>(param.value) ? 1u : 0u), "set lossless");
		} else if (param.name == "enable-cdef") {
			checked_codec_call(ctx, avm_codec_control(&ctx, AV2E_SET_ENABLE_CDEF, std::get<bool>(param.value) ? 1u : 0u), "set cdef");
		} else if (param.name == "enable-restoration") {
			checked_codec_call(ctx, avm_codec_control(&ctx, AV2E_SET_ENABLE_RESTORATION, std::get<bool>(param.value) ? 1u : 0u), "set restoration");
		} else if (param.name == "enable-deblocking") {
			checked_codec_call(ctx, avm_codec_control(&ctx, AV2E_SET_ENABLE_DEBLOCKING, std::get<bool>(param.value) ? 1u : 0u), "set deblocking");
		} else if (param.name == "enable-intrabc") {
			checked_codec_call(ctx, avm_codec_control(&ctx, AV2E_SET_ENABLE_INTRABC, std::get<bool>(param.value) ? 1 : 0), "set intrabc");
		} else if (param.name == "enable-rect-partitions") {
			checked_codec_call(ctx, avm_codec_control(&ctx, AV2E_SET_ENABLE_RECT_PARTITIONS, std::get<bool>(param.value) ? 1 : 0), "set rect partitions");
		} else if (param.name == "enable-1to4-partitions") {
			checked_codec_call(ctx, avm_codec_control(&ctx, AV2E_SET_ENABLE_1TO4_PARTITIONS, std::get<bool>(param.value) ? 1 : 0), "set 1:4 partitions");
		} else if (param.name == "min-partition-size") {
			checked_codec_call(ctx, avm_codec_control(&ctx, AV2E_SET_MIN_PARTITION_SIZE, static_cast<int>(std::get<int64_t>(param.value))), "set min partition size");
		} else if (param.name == "max-partition-size") {
			checked_codec_call(ctx, avm_codec_control(&ctx, AV2E_SET_MAX_PARTITION_SIZE, static_cast<int>(std::get<int64_t>(param.value))), "set max partition size");
		} else if (param.name == "tile-columns") {
			checked_codec_call(ctx, avm_codec_control(&ctx, AV2E_SET_TILE_COLUMNS, static_cast<unsigned int>(std::get<int64_t>(param.value))), "set tile columns");
		} else if (param.name == "tile-rows") {
			checked_codec_call(ctx, avm_codec_control(&ctx, AV2E_SET_TILE_ROWS, static_cast<unsigned int>(std::get<int64_t>(param.value))), "set tile rows");
		} else {
			checked_codec_call(ctx, avm_codec_set_option(&ctx, param.name.c_str(), value.c_str()), "set option " + param.name);
		}
	}

	std::vector<std::vector<std::byte>> frames;

	auto collect_packets = [&]() {
		avm_codec_iter_t iter = nullptr;
		while (const avm_codec_cx_pkt_t* pkt = avm_codec_get_cx_data(&ctx, &iter)) {
			if (pkt->kind == AVM_CODEC_CX_FRAME_PKT || pkt->kind == AVM_CODEC_CX_FRAME_NULL_PKT) {
				const auto* first = reinterpret_cast<const std::byte*>(pkt->data.frame.buf);
				frames.emplace_back(first, first + pkt->data.frame.sz);
			}
		}
	};

	checked_codec_call(ctx, avm_codec_encode(&ctx, &raw, 0, 1, AVM_EFLAG_FORCE_KF), "encode");
	collect_packets();

	while (true) {
		const std::size_t before = frames.size();
		checked_codec_call(ctx, avm_codec_encode(&ctx, nullptr, -1, 0, 0), "flush");
		collect_packets();
		if (frames.size() == before) {
			break;
		}
	}

	if (frames.empty()) {
		throw std::runtime_error("AV2 produced an empty bitstream");
	}

	EncodedImage result;
	append_ivf_header(result.hevcAnnexB, image.width, image.height, static_cast<uint32_t>(frames.size()));
	for (std::size_t i = 0; i < frames.size(); ++i) {
		append_ivf_frame(result.hevcAnnexB, frames[i], i);
	}

	return result;
}

} // namespace codec_gui
