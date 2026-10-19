// codec_gui_uvg266.cpp
#include "codec_gui_x265.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <span>
#include <sstream>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

extern "C" {
#include <uvg266/uvg266.h>
}

namespace codec_gui {
namespace {

// The installed header defaults to UVG_BIT_DEPTH=8 even for a 10-bit
// library, and uvg_api_get ignores its argument in 0.8.x. Query the library's
// input validation instead. Upstream supports exactly 8- and 10-bit builds.
int library_bit_depth() {
    static const int depth = [] {
        const uvg_api* api = uvg_api_get(8);
        if (!api) throw std::runtime_error("uvg266 API unavailable");
        uvg_config* cfg = api->config_alloc();
        if (!cfg) throw std::runtime_error("uvg266 config allocation failed");
        if (!api->config_init(cfg)) {
            api->config_destroy(cfg);
            throw std::runtime_error("uvg266 config initialization failed");
        }
        const bool highDepth = api->config_parse(cfg, "input-bitdepth", "10") != 0;
        api->config_destroy(cfg);
        return highDepth ? 10 : 8;
    }();
    return depth;
}

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
			return {.chromaFormat=UVG_CSP_420, .bitDepth=8, .planeCount=3, .widthDiv={1, 2, 2}, .heightDiv={1, 2, 2}};
			case PixelFormat::YUV420P10LE:
				return {.chromaFormat=UVG_CSP_420, .bitDepth=10, .planeCount=3, .widthDiv={1, 2, 2}, .heightDiv={1, 2, 2}};
			case PixelFormat::YUV422P8:
				return {.chromaFormat=UVG_CSP_422, .bitDepth=8, .planeCount=3, .widthDiv={1, 2, 2}, .heightDiv={1, 1, 1}};
			case PixelFormat::YUV422P10LE:
				return {.chromaFormat=UVG_CSP_422, .bitDepth=10, .planeCount=3, .widthDiv={1, 2, 2}, .heightDiv={1, 1, 1}};
			case PixelFormat::YUV444P8:
			return {.chromaFormat=UVG_CSP_444, .bitDepth=8, .planeCount=3, .widthDiv={1, 1, 1}, .heightDiv={1, 1, 1}};
		case PixelFormat::YUV444P10LE:
			return {.chromaFormat=UVG_CSP_444, .bitDepth=10, .planeCount=3, .widthDiv={1, 1, 1}, .heightDiv={1, 1, 1}};
		case PixelFormat::Gray8:
			return {.chromaFormat=UVG_CSP_400, .bitDepth=8, .planeCount=1, .widthDiv={1, 1, 1}, .heightDiv={1, 1, 1}};
		case PixelFormat::Gray10LE:
			return {.chromaFormat=UVG_CSP_400, .bitDepth=10, .planeCount=1, .widthDiv={1, 1, 1}, .heightDiv={1, 1, 1}};
		case PixelFormat::YUV420P12LE: case PixelFormat::YUV420P14LE:
		case PixelFormat::YUV422P12LE: case PixelFormat::YUV422P14LE:
		case PixelFormat::YUV444P12LE: case PixelFormat::YUV444P14LE:
		case PixelFormat::Gray12LE: case PixelFormat::Gray14LE: break;
		default: break;
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

// uvg266 0.8.x accepts cfg->vui but its SPS writer hardcodes VUI absent.
// Only rewrite SPS units produced by this adapter, whose fixed progressive
// configuration ends in field_seq=0, vui_present=0, extension_present=0.
// Normal preview decoding verifies the result before publishing it; an
// incompatible serializer must fail, not acquire fabricated color hints.
void write_uvg_vui(EncodedImage& encoded, const RawImage& image) {
    using Bits = std::vector<uint8_t>;
    auto put = [](Bits& bits, unsigned value, unsigned count) {
        for (unsigned n=count; n; --n) bits.push_back((value>>(n-1))&1);
    };
    auto ue = [&](Bits& bits, unsigned value) {
        unsigned code=value+1, width=0;
        for (unsigned v=code; v; v>>=1) ++width;
        bits.insert(bits.end(),width-1,0); put(bits,code,width);
    };
    auto align = [](Bits& bits) {while (bits.size()%8) bits.push_back(0);};
    const bool chroma420=format_info(image.format).chromaFormat==UVG_CSP_420;
    Bits vui;
    put(vui,1,1); put(vui,0,5); // progressive; no interlacing or geometry hints
    put(vui,1,1); // colour_description_present
    put(vui,unsigned(image.color.primaries),8);
    put(vui,unsigned(image.color.transfer),8);
    put(vui,unsigned(image.color.matrix),8);
    put(vui,image.color.range==ColorRange::Full,1);
    put(vui,chroma420,1);
    if (chroma420) ue(vui,unsigned(image.color.chroma420Location.value_or(Chroma420SampleLocation::LeftCenter)));
    if (vui.size()%8) {put(vui,1,1); align(vui);}

    const auto& input=encoded.hevcAnnexB;
    auto byte=[&](size_t i) {return std::to_integer<uint8_t>(input[i]);};
    auto start=[&](size_t from) {
        for (size_t i=from; i+3<=input.size(); ++i) {
            if (byte(i)==0 && byte(i+1)==0) {
                if (byte(i+2)==1) return std::pair{i,size_t(3)};
                if (i+4<=input.size() && byte(i+2)==0 && byte(i+3)==1) return std::pair{i,size_t(4)};
            }
        }
        return std::pair{input.size(),size_t(0)};
    };
    std::vector<std::byte> output;
    bool found=false;
    auto current=start(0);
    output.insert(output.end(),input.begin(),input.begin()+current.first);
    while (current.second) {
        const size_t nal=current.first+current.second;
        const auto next=start(nal);
        if (nal+2>next.first) throw std::runtime_error("uvg266: truncated NAL header");
        if (((byte(nal+1)>>3)&31)!=15) {
            output.insert(output.end(),input.begin()+current.first,input.begin()+next.first);
        } else {
            found=true; // encoder_headers and encoder_encode both emit SPS
            Bits bits;
            unsigned zeros=0;
            for (size_t i=nal+2; i<next.first; ++i) {
                const unsigned v=byte(i);
                if (zeros==2 && v==3) {zeros=0; continue;}
                put(bits,v,8); zeros=v==0 ? zeros+1 : 0;
            }
            while (!bits.empty() && bits.back()==0) bits.pop_back();
            if (bits.size()<4 || bits[bits.size()-2] || bits[bits.size()-3] || bits[bits.size()-4])
                throw std::runtime_error("uvg266: unsupported SPS tail for color signaling");
            bits.resize(bits.size()-3); // absent VUI, absent extension, RBSP stop
            put(bits,1,1); ue(bits,unsigned(vui.size()/8-1)); align(bits);
            bits.insert(bits.end(),vui.begin(),vui.end());
            put(bits,0,1); put(bits,1,1); align(bits);
            output.insert(output.end(),input.begin()+current.first,input.begin()+nal+2);
            zeros=0;
            for (size_t i=0; i<bits.size(); i+=8) {
                unsigned v=0; for (size_t j=0; j<8; ++j) v=(v<<1)|bits[i+j];
                if (zeros==2 && v<=3) {output.push_back(std::byte{3}); zeros=0;}
                output.push_back(std::byte(v)); zeros=v==0 ? zeros+1 : 0;
            }
        }
        current=next;
    }
    if (!found) throw std::runtime_error("uvg266: SPS missing from encoded stream");
    encoded.hevcAnnexB=std::move(output);
    encoded.requiredStreamColor=image.color;
    if (chroma420 && !encoded.requiredStreamColor->chroma420Location)
        encoded.requiredStreamColor->chroma420Location=Chroma420SampleLocation::LeftCenter;
}

void copy_plane_to_uvg(
	const ImagePlane& src,
	uvg_pixel*        dst,
	const int         dstStrideSamples,
	const int         width,
	const int         height,
	const int         srcBytesPerSample,
	const int         dstBytesPerSample,
	const int         dstHeight
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
				const uint16_t sample = static_cast<uint16_t>(srcRow[static_cast<ptrdiff_t>(x * 2)]) |
				                        (static_cast<uint16_t>(srcRow[x * 2 + 1]) << 8u);
				dstRow[x] = static_cast<uint8_t>(std::min<uint16_t>(sample >> 2u, 255u));
			}
		} else if (srcBytesPerSample == 1 && dstBytesPerSample == 2) {
			for (int x = 0; x < width; ++x) {
				const uint16_t sample = static_cast<uint16_t>(srcRow[x]) << 2u;
				dstRow[static_cast<ptrdiff_t>(x * 2)] = static_cast<uint8_t>(sample & 0xffu);
				dstRow[x * 2 + 1] = static_cast<uint8_t>(sample >> 8u);
			}
		} else {
			throw std::runtime_error("unsupported uvg266 sample-size conversion");
		}
        // Match uvg266's CLI reader: initialize the full stride and the padded
        // picture height by edge extension. Filters read beyond visible pixels.
        for (int x = width; x < dstStrideSamples; ++x)
            std::memcpy(dstRow + x * dstBytesPerSample, dstRow + (width - 1) * dstBytesPerSample, dstBytesPerSample);
	}
    auto* bytes = reinterpret_cast<uint8_t*>(dst);
    const std::size_t stride = static_cast<std::size_t>(dstStrideSamples) * dstBytesPerSample;
    for (int y = height; y < dstHeight; ++y)
        std::memcpy(bytes + y * stride, bytes + (height - 1) * stride, stride);
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
			std::to_string(library_bit_depth()),
			{{.value=std::to_string(library_bit_depth()), .label=std::to_string(library_bit_depth()) + "-bit"}},
			"Bit depth compiled into this uvg266 library."
		),
		int_param("qp", "QP", "Rate Control", 48, {.min=0, .max=63, .step=1}, "Quantization parameter."),
		enum_param(
			"preset",
			"Preset",
			"Speed / Quality",
			"medium",
			{
				{.value="ultrafast", .label="Ultrafast"},
				{.value="superfast", .label="Superfast"},
				{.value="veryfast", .label="Veryfast"},
				{.value="faster", .label="Faster"},
				{.value="fast", .label="Fast"},
				{.value="medium", .label="Medium"},
				{.value="slow", .label="Slow"},
				{.value="slower", .label="Slower"},
			},
			"uvg266 preset, passed through uvg266's option parser."
		),
		int_param("rdo", "RDO level", "Analysis", 2, {.min=0, .max=2, .step=1}, "Rate-distortion optimization level."),
		bool_param("rdoq", "RDOQ", "Quantization", true, "Enable RD optimized quantization."),
		bool_param("sao", "SAO", "Filters", true, "Enable sample adaptive offset."),
		[] {
            auto p = bool_param("alf", "ALF", "Filters", false, "Unavailable: the installed uvg266 emits invalid ALF coefficients at RDO 0/1/2 and can crash on monochrome ALF.");
            p.enabledWhen = {{.parameter="rdo", .acceptedValues={}, .explanation=p.help}};
            return p;
        }(),
		bool_param("mip", "MIP", "Intra Tools", true, "Enable matrix weighted intra prediction."),
		bool_param("lfnst", "LFNST", "Intra Tools", true, "Enable low-frequency non-separable transform."),
		bool_param("isp", "ISP", "Intra Tools", false, "Disabled for still images: this libuvg266 build aborts in AVX2 SSD for non-square ISP blocks.", false),
	};
}

