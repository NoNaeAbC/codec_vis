// codec_gui_x265.cpp
#include "codec_gui_x265.hpp"

#include <charconv>
#include <cstring>
#include <memory>
#include <sstream>

namespace codec_gui {
	namespace {

		struct X265ParamDeleter {
			const x265_api *api = nullptr;

			void operator()(x265_param *p) const noexcept {
				if (p && api) api->param_free(p);
			}
		};

		struct X265EncoderDeleter {
			const x265_api *api = nullptr;

			void operator()(x265_encoder *e) const noexcept {
				if (e && api) api->encoder_close(e);
			}
		};

		using ParamPtr   = std::unique_ptr<x265_param, X265ParamDeleter>;
		using EncoderPtr = std::unique_ptr<x265_encoder, X265EncoderDeleter>;

		std::string to_x265_value(const ParamValue &value) {
			return std::visit(
					[](const auto &v) -> std::string {
						using T = std::decay_t<decltype(v)>;

						if constexpr (std::is_same_v<T, bool>) {
							return v ? "1" : "0";
						} else if constexpr (std::is_same_v<T, int64_t>) {
							return std::to_string(v);
						} else if constexpr (std::is_same_v<T, double>) {
							std::ostringstream oss;
							oss << v;
							return oss.str();
						} else {
							return v;
						}
					},
					value);
		}

		void parse_or_throw(const x265_api *api, x265_param *p, std::string_view name, std::string_view value) {
			const std::string n{name};
			const std::string v{value};

			const int rc = api->param_parse(p, n.c_str(), v.c_str());
			if (rc == X265_PARAM_BAD_NAME) { throw std::invalid_argument("x265: unknown parameter: " + n); }
			if (rc == X265_PARAM_BAD_VALUE) {
				throw std::invalid_argument("x265: invalid value for parameter '" + n + "': " + v);
			}
			if (rc != 0) { throw std::runtime_error("x265: failed to parse parameter '" + n + "'"); }
		}

		int x265_csp(PixelFormat fmt) {
			switch (fmt) {
				case PixelFormat::YUV420P8:
				case PixelFormat::YUV420P10LE:
				case PixelFormat::YUV420P12LE:
				case PixelFormat::YUV420P14LE:
					return X265_CSP_I420;
				case PixelFormat::YUV422P8:
				case PixelFormat::YUV422P10LE:
				case PixelFormat::YUV422P12LE:
				case PixelFormat::YUV422P14LE:
					return X265_CSP_I422;
				case PixelFormat::YUV444P8:
				case PixelFormat::YUV444P10LE:
				case PixelFormat::YUV444P12LE:
				case PixelFormat::YUV444P14LE:
					return X265_CSP_I444;
				case PixelFormat::Gray8:
				case PixelFormat::Gray10LE:
				case PixelFormat::Gray12LE:
				case PixelFormat::Gray14LE:
					return X265_CSP_I400;
			}
			throw std::invalid_argument("unsupported pixel format");
		}

		int bit_depth(PixelFormat fmt) {
			switch (fmt) {
				case PixelFormat::YUV420P8:
				case PixelFormat::YUV422P8:
				case PixelFormat::YUV444P8:
				case PixelFormat::Gray8:
					return 8;
				case PixelFormat::YUV420P10LE:
				case PixelFormat::YUV422P10LE:
				case PixelFormat::YUV444P10LE:
				case PixelFormat::Gray10LE:
					return 10;
				case PixelFormat::YUV420P12LE:
				case PixelFormat::YUV422P12LE:
				case PixelFormat::YUV444P12LE:
				case PixelFormat::Gray12LE:
					return 12;
				case PixelFormat::YUV420P14LE:
				case PixelFormat::YUV422P14LE:
				case PixelFormat::YUV444P14LE:
				case PixelFormat::Gray14LE:
					return 14;
			}
			throw std::invalid_argument("unsupported pixel format");
		}

		int plane_count(PixelFormat fmt) {
			switch (fmt) {
				case PixelFormat::Gray8:
				case PixelFormat::Gray10LE:
				case PixelFormat::Gray12LE:
				case PixelFormat::Gray14LE:
					return 1;
				default:
					return 3;
			}
		}

