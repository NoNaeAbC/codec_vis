// codec_gui_svt_av1.cpp
#include "codec_gui_x265.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

extern "C" {
#include <EbSvtAv1Enc.h>
}

namespace codec_gui {
namespace {

struct SvtEncoderDeleter {
	bool initialized = false;

	void operator()(EbComponentType* encoder) const noexcept {
		if (encoder == nullptr) {
			return;
		}

		if (initialized) {
			(void)svt_av1_enc_deinit(encoder);
		}
		(void)svt_av1_enc_deinit_handle(encoder);
	}
};

struct SvtOutputDeleter {
	void operator()(EbBufferHeaderType* buffer) const noexcept {
		if (buffer != nullptr) {
			svt_av1_enc_release_out_buffer(&buffer);
		}
	}
};

struct SvtHeaderDeleter {
	void operator()(EbBufferHeaderType* buffer) const noexcept {
		if (buffer != nullptr) {
			(void)svt_av1_enc_stream_header_release(buffer);
		}
	}
};

using EncoderPtr = std::unique_ptr<EbComponentType, SvtEncoderDeleter>;
using OutputPtr  = std::unique_ptr<EbBufferHeaderType, SvtOutputDeleter>;
using HeaderPtr  = std::unique_ptr<EbBufferHeaderType, SvtHeaderDeleter>;

struct FormatInfo {
	EbColorFormat     colorFormat;
	EbAv1SeqProfile   profile;
	uint32_t          bitDepth;
	int               planeCount;
	std::array<int, 3> widthDiv;
	std::array<int, 3> heightDiv;
};

[[nodiscard]] int ceil_div(const int value, const int divisor) {
	return (value + divisor - 1) / divisor;
}

[[nodiscard]] FormatInfo format_info(const PixelFormat format) {
	switch (format) {
		case PixelFormat::YUV420P8:
			return {EB_YUV420, MAIN_PROFILE, 8, 3, {1, 2, 2}, {1, 2, 2}};
			case PixelFormat::YUV420P10LE:
				return {EB_YUV420, MAIN_PROFILE, 10, 3, {1, 2, 2}, {1, 2, 2}};
			case PixelFormat::YUV422P8:
				return {EB_YUV422, PROFESSIONAL_PROFILE, 8, 3, {1, 2, 2}, {1, 1, 1}};
			case PixelFormat::YUV422P10LE:
				return {EB_YUV422, PROFESSIONAL_PROFILE, 10, 3, {1, 2, 2}, {1, 1, 1}};
			case PixelFormat::YUV444P8:
			return {EB_YUV444, HIGH_PROFILE, 8, 3, {1, 1, 1}, {1, 1, 1}};
		case PixelFormat::YUV444P10LE:
			return {EB_YUV444, PROFESSIONAL_PROFILE, 10, 3, {1, 1, 1}, {1, 1, 1}};
		case PixelFormat::Gray8:
			return {EB_YUV400, MAIN_PROFILE, 8, 1, {1, 1, 1}, {1, 1, 1}};
		case PixelFormat::Gray10LE:
			return {EB_YUV400, MAIN_PROFILE, 10, 1, {1, 1, 1}, {1, 1, 1}};
		case PixelFormat::YUV420P12LE: case PixelFormat::YUV420P14LE:
		case PixelFormat::YUV422P12LE: case PixelFormat::YUV422P14LE:
		case PixelFormat::YUV444P12LE: case PixelFormat::YUV444P14LE:
		case PixelFormat::Gray12LE: case PixelFormat::Gray14LE: break;
		default: break;
	}

	throw std::runtime_error("unsupported pixel format");
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

void checked_svt_call(const EbErrorType error, std::string_view operation) {
	if (error == EB_ErrorNone) {
		return;
	}

	std::ostringstream oss;
	oss << "SVT-AV1 " << operation << " failed with error 0x" << std::hex
	    << static_cast<int32_t>(error);
	throw std::runtime_error(oss.str());
}

void discard_svt_log(void*, SvtAv1LogLevel, const char*, const char*, va_list) {}

void validate_image(const RawImage& image, const FormatInfo& info) {
	if (image.width <= 0 || image.height <= 0) {
		throw std::runtime_error("image dimensions must be positive");
	}

	if (info.colorFormat == EB_YUV420 && ((image.width & 1) != 0 || (image.height & 1) != 0)) {
		throw std::runtime_error("YUV420 images require even width and height");
	}

	const int bytesPerSample = info.bitDepth <= 8 ? 1 : 2;

	for (int plane = 0; plane < info.planeCount; ++plane) {
		const int planeWidth  = ceil_div(image.width, info.widthDiv[plane]);
		const int planeHeight = ceil_div(image.height, info.heightDiv[plane]);
		const int minStride   = planeWidth * bytesPerSample;

		const ImagePlane& src = image.planes[plane];
		if (src.strideBytes < minStride) {
			throw std::runtime_error("plane stride is smaller than the required row size");
		}

		const auto requiredSize =
			static_cast<std::size_t>(src.strideBytes) * static_cast<std::size_t>(planeHeight);
		if (src.bytes.size() < requiredSize) {
			throw std::runtime_error("plane byte buffer is smaller than stride * height");
		}
	}
}

void append_buffer(std::vector<std::byte>& out, const EbBufferHeaderType& buffer) {
	if (buffer.p_buffer == nullptr || buffer.n_filled_len == 0) {
		return;
	}

	const auto* first = reinterpret_cast<const std::byte*>(buffer.p_buffer);
	out.insert(out.end(), first, first + buffer.n_filled_len);
}

void append_temporal_delimiter(std::vector<std::byte>& out) {
	out.push_back(std::byte{0x12});
	out.push_back(std::byte{0x00});
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
	const char fourcc[] = {'A', 'V', '0', '1'};
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

void parse_or_throw(EbSvtAv1EncConfiguration& cfg, std::string_view name, std::string_view value) {
	const std::string nameString{name};
	const std::string valueString{value};
	const EbErrorType error = svt_av1_enc_parse_parameter(&cfg, nameString.c_str(), valueString.c_str());

	if (error != EB_ErrorNone) {
		throw std::runtime_error(
			"invalid SVT-AV1 parameter '" + nameString + "' value '" + valueString + "'"
		);
	}
}

[[nodiscard]] bool is_structural_param(std::string_view name) {
	static constexpr std::array<std::string_view, 21> structural = {
		"width",
		"height",
		"forced-max-frame-width",
		"forced-max-frame-height",
		"frames",
		"fps-num",
		"fps-denom",
		"input-depth",
		"color-format",
		"profile",
		"keyint",
		"irefresh-type",
		"hierarchical-levels",
		"pred-struct",
		"lookahead",
		"scd",
		"passes",
		"pass",
		"avif",
		"inj",
		"nb",
	};

	return std::ranges::find(structural, name) != structural.end();
}

void force_still_image_config(EbSvtAv1EncConfiguration& cfg, const RawImage& image, const FormatInfo& info) {
	cfg.source_width           = static_cast<uint32_t>(image.width);
	cfg.source_height          = static_cast<uint32_t>(image.height);
	cfg.forced_max_frame_width = static_cast<uint32_t>(image.width);
	cfg.forced_max_frame_height = static_cast<uint32_t>(image.height);
	cfg.frame_rate_numerator   = 1;
	cfg.frame_rate_denominator = 1;
	cfg.encoder_bit_depth      = info.bitDepth;
	cfg.encoder_color_format   = info.colorFormat;
	cfg.profile                = info.profile;
	cfg.intra_period_length    = 0;
	cfg.intra_refresh_type     = SVT_AV1_KF_REFRESH;
	cfg.hierarchical_levels    = 2;
	cfg.pred_structure         = ALL_INTRA;
	cfg.look_ahead_distance    = 0;
	cfg.scene_change_detection = 0;
	cfg.pass                   = 0;
	cfg.avif                   = true;
	cfg.color_primaries        = static_cast<EbColorPrimaries>(image.color.primaries);
	cfg.transfer_characteristics = static_cast<EbTransferCharacteristics>(image.color.transfer);
	cfg.matrix_coefficients    = static_cast<EbMatrixCoefficients>(image.color.matrix);
	cfg.color_range            = image.color.range == ColorRange::Full ? EB_CR_FULL_RANGE : EB_CR_STUDIO_RANGE;
	cfg.chroma_sample_position = static_cast<EbChromaSamplePosition>(image.color.chroma420Location.value_or(Chroma420SampleLocation::LeftCenter));
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

[[nodiscard]] EncoderParamInfo float_param(
	std::string name,
	std::string label,
	std::string group,
	double      defaultValue,
	FloatRange  range,
	std::string help
) {
	EncoderParamInfo p;
	p.name         = std::move(name);
	p.label        = std::move(label);
	p.group        = std::move(group);
	p.kind         = ParamKind::Float;
	p.defaultValue = defaultValue;
	p.floatRange   = range;
	p.help         = std::move(help);
	return p;
}

[[nodiscard]] EncoderParamInfo enum_param(
	std::string            name,
	std::string            label,
	std::string            group,
	std::string            defaultValue,
	std::vector<EnumValue> values,
	std::string            help
) {
	EncoderParamInfo p;
	p.name         = std::move(name);
	p.label        = std::move(label);
	p.group        = std::move(group);
	p.kind         = ParamKind::Enum;
	p.defaultValue = std::move(defaultValue);
	p.enumValues   = std::move(values);
	p.help         = std::move(help);
	return p;
}

[[nodiscard]] EncoderParamInfo enabled_when(
	EncoderParamInfo parameter,
	std::string controller,
	std::vector<std::string> acceptedValues,
	std::string explanation
) {
	parameter.enabledWhen.push_back({
		std::move(controller),
		std::move(acceptedValues),
		std::move(explanation),
	});
	return parameter;
}

} // namespace

std::vector<EncoderParamInfo> query_svt_av1_parameters() {
	return {
		int_param(
			"preset",
			"Preset",
			"Speed / Quality",
			8,
			{-1, 13, 1},
			"SVT-AV1 encoder preset. Lower is slower and higher quality; higher is faster."
		),
		enum_param(
			"rate-control",
			"Mode",
			"Rate Control",
			"crf",
			{{"crf", "Constant quality (CRF)"}, {"cqp", "Constant QP"}, {"lossless", "Lossless"}},
			"Select one mutually exclusive SVT-AV1 rate-control strategy."
		),
		enabled_when(float_param(
			"crf",
			"CRF",
			"Rate Control",
			35.0,
			{1.0, 70.0, 0.25},
			"Constant rate factor. Lower values improve quality and increase size."
		), "rate-control", {"crf"}, "CRF applies only in constant-quality mode."),
		enabled_when(int_param(
			"qp",
			"QP",
			"Rate Control",
			35,
			{1, 63, 1},
			"Initial quantizer. Used directly when adaptive quantization is disabled."
		), "rate-control", {"cqp"}, "QP applies only in constant-QP mode."),
		enabled_when(enum_param(
			"aq-mode",
			"AQ mode",
			"Rate Control",
			"2",
			{
				{"0", "Off / CQP"},
				{"1", "Variance AQ"},
				{"2", "Delta-Q / CRF"},
			},
			"Adaptive quantization mode. SVT-AV1 CRF mode uses aq-mode 2."
		), "rate-control", {"crf"}, "Adaptive quantization applies only in CRF mode."),
		enum_param(
			"tune",
			"Tune",
			"Quality Metric",
			"3",
			{
				{"0", "VQ"},
				{"1", "PSNR"},
				{"2", "SSIM"},
				{"3", "Image Quality"},
				{"4", "MS-SSIM / SSIMULACRA2"},
			},
			"Metric or perceptual target used by SVT-AV1 mode decisions."
		),
		enum_param(
			"enable-dlf",
			"Deblocking",
			"Filters",
			"1",
			{
				{"0", "Disabled"},
				{"1", "Enabled"},
				{"2", "Accurate"},
			},
			"Deblocking loop filter control."
		),
		bool_param(
			"enable-cdef",
			"CDEF",
			"Filters",
			true,
			"Enable constrained directional enhancement filtering."
		),
		bool_param(
			"enable-restoration",
			"Loop restoration",
			"Filters",
			true,
			"Enable AV1 loop restoration filtering."
		),
		enum_param(
			"scm",
			"Screen content mode",
			"Content Tools",
			"2",
			{
				{"0", "Off"},
				{"1", "On"},
				{"2", "Adaptive"},
				{"3", "Adaptive anti-aliased"},
			},
			"Screen-content detection level."
		),
		bool_param(
			"enable-qm",
			"Quant matrices",
			"Quantization",
			false,
			"Enable quantization matrices."
		),
		int_param(
			"sharpness",
			"Sharpness",
			"Psychovisual",
			0,
			{-7, 7, 1},
			"Bias deblocking and rate distortion toward softer or sharper output."
		),
		bool_param(
			"enable-variance-boost",
			"Variance boost",
			"Psychovisual",
			false,
			"Boost quality in selected low-variance regions."
		),
		enum_param(
			"variance-boost-curve",
			"Variance boost curve",
			"Psychovisual",
			"0",
			{
				{"0", "Default"},
				{"1", "Low-medium contrast"},
				{"2", "Still picture"},
			},
			"Curve used by variance boost. Curve 2 is intended for still pictures."
		),
		int_param(
			"max-tx-size",
			"Max transform size",
			"Transforms",
			64,
			{32, 64, 32},
			"Limit AV1 transform size. 32 can be useful for still-image detail."
		),
		float_param(
			"ac-bias",
			"AC bias",
			"Psychovisual",
			0.0,
			{0.0, 8.0, 0.05},
			"Bias the internal RD metric toward high-frequency detail preservation."
		),
	};
}

EncodedImage encode_svt_av1_still_image(const RawImage& image, std::span<const EncoderParam> params) {
	const FormatInfo info = format_info(image.format);
	validate_image(image, info);

	EbSvtAv1EncConfiguration cfg{};
	EbComponentType* rawEncoder = nullptr;
	svt_av1_set_log_callback(discard_svt_log, nullptr);
	checked_svt_call(svt_av1_enc_init_handle(&rawEncoder, &cfg), "init_handle");
	EncoderPtr encoder{rawEncoder, SvtEncoderDeleter{}};

	force_still_image_config(cfg, image, info);

	const auto rateControl = std::find_if(params.begin(), params.end(), [](const EncoderParam& param) {
		return param.name == "rate-control";
	});
	const std::string rateControlMode =
		rateControl == params.end() ? std::string{} : std::get<std::string>(rateControl->value);
	for (const EncoderParam& param : params) {
		if (param.name == "rate-control") {
			continue;
		}
		if (is_structural_param(param.name)) {
			throw std::runtime_error(
				"parameter '" + param.name + "' is controlled by encode_svt_av1_still_image"
			);
		}

		parse_or_throw(cfg, param.name, param_value_to_string(param.value));
	}
	if (rateControlMode == "cqp") {
		parse_or_throw(cfg, "aq-mode", "0");
	} else if (rateControlMode == "lossless") {
		parse_or_throw(cfg, "lossless", "1");
	} else if (!rateControlMode.empty() && rateControlMode != "crf") {
		throw std::invalid_argument("unsupported SVT-AV1 rate-control mode: " + rateControlMode);
	}

	force_still_image_config(cfg, image, info);
	checked_svt_call(svt_av1_enc_set_parameter(encoder.get(), &cfg), "set_parameter");
	checked_svt_call(svt_av1_enc_init(encoder.get()), "init");
	encoder.get_deleter().initialized = true;

	std::vector<std::byte> obu;
	append_temporal_delimiter(obu);

	EbBufferHeaderType* rawHeader = nullptr;
	checked_svt_call(svt_av1_enc_stream_header(encoder.get(), &rawHeader), "stream_header");
	HeaderPtr header{rawHeader};
	append_buffer(obu, *header);

	EbSvtIOFormat input{};
	input.luma     = const_cast<uint8_t*>(image.planes[0].bytes.data());
	input.y_stride = static_cast<uint32_t>(image.planes[0].strideBytes / (info.bitDepth <= 8 ? 1 : 2));
	if (info.planeCount > 1) {
		input.cb        = const_cast<uint8_t*>(image.planes[1].bytes.data());
		input.cr        = const_cast<uint8_t*>(image.planes[2].bytes.data());
		input.cb_stride = static_cast<uint32_t>(image.planes[1].strideBytes / (info.bitDepth <= 8 ? 1 : 2));
		input.cr_stride = static_cast<uint32_t>(image.planes[2].strideBytes / (info.bitDepth <= 8 ? 1 : 2));
	}

	EbBufferHeaderType inputHeader{};
	std::size_t inputByteCount = 0;
	for (int plane = 0; plane < info.planeCount; ++plane) {
		const int planeHeight = ceil_div(image.height, info.heightDiv[plane]);
		inputByteCount += static_cast<std::size_t>(image.planes[plane].strideBytes) *
		                  static_cast<std::size_t>(planeHeight);
	}

	inputHeader.size         = sizeof(inputHeader);
	inputHeader.p_buffer     = reinterpret_cast<uint8_t*>(&input);
	inputHeader.n_alloc_len  = static_cast<uint32_t>(inputByteCount);
	inputHeader.n_filled_len = static_cast<uint32_t>(inputByteCount);
	inputHeader.pts          = 0;
	inputHeader.dts          = 0;
	inputHeader.flags        = 0;

	checked_svt_call(svt_av1_enc_send_picture(encoder.get(), &inputHeader), "send_picture");

	EbBufferHeaderType eosHeader{};
	eosHeader.size  = sizeof(eosHeader);
	eosHeader.flags = EB_BUFFERFLAG_EOS;
	checked_svt_call(svt_av1_enc_send_picture(encoder.get(), &eosHeader), "send_eos");

	while (true) {
		EbBufferHeaderType* rawPacket = nullptr;
		const EbErrorType error = svt_av1_enc_get_packet(encoder.get(), &rawPacket, 1);

		if (error == EB_NoErrorEmptyQueue) {
			continue;
		}
		checked_svt_call(error, "get_packet");

		OutputPtr packet{rawPacket};
		append_buffer(obu, *packet);

		if ((packet->flags & EB_BUFFERFLAG_EOS) != 0) {
			break;
		}
	}

	if (obu.empty()) {
		throw std::runtime_error("SVT-AV1 produced an empty bitstream");
	}

	EncodedImage result;
	append_ivf_header(result.hevcAnnexB, image.width, image.height, 1);
	append_ivf_frame(result.hevcAnnexB, obu, 0);

	return result;
}

} // namespace codec_gui