EncodedImage encode_uvg266_still_image(const RawImage& image, std::span<const EncoderParam> params) {
	const FormatInfo info = format_info(image.format);
	validate_image(image, info);
	const int uvgBitDepth = library_bit_depth();
	const int uvgBytesPerSample = uvgBitDepth > 8 ? 2 : 1;

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

    for (const auto& param : params)
        if (param.name=="alf" && std::get<bool>(param.value))
            throw std::invalid_argument("uvg266 ALF is unavailable: the installed library emits invalid coefficients at RDO 0/1/2; use ALF=false or another VVC encoder");
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

    if (cfg->lossless) throw std::invalid_argument("uvg266 lossless emits a legacy transquant-bypass syntax that the tested VVC decoder rejects; lossless is unavailable");

    cfg->alf_type = UVG_ALF_OFF; // presets must not implicitly enable the broken tool

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

	std::unique_ptr<uvg_encoder, EncoderDeleter> const encoder{api->encoder_open(cfg.get()), EncoderDeleter{api}};
	if (encoder == nullptr) {
		throw std::runtime_error("uvg266 encoder_open failed");
	}

	std::unique_ptr<uvg_picture, PictureDeleter> pic{
		api->picture_alloc_csp(info.chromaFormat, ceil_div(image.width, 8) * 8, ceil_div(image.height, 8) * 8),
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
			uvgBytesPerSample,
            ceil_div(pic->height, chroma_height_div(info, plane))
		);
	}

	EncodedImage result;

	uvg_data_chunk* rawHeader = nullptr;
	uint32_t headerBytes = 0;
	if (api->encoder_headers(encoder.get(), &rawHeader, &headerBytes) == 0) {
		throw std::runtime_error("uvg266 encoder_headers failed");
	}
	std::unique_ptr<uvg_data_chunk, ChunkDeleter> const header{rawHeader, ChunkDeleter{api}};
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

		std::unique_ptr<uvg_data_chunk, ChunkDeleter> const chunk{rawChunk, ChunkDeleter{api}};
		std::unique_ptr<uvg_picture, PictureDeleter> const recGuard{
			rec != pic.get() ? rec : nullptr,
			PictureDeleter{api},
		};
		std::unique_ptr<uvg_picture, PictureDeleter> const srcGuard{
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

    write_uvg_vui(result,image);
	return result;
}

} // namespace codec_gui