		void append_nals(std::vector<std::byte> &out, x265_nal *nals, uint32_t count) {
			for (uint32_t i = 0; i < count; ++i) {
				const auto *p = reinterpret_cast<const std::byte *>(nals[i].payload);
				out.insert(out.end(), p, p + nals[i].sizeBytes);
			}
		}

	} // namespace

	std::vector<EncoderParamInfo> query_x265_parameters() {
		return {
				{
						.name         = "preset",
						.label        = "Preset",
						.group        = "Speed / Search",
						.kind         = ParamKind::Enum,
						.defaultValue = std::string{"medium"},
						.enumValues =
								{
										{"ultrafast", "Ultrafast"},
										{"superfast", "Superfast"},
										{"veryfast", "Veryfast"},
										{"faster", "Faster"},
										{"fast", "Fast"},
										{"medium", "Medium"},
										{"slow", "Slow"},
										{"slower", "Slower"},
										{"veryslow", "Veryslow"},
										{"placebo", "Placebo"},
								},
						.help = "Global speed/quality preset. Still-image comparison should usually expose slow, "
								"slower, veryslow, and placebo.",
				},
				{
						.name         = "tune",
						.label        = "Tune",
						.group        = "Speed / Search",
						.kind         = ParamKind::Enum,
						.defaultValue = std::string{""},
						.enumValues =
								{
										{"", "None"},
										{"psnr", "PSNR"},
										{"ssim", "SSIM"},
										{"grain", "Grain"},
										{"zerolatency", "Zero latency"},
										{"fastdecode", "Fast decode"},
										{"animation", "Animation"},
								},
						.help = "High-level encoder tuning.",
				},
				{
						.name         = "profile",
						.label        = "Profile",
						.group        = "Bitstream",
						.kind         = ParamKind::Enum,
						.defaultValue = std::string{"auto"},
						.enumValues =
								{
										{"auto", "Auto"},
								},
						.help = "The profile is derived from the selected bit depth and chroma layout so an incompatible restriction cannot be selected.",
				},
				{
						.name         = "rate-control",
						.label        = "Rate-control mode",
						.group        = "Rate Control",
						.kind         = ParamKind::Enum,
						.defaultValue = std::string{"crf"},
						.enumValues   = {{"qp", "Constant QP"}, {"crf", "Constant quality (CRF)"}, {"lossless", "Lossless"}},
						.help         = "Selects exactly one rate-control strategy. Inactive controls are disabled and are not sent to x265.",
				},
				{
						.name         = "qp",
						.label        = "QP",
						.group        = "Rate Control",
						.kind         = ParamKind::Int,
						.defaultValue = int64_t{22},
						.intRange     = IntRange{0, 51, 1},
						.help         = "Constant quantizer. Lower means larger and higher quality. Very relevant for "
										"still-image comparisons.",
						.enabledWhen  = {{"rate-control", {"qp"}, "QP is available only in Constant QP mode."}},
				},
				{
						.name         = "crf",
						.label        = "CRF",
						.group        = "Rate Control",
						.kind         = ParamKind::Float,
						.defaultValue = double{28.0},
						.floatRange   = FloatRange{0.0, 51.0, 0.1},
						.help         = "Constant rate factor. Better for perceptual target quality than exact "
										"reproducibility.",
						.enabledWhen  = {{"rate-control", {"crf"}, "CRF is available only in Constant quality mode."}},
				},
				{
						.name         = "lossless",
						.label        = "Lossless",
						.group        = "Rate Control",
						.kind         = ParamKind::Bool,
						.defaultValue = false,
						.help         = "Enable mathematically lossless coding.",
						.relevantForStillImage = false,
				},
				{
						.name         = "rd",
						.label        = "RD level",
						.group        = "Intra Analysis",
						.kind         = ParamKind::Int,
						.defaultValue = int64_t{3},
						.intRange     = IntRange{0, 6, 1},
						.help = "Rate-distortion search depth. One of the most important knobs for I-frame image "
								"coding.",
				},
				{
						.name         = "rdoq-level",
						.label        = "RDOQ level",
						.group        = "Intra Analysis",
						.kind         = ParamKind::Int,
						.defaultValue = int64_t{0},
						.intRange     = IntRange{0, 2, 1},
						.help         = "Rate-distortion optimized quantization level.",
				},
				{
						.name         = "psy-rd",
						.label        = "Psy RD",
						.group        = "Psychovisual",
						.kind         = ParamKind::Float,
						.defaultValue = double{2.0},
						.floatRange   = FloatRange{0.0, 5.0, 0.05},
						.help         = "Psychovisual rate-distortion strength. Useful for photographic detail.",
				},
				{
						.name         = "psy-rdoq",
						.label        = "Psy RDOQ",
						.group        = "Psychovisual",
						.kind         = ParamKind::Float,
						.defaultValue = double{0.0},
						.floatRange   = FloatRange{0.0, 50.0, 0.05},
						.help = "Psychovisual quantization bias. Can retain texture, but may hurt objective metrics.",
				},
				{
						.name         = "aq-mode",
						.label        = "AQ mode",
						.group        = "Quantization",
						.kind         = ParamKind::Enum,
						.defaultValue = std::string{"3"},
						.enumValues =
								{
										{"0", "Disabled"},
										{"1", "Variance AQ"},
										{"2", "Auto-variance AQ"},
										{"3", "Auto-variance AQ biased"},
										{"4", "Edge AQ"},
								},
						.help = "Adaptive quantization mode. Edge AQ is interesting for screenshots and text; biased "
								"auto-variance is often good for photos.",
				},
				{
						.name         = "aq-strength",
						.label        = "AQ strength",
						.group        = "Quantization",
						.kind         = ParamKind::Float,
						.defaultValue = double{1.0},
						.floatRange   = FloatRange{0.0, 3.0, 0.05},
						.help         = "Adaptive quantization strength.",
				},
				{
						.name         = "ctu",
						.label        = "CTU size",
						.group        = "Block Structure",
						.kind         = ParamKind::Enum,
						.defaultValue = std::string{"64"},
						.enumValues =
								{
										{"16", "16"},
										{"32", "32"},
										{"64", "64"},
								},
						.help = "Coding tree unit size. Smaller can help sharp synthetic content; 64 is normally best "
								"for photos.",
				},
				{
						.name         = "min-cu-size",
						.label        = "Minimum CU size",
						.group        = "Block Structure",
						.kind         = ParamKind::Enum,
						.defaultValue = std::string{"8"},
						.enumValues =
								{
										{"8", "8"},
										{"16", "16"},
								},
						.help = "Minimum coding unit sizes valid with every selectable CTU size.",
				},
				{
						.name         = "tu-intra-depth",
						.label        = "TU intra depth",
						.group        = "Transform",
						.kind         = ParamKind::Int,
						.defaultValue = int64_t{1},
						.intRange     = IntRange{1, 4, 1},
						.help = "Transform recursion depth for intra blocks. Important for still-image texture and "
								"edges.",
				},
				{
						.name         = "limit-tu",
						.label        = "Limit TU",
						.group        = "Transform",
						.kind         = ParamKind::Int,
						.defaultValue = int64_t{0},
						.intRange     = IntRange{0, 4, 1},
						.help         = "Prunes transform search. Keep at 0 for exhaustive still-image comparisons.",
				},
				{
						.name         = "sao",
						.label        = "SAO",
						.group        = "Filters",
						.kind         = ParamKind::Bool,
						.defaultValue = true,
						.help = "Sample adaptive offset. Often useful for photos, often questionable for UI/text.",
				},
				{
						.name         = "deblock",
						.label        = "Deblock",
						.group        = "Filters",
						.kind         = ParamKind::String,
						.defaultValue = std::string{"0:0"},
						.help         = "Deblocking offsets as alpha:beta, for example 0:0 or -1:-1.",
				},
				{
						.name         = "strong-intra-smoothing",
						.label        = "Strong intra smoothing",
						.group        = "Intra Analysis",
						.kind         = ParamKind::Bool,
						.defaultValue = true,
						.help         = "HEVC strong intra smoothing. Can help gradients; may blur synthetic edges.",
				},
				{
						.name         = "constrained-intra",
						.label        = "Constrained intra",
						.group        = "Intra Analysis",
						.kind         = ParamKind::Bool,
						.defaultValue = false,
						.help = "Mostly useful for mixed inter/intra streams. Usually not useful for single still "
								"images.",
				},
				{
						.name         = "keyint",
						.label        = "Keyframe interval",
						.group        = "Still Image Safety",
						.kind         = ParamKind::Int,
						.defaultValue = int64_t{1},
						.intRange     = IntRange{1, 1000, 1},
						.help         = "For still-image encoding, force this to 1.",
						.relevantForStillImage = false,
				},
				{
						.name         = "min-keyint",
						.label        = "Minimum keyframe interval",
						.group        = "Still Image Safety",
						.kind         = ParamKind::Int,
						.defaultValue = int64_t{1},
						.intRange     = IntRange{1, 1000, 1},
						.help         = "For still-image encoding, force this to 1.",
						.relevantForStillImage = false,
				},
				{
						.name         = "bframes",
						.label        = "B-frames",
						.group        = "Still Image Safety",
						.kind         = ParamKind::Int,
						.defaultValue = int64_t{0},
						.intRange     = IntRange{0, X265_BFRAME_MAX, 1},
						.help         = "For still-image encoding, force this to 0.",
						.relevantForStillImage = false,
				},
				{
						.name         = "ref",
						.label        = "References",
						.group        = "Still Image Safety",
						.kind         = ParamKind::Int,
						.defaultValue = int64_t{1},
						.intRange     = IntRange{1, 16, 1},
						.help         = "For single-frame encoding, extra references are useless.",
						.relevantForStillImage = false,
				},
		};
	}

