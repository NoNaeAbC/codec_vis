// codec_gui_uvg266.cpp
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
#include <uvg266/uvg266.h>
}

namespace codec_gui {
namespace {

struct FormatInfo {
	enum uvg_chroma_format chromaFormat;
	int                    bitDepth;
	int                    planeCount;
	std::array<int, 3>     widthDiv;
	std::array<int, 3>     heightDiv;
};

[[nodiscard]] int ceil_div(const int value, const int divisor) {
	return (value + divisor - 1) / divisor;
}

[[nodiscard]] FormatInfo format_info(const PixelFormat format) {
	switch (format) {
		case PixelFormat::YUV420P8:
			return {UVG_CSP_420, 8, 3, {1, 2, 2}, {1, 2, 2}};
			case PixelFormat::YUV420P10LE:
				return {UVG_CSP_420, 10, 3, {1, 2, 2}, {1, 2, 2}};
			case PixelFormat::YUV422P8:
				return {UVG_CSP_422, 8, 3, {1, 2, 2}, {1, 1, 1}};
			case PixelFormat::YUV422P10LE:
				return {UVG_CSP_422, 10, 3, {1, 2, 2}, {1, 1, 1}};
			case PixelFormat::YUV444P8:
			return {UVG_CSP_444, 8, 3, {1, 1, 1}, {1, 1, 1}};
		case PixelFormat::YUV444P10LE:
			return {UVG_CSP_444, 10, 3, {1, 1, 1}, {1, 1, 1}};
		case PixelFormat::Gray8:
			return {UVG_CSP_400, 8, 1, {1, 1, 1}, {1, 1, 1}};
		case PixelFormat::Gray10LE:
			return {UVG_CSP_400, 10, 1, {1, 1, 1}, {1, 1, 1}};
		case PixelFormat::YUV420P12LE: case PixelFormat::YUV420P14LE:
		case PixelFormat::YUV422P12LE: case PixelFormat::YUV422P14LE:
		case PixelFormat::YUV444P12LE: case PixelFormat::YUV444P14LE:
		case PixelFormat::Gray12LE: case PixelFormat::Gray14LE: break;
	}

	throw std::runtime_error("unsupported pixel format");
}

[[nodiscard]] int chroma_width_div(const FormatInfo& info, const int plane) {
	return info.widthDiv[plane];
}

[[nodiscard]] int chroma_height_div(const FormatInfo& info, const int plane) {
	return info.heightDiv[plane];
}

[[nodiscard]] enum uvg_input_format input_format_for_chroma(const FormatInfo& info) {
	switch (info.chromaFormat) {
		case UVG_CSP_400:
			return UVG_FORMAT_P400;
		case UVG_CSP_420:
			return UVG_FORMAT_P420;
		case UVG_CSP_422:
			return UVG_FORMAT_P422;
		case UVG_CSP_444:
			return UVG_FORMAT_P444;
	}
	throw std::runtime_error("unsupported uvg266 chroma format");
}

void validate_image(const RawImage& image, const FormatInfo& info) {
	if (image.width <= 0 || image.height <= 0) {
		throw std::runtime_error("image dimensions must be positive");
	}

	if (info.chromaFormat != UVG_CSP_400 && info.chromaFormat != UVG_CSP_420) {
		throw std::runtime_error("this uvg266 build only supports monochrome and YUV420 still images");
	}

	if (info.chromaFormat == UVG_CSP_420 && ((image.width & 1) != 0 || (image.height & 1) != 0)) {
		throw std::runtime_error("YUV420 images require even width and height");
	}

	const int bytesPerSample = info.bitDepth <= 8 ? 1 : 2;
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

void append_chunks(std::vector<std::byte>& out, const uvg_data_chunk* chunk) {
	for (const uvg_data_chunk* cur = chunk; cur != nullptr; cur = cur->next) {
		const auto* first = reinterpret_cast<const std::byte*>(cur->data);
		out.insert(out.end(), first, first + cur->len);
	}
}

void copy_plane_to_uvg(
	const ImagePlane& src,
	uvg_pixel*        dst,
	const int         dstStrideSamples,
	const int         width,
	const int         height,
	const int         srcBytesPerSample,
	const int         dstBytesPerSample
) {
	for (int y = 0; y < height; ++y) {
		const uint8_t* srcRow = src.bytes.data() + static_cast<std::size_t>(y) * src.strideBytes;
		auto* dstRow = reinterpret_cast<uint8_t*>(dst) +
		               static_cast<std::size_t>(y) * static_cast<std::size_t>(dstStrideSamples) *
		                   static_cast<std::size_t>(dstBytesPerSample);
		if (srcBytesPerSample == dstBytesPerSample) {
			std::memcpy(dstRow, srcRow, static_cast<std::size_t>(width) * static_cast<std::size_t>(dstBytesPerSample));
		} else if (srcBytesPerSample == 2 && dstBytesPerSample == 1) {
			for (int x = 0; x < width; ++x) {
				const uint16_t sample = static_cast<uint16_t>(srcRow[x * 2]) |
				                        (static_cast<uint16_t>(srcRow[x * 2 + 1]) << 8u);
				dstRow[x] = static_cast<uint8_t>(std::min<uint16_t>(sample >> 2u, 255u));
			}
		} else if (srcBytesPerSample == 1 && dstBytesPerSample == 2) {
			for (int x = 0; x < width; ++x) {
				const uint16_t sample = static_cast<uint16_t>(srcRow[x]) << 2u;
				dstRow[x * 2] = static_cast<uint8_t>(sample & 0xffu);
				dstRow[x * 2 + 1] = static_cast<uint8_t>(sample >> 8u);
			}
		} else {
			throw std::runtime_error("unsupported uvg266 sample-size conversion");
		}
	}
}

std::shared_ptr<const RawImage> copy_uvg_picture_to_raw(
	const uvg_picture& pic,
	const FormatInfo& info,
	const int         bytesPerSample
) {
	auto image = std::make_shared<RawImage>();
	image->width = pic.width;
	image->height = pic.height;
	if (info.chromaFormat == UVG_CSP_400) {
		image->format = bytesPerSample == 1 ? PixelFormat::Gray8 : PixelFormat::Gray10LE;
	} else if (info.chromaFormat == UVG_CSP_444) {
		image->format = bytesPerSample == 1 ? PixelFormat::YUV444P8 : PixelFormat::YUV444P10LE;
	} else if (info.chromaFormat == UVG_CSP_422) {
		image->format = bytesPerSample == 1 ? PixelFormat::YUV422P8 : PixelFormat::YUV422P10LE;
	} else {
		image->format = bytesPerSample == 1 ? PixelFormat::YUV420P8 : PixelFormat::YUV420P10LE;
	}

	for (int plane = 0; plane < info.planeCount; ++plane) {
		const int planeWidth = ceil_div(pic.width, chroma_width_div(info, plane));
		const int planeHeight = ceil_div(pic.height, chroma_height_div(info, plane));
		const int srcStrideSamples = plane == 0 ? pic.stride : ceil_div(pic.stride, chroma_width_div(info, plane));
		const int dstStrideBytes = planeWidth * bytesPerSample;
		image->planes[plane].strideBytes = dstStrideBytes;
		image->planes[plane].bytes.resize(static_cast<std::size_t>(dstStrideBytes) * static_cast<std::size_t>(planeHeight));
		const auto* src = reinterpret_cast<const uint8_t*>(pic.data[plane]);
		for (int y = 0; y < planeHeight; ++y) {
			std::memcpy(
				image->planes[plane].bytes.data() + static_cast<std::size_t>(y) * static_cast<std::size_t>(dstStrideBytes),
				src + static_cast<std::size_t>(y) * static_cast<std::size_t>(srcStrideSamples) * static_cast<std::size_t>(bytesPerSample),
				static_cast<std::size_t>(dstStrideBytes)
			);
		}
	}
	return image;
}

[[nodiscard]] EncoderParamInfo bool_param(
	std::string name,
	std::string label,
	std::string group,
	bool        defaultValue,
	std::string help,
	bool        relevant = true
) {
	EncoderParamInfo p;
	p.name                  = std::move(name);
	p.label                 = std::move(label);
	p.group                 = std::move(group);
	p.kind                  = ParamKind::Bool;
	p.defaultValue          = defaultValue;
	p.help                  = std::move(help);
	p.relevantForStillImage = relevant;
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

} // namespace

std::vector<EncoderParamInfo> query_uvg266_parameters() {
	return {
		enum_param(
			"bit-depth",
			"Bit depth",
			"Output Format",
			std::to_string(UVG_BIT_DEPTH),
			{{std::to_string(UVG_BIT_DEPTH), std::to_string(UVG_BIT_DEPTH) + "-bit"}},
			"Bit depth compiled into this uvg266 library."
		),
		int_param("qp", "QP", "Rate Control", 48, {0, 63, 1}, "Quantization parameter."),
		enum_param(
			"preset",
			"Preset",
			"Speed / Quality",
			"medium",
			{
				{"ultrafast", "Ultrafast"},
				{"superfast", "Superfast"},
				{"veryfast", "Veryfast"},
				{"faster", "Faster"},
				{"fast", "Fast"},
				{"medium", "Medium"},
				{"slow", "Slow"},
				{"slower", "Slower"},
			},
			"uvg266 preset, passed through uvg266's option parser."
		),
		int_param("rdo", "RDO level", "Analysis", 2, {0, 2, 1}, "Rate-distortion optimization level."),
		bool_param("rdoq", "RDOQ", "Quantization", true, "Enable RD optimized quantization."),
		bool_param("sao", "SAO", "Filters", true, "Enable sample adaptive offset."),
		bool_param("alf", "ALF", "Filters", true, "Enable adaptive loop filtering."),
		bool_param("lossless", "Lossless", "Rate Control", false, "Enable lossless coding."),
		bool_param("mip", "MIP", "Intra Tools", true, "Enable matrix weighted intra prediction."),
		bool_param("lfnst", "LFNST", "Intra Tools", true, "Enable low-frequency non-separable transform."),
		bool_param("isp", "ISP", "Intra Tools", false, "Disabled for still images: this libuvg266 build aborts in AVX2 SSD for non-square ISP blocks.", false),
	};
}

EncodedImage encode_uvg266_still_image(const RawImage& image, std::span<const EncoderParam> params) {
	const FormatInfo info = format_info(image.format);
	validate_image(image, info);
	constexpr int uvgBytesPerSample = static_cast<int>(sizeof(uvg_pixel));
	const int uvgBitDepth = uvgBytesPerSample * 8;

	const uvg_api* api = uvg_api_get(uvgBitDepth);
	if (api == nullptr) {
		throw std::runtime_error("uvg266 does not provide a " + std::to_string(uvgBitDepth) + "-bit API");
	}

	struct ConfigDeleter {
		const uvg_api* api = nullptr;
		void operator()(uvg_config* cfg) const noexcept {
			if (cfg != nullptr && api != nullptr) {
				(void)api->config_destroy(cfg);
			}
		}
	};
	struct PictureDeleter {
		const uvg_api* api = nullptr;
		void operator()(uvg_picture* pic) const noexcept {
			if (pic != nullptr && api != nullptr) {
				api->picture_free(pic);
			}
		}
	};
	struct EncoderDeleter {
		const uvg_api* api = nullptr;
		void operator()(uvg_encoder* encoder) const noexcept {
			if (encoder != nullptr && api != nullptr) {
				api->encoder_close(encoder);
			}
		}
	};
	struct ChunkDeleter {
		const uvg_api* api = nullptr;
		void operator()(uvg_data_chunk* chunk) const noexcept {
			if (chunk != nullptr && api != nullptr) {
				api->chunk_free(chunk);
			}
		}
	};

	std::unique_ptr<uvg_config, ConfigDeleter> cfg{api->config_alloc(), ConfigDeleter{api}};
	if (cfg == nullptr || api->config_init(cfg.get()) == 0) {
		throw std::runtime_error("uvg266 config initialization failed");
	}

	cfg->width          = image.width;
	cfg->height         = image.height;
	cfg->framerate_num  = 1;
	cfg->framerate_denom = 1;
	cfg->intra_period   = 1;
	cfg->vps_period     = 0;
	cfg->gop_len        = 0;
	cfg->ref_frames     = 1;
	cfg->input_format   = input_format_for_chroma(info);
	cfg->input_bitdepth = uvgBitDepth;
	cfg->vui.colorprim  = static_cast<int8_t>(image.color.primaries);
	cfg->vui.transfer   = static_cast<int8_t>(image.color.transfer);
	cfg->vui.colormatrix = static_cast<int8_t>(image.color.matrix);
	cfg->vui.fullrange  = image.color.range == ColorRange::Full ? 1 : 0;
	cfg->vui.chroma_loc = static_cast<int8_t>(image.color.chroma420Location.value_or(Chroma420SampleLocation::LeftCenter));

	for (const EncoderParam& param : params) {
		const std::string value = param_value_to_string(param.value);
		if (param.name == "sao") {
			cfg->sao_type = std::get<bool>(param.value) ? UVG_SAO_FULL : UVG_SAO_OFF;
		} else if (param.name == "alf") {
			cfg->alf_type = std::get<bool>(param.value) ? UVG_ALF_FULL : UVG_ALF_OFF;
		} else if (param.name == "rdo") {
			cfg->rdo = static_cast<int32_t>(std::get<int64_t>(param.value));
		} else if (param.name == "rdoq") {
			cfg->rdoq_enable = std::get<bool>(param.value) ? 1 : 0;
		} else if (param.name == "mip") {
			cfg->mip = std::get<bool>(param.value) ? 1 : 0;
		} else if (param.name == "lfnst") {
			cfg->lfnst = std::get<bool>(param.value) ? 1 : 0;
		} else if (param.name == "isp") {
			cfg->isp = 0;
		} else if (api->config_parse(cfg.get(), param.name.c_str(), value.c_str()) == 0) {
			throw std::runtime_error("invalid uvg266 parameter '" + param.name + "' value '" + value + "'");
		}
	}

	// uvg266 0.8.x AVX2 can abort in pixels_calc_ssd_avx2 for non-square ISP
	// blocks. Keep this hard-disabled so stale settings cannot crash the GUI.
	cfg->isp = 0;

	cfg->width          = image.width;
	cfg->height         = image.height;
	cfg->framerate_num  = 1;
	cfg->framerate_denom = 1;
	cfg->intra_period   = 1;
	cfg->vps_period     = 0;
	cfg->gop_len        = 0;
	cfg->ref_frames     = 1;
	cfg->owf            = 0;
	cfg->threads        = 1;
	cfg->wpp            = 0;
	cfg->hash           = UVG_HASH_NONE;
	cfg->input_format   = input_format_for_chroma(info);
	cfg->input_bitdepth = uvgBitDepth;

	std::unique_ptr<uvg_encoder, EncoderDeleter> encoder{api->encoder_open(cfg.get()), EncoderDeleter{api}};
	if (encoder == nullptr) {
		throw std::runtime_error("uvg266 encoder_open failed");
	}

	std::unique_ptr<uvg_picture, PictureDeleter> pic{
		api->picture_alloc_csp(info.chromaFormat, image.width, image.height),
		PictureDeleter{api},
	};
	if (pic == nullptr) {
		throw std::runtime_error("uvg266 picture allocation failed");
	}
	pic->pts = 0;

	const int srcBytesPerSample = info.bitDepth <= 8 ? 1 : 2;
	for (int plane = 0; plane < info.planeCount; ++plane) {
		const int planeWidth  = ceil_div(image.width, chroma_width_div(info, plane));
		const int planeHeight = ceil_div(image.height, chroma_height_div(info, plane));
		const int dstStride   = plane == 0 ? pic->stride : ceil_div(pic->stride, chroma_width_div(info, plane));
		copy_plane_to_uvg(
			image.planes[plane],
			pic->data[plane],
			dstStride,
			planeWidth,
			planeHeight,
			srcBytesPerSample,
			uvgBytesPerSample
		);
	}

	EncodedImage result;

	uvg_data_chunk* rawHeader = nullptr;
	uint32_t headerBytes = 0;
	if (api->encoder_headers(encoder.get(), &rawHeader, &headerBytes) == 0) {
		throw std::runtime_error("uvg266 encoder_headers failed");
	}
	std::unique_ptr<uvg_data_chunk, ChunkDeleter> header{rawHeader, ChunkDeleter{api}};
	append_chunks(result.hevcAnnexB, header.get());

	for (uvg_picture* input = pic.get();;) {
		uvg_data_chunk* rawChunk = nullptr;
		uint32_t frameBytes = 0;
		uvg_picture* rec = nullptr;
		uvg_picture* src = nullptr;
		uvg_frame_info frameInfo{};

		if (api->encoder_encode(encoder.get(), input, &rawChunk, &frameBytes, &rec, &src, &frameInfo) == 0) {
			throw std::runtime_error("uvg266 encoder_encode failed");
		}

		std::unique_ptr<uvg_data_chunk, ChunkDeleter> chunk{rawChunk, ChunkDeleter{api}};
		std::unique_ptr<uvg_picture, PictureDeleter> recGuard{
			rec != pic.get() ? rec : nullptr,
			PictureDeleter{api},
		};
		std::unique_ptr<uvg_picture, PictureDeleter> srcGuard{
			src != pic.get() ? src : nullptr,
			PictureDeleter{api},
		};

		if (chunk != nullptr) {
			append_chunks(result.hevcAnnexB, chunk.get());
		}
		if (rec != nullptr && !result.previewImage) {
			result.previewImage = copy_uvg_picture_to_raw(*rec, info, uvgBytesPerSample);
		}

		if (input == nullptr && chunk == nullptr) {
			break;
		}
		input = nullptr;
	}

	if (result.hevcAnnexB.empty()) {
		throw std::runtime_error("uvg266 produced an empty bitstream");
	}

	return result;
}

} // namespace codec_gui