	EncodedImage encode_x265_still_image(const RawImage &image, std::span<const EncoderParam> userParams) {
		if (image.width <= 0 || image.height <= 0) { throw std::invalid_argument("invalid image dimensions"); }

		const int planes = plane_count(image.format);
		for (int i = 0; i < planes; ++i) {
			if (image.planes[i].bytes.empty() || image.planes[i].strideBytes <= 0) {
				throw std::invalid_argument("missing image plane");
			}
		}

		const int bitDepth = bit_depth(image.format);
		const x265_api *api = x265_api_get(bitDepth);
		if (api == nullptr) {
			throw std::runtime_error("libx265 does not provide an API for " + std::to_string(bitDepth) + "-bit input");
		}

		ParamPtr param{api->param_alloc(), X265ParamDeleter{api}};
		if (!param) { throw std::bad_alloc{}; }

		std::string requestedPreset;
		std::string requestedTune;
		std::string rateControl = "crf";
		for (const EncoderParam &p: userParams) {
			const std::string value = to_x265_value(p.value);
			if (p.name == "preset") {
				requestedPreset = value;
			} else if (p.name == "tune") {
				requestedTune = value;
			} else if (p.name == "rate-control") {
				rateControl = value;
			}
		}

		if (!requestedPreset.empty() || !requestedTune.empty()) {
			const int rc = api->param_default_preset(
					param.get(),
					requestedPreset.empty() ? nullptr : requestedPreset.c_str(),
					requestedTune.empty() ? nullptr : requestedTune.c_str());
			if (rc < 0) {
				throw std::invalid_argument("invalid x265 preset/tune: " + requestedPreset + "/" + requestedTune);
			}
		} else {
			api->param_default(param.get());
		}

		// Still-image defaults. User params below may override most of these.
		parse_or_throw(api, param.get(), "keyint", "1");
		parse_or_throw(api, param.get(), "min-keyint", "1");
		parse_or_throw(api, param.get(), "bframes", "0");
		parse_or_throw(api, param.get(), "ref", "1");
		parse_or_throw(api, param.get(), "scenecut", "0");
		parse_or_throw(api, param.get(), "open-gop", "0");
		parse_or_throw(api, param.get(), "rc-lookahead", "0");
		parse_or_throw(api, param.get(), "repeat-headers", "1");

		param->sourceWidth      = image.width;
		param->sourceHeight     = image.height;
		param->internalCsp      = x265_csp(image.format);
		param->internalBitDepth = bitDepth;
		param->fpsNum           = 1;
		param->fpsDenom         = 1;
		param->vui.colorPrimaries = static_cast<int>(image.color.primaries);
		param->vui.transferCharacteristics = static_cast<int>(image.color.transfer);
		param->vui.matrixCoeffs = static_cast<int>(image.color.matrix);
		param->vui.bEnableVideoFullRangeFlag = image.color.range == ColorRange::Full ? 1 : 0;

		auto auto_profile = [](PixelFormat format) -> const char* {
				switch (format) {
					case PixelFormat::YUV420P8: return "main";
					case PixelFormat::YUV420P10LE: return "main10";
					case PixelFormat::YUV420P12LE: return "main12";
					case PixelFormat::YUV420P14LE: return "main12";
					case PixelFormat::YUV422P8: return "main422-10";
					case PixelFormat::YUV422P10LE: return "main422-10";
					case PixelFormat::YUV422P12LE: return "main422-12";
					case PixelFormat::YUV422P14LE: return "main422-12";
					case PixelFormat::YUV444P8: return "main444-8";
				case PixelFormat::YUV444P10LE: return "main444-10";
				case PixelFormat::YUV444P12LE: return "main444-12";
				case PixelFormat::YUV444P14LE: return "main444-12";
				case PixelFormat::Gray8: return "main";
				case PixelFormat::Gray10LE: return "main10";
				case PixelFormat::Gray12LE: return "main12";
				case PixelFormat::Gray14LE: return "main12";
			}
			return "main";
		};

		std::string requestedProfile = "auto";

		for (const EncoderParam &p: userParams) {
			const std::string value = to_x265_value(p.value);

			if (p.name == "preset" || p.name == "tune" || p.name == "rate-control") {
				continue;
			}

			if (p.name == "profile") {
				requestedProfile = value;
				continue;
			}

			parse_or_throw(api, param.get(), p.name, value);
		}
		if (rateControl == "lossless") {
			parse_or_throw(api, param.get(), "lossless", "1");
		} else if (rateControl != "qp" && rateControl != "crf") {
			throw std::invalid_argument("invalid x265 rate-control mode: " + rateControl);
		}

		// These are still-image invariants, not tunable controls. Re-apply them
		// after parsing so callers cannot override them through expert params.
		parse_or_throw(api, param.get(), "keyint", "1");
		parse_or_throw(api, param.get(), "min-keyint", "1");
		parse_or_throw(api, param.get(), "bframes", "0");
		parse_or_throw(api, param.get(), "ref", "1");
		parse_or_throw(api, param.get(), "scenecut", "0");
		parse_or_throw(api, param.get(), "open-gop", "0");
		parse_or_throw(api, param.get(), "rc-lookahead", "0");

		if (requestedProfile == "auto") {
			requestedProfile = auto_profile(image.format);
		}

		if (!requestedProfile.empty()) {
			const int rc = api->param_apply_profile(param.get(), requestedProfile.c_str());
			if (rc < 0) {
				throw std::invalid_argument("x265 profile is incompatible with selected settings: " + requestedProfile);
			}
		}

		EncoderPtr encoder{api->encoder_open(param.get()), X265EncoderDeleter{api}};
		if (!encoder) {
			throw std::runtime_error(
					"x265_encoder_open failed; selected parameters are probably out of range or incompatible");
		}

		EncodedImage result;

		x265_nal *nals     = nullptr;
		uint32_t  nalCount = 0;

		// Emit VPS/SPS/PPS.
		const int headerBytes = api->encoder_headers(encoder.get(), &nals, &nalCount);
		if (headerBytes < 0) { throw std::runtime_error("x265_encoder_headers failed"); }
		append_nals(result.hevcAnnexB, nals, nalCount);

		x265_picture pic;
		api->picture_init(param.get(), &pic);

		pic.pts        = 0;
		pic.sliceType  = X265_TYPE_IDR;
		pic.colorSpace = x265_csp(image.format);
		pic.bitDepth   = bitDepth;

		for (int i = 0; i < planes; ++i) {
			pic.planes[i] = (void *) image.planes[i].bytes.data();
			pic.stride[i] = image.planes[i].strideBytes;
		}

		x265_picture outPic;
		std::memset(&outPic, 0, sizeof(outPic));

		const int encoded = api->encoder_encode(encoder.get(), &nals, &nalCount, &pic, &outPic);

		if (encoded < 0) { throw std::runtime_error("x265_encoder_encode failed"); }

		append_nals(result.hevcAnnexB, nals, nalCount);

		// Flush delayed output. Should usually be empty for this still-image setup.
		while (true) {
			const int flushed = api->encoder_encode(encoder.get(), &nals, &nalCount, nullptr, &outPic);

			if (flushed < 0) { throw std::runtime_error("x265 flush failed"); }

			if (flushed == 0 && nalCount == 0) { break; }

			append_nals(result.hevcAnnexB, nals, nalCount);

			if (flushed == 0) { break; }
		}

		return result;
	}

} // namespace codec_gui
