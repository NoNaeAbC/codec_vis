// codec_gui_vvenc.cpp
#include "codec_gui_x265.hpp"

#include <algorithm>
#include <array>
#include <cstdarg>
#include <cstddef>
#include <cstdint>
#include <cstdio>
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
#include <vvenc/vvenc.h>
}

namespace codec_gui {
namespace {

struct VVenCEncoderDeleter {
    void operator()(vvencEncoder* enc) const noexcept {
        if (enc != nullptr) {
            (void)vvenc_encoder_close(enc);
        }
    }
};

struct VVenCYuvDeleter {
    void operator()(vvencYUVBuffer* yuv) const noexcept {
        if (yuv != nullptr) {
            vvenc_YUVBuffer_free(yuv, true);
        }
    }
};

struct VVenCAccessUnitDeleter {
    void operator()(vvencAccessUnit* au) const noexcept {
        if (au != nullptr) {
            vvenc_accessUnit_free(au, true);
        }
    }
};

using EncoderPtr = std::unique_ptr<vvencEncoder, VVenCEncoderDeleter>;
using YuvPtr     = std::unique_ptr<vvencYUVBuffer, VVenCYuvDeleter>;
using AuPtr      = std::unique_ptr<vvencAccessUnit, VVenCAccessUnitDeleter>;

thread_local std::string vvencLog;

void collect_vvenc_log(void*, int, const char* fmt, va_list args) {
    std::array<char, 1024> buffer{};
    va_list copy;
    va_copy(copy, args);
    const int written = std::vsnprintf(buffer.data(), buffer.size(), fmt, copy);
    va_end(copy);
    if (written <= 0) {
        return;
    }
    if (!vvencLog.empty() && vvencLog.back() != '\n') {
        vvencLog += '\n';
    }
    vvencLog.append(buffer.data(), static_cast<std::size_t>(std::min<int>(written, static_cast<int>(buffer.size() - 1))));
}

struct FormatInfo {
    vvencChromaFormat chromaFormat;
    int               bitDepth;
    int               planeCount;
    std::array<int, 3> widthDiv;
    std::array<int, 3> heightDiv;
};

[[nodiscard]] FormatInfo format_info(const PixelFormat format) {
    switch (format) {
        case PixelFormat::YUV420P8:
            return { VVENC_CHROMA_420, 8, 3, { 1, 2, 2 }, { 1, 2, 2 } };

        case PixelFormat::YUV420P10LE:
            return { VVENC_CHROMA_420, 10, 3, { 1, 2, 2 }, { 1, 2, 2 } };

        case PixelFormat::YUV422P8:
            return { VVENC_CHROMA_422, 8, 3, { 1, 2, 2 }, { 1, 1, 1 } };

        case PixelFormat::YUV422P10LE:
            return { VVENC_CHROMA_422, 10, 3, { 1, 2, 2 }, { 1, 1, 1 } };

        case PixelFormat::YUV444P8:
            return { VVENC_CHROMA_444, 8, 3, { 1, 1, 1 }, { 1, 1, 1 } };

        case PixelFormat::YUV444P10LE:
            return { VVENC_CHROMA_444, 10, 3, { 1, 1, 1 }, { 1, 1, 1 } };

        case PixelFormat::Gray8:
            return { VVENC_CHROMA_400, 8, 1, { 1, 1, 1 }, { 1, 1, 1 } };

        case PixelFormat::Gray10LE:
            return { VVENC_CHROMA_400, 10, 1, { 1, 1, 1 }, { 1, 1, 1 } };
		case PixelFormat::YUV420P12LE: case PixelFormat::YUV420P14LE:
		case PixelFormat::YUV422P12LE: case PixelFormat::YUV422P14LE:
		case PixelFormat::YUV444P12LE: case PixelFormat::YUV444P14LE:
		case PixelFormat::Gray12LE: case PixelFormat::Gray14LE: break;
    }

    throw std::runtime_error("unsupported pixel format");
}

[[nodiscard]] int ceil_div(const int value, const int divisor) {
    return (value + divisor - 1) / divisor;
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

[[nodiscard]] vvencPresetMode parse_preset(std::string_view preset) {
    if (preset == "faster") {
        return VVENC_FASTER;
    }
    if (preset == "fast") {
        return VVENC_FAST;
    }
    if (preset == "medium") {
        return VVENC_MEDIUM;
    }
    if (preset == "slow") {
        return VVENC_SLOW;
    }
    if (preset == "slower") {
        return VVENC_SLOWER;
    }
    if (preset == "medium_lowDecEnergy") {
        return VVENC_MEDIUM_LOWDECNRG;
    }

    throw std::runtime_error("invalid VVenC preset: " + std::string(preset));
}

[[nodiscard]] std::string find_string_param(
    std::span<const EncoderParam> params,
    std::string_view              name,
    std::string_view              defaultValue
) {
    for (const EncoderParam& param : params) {
        if (param.name == name) {
            if (const auto* value = std::get_if<std::string>(&param.value)) {
                return *value;
            }

            throw std::runtime_error("parameter '" + param.name + "' must be a string");
        }
    }

    return std::string(defaultValue);
}

[[nodiscard]] int64_t require_int_param(const EncoderParam& param) {
    if (const auto* value = std::get_if<int64_t>(&param.value)) {
        return *value;
    }
    throw std::runtime_error("parameter '" + param.name + "' must be an integer");
}

[[nodiscard]] bool require_bool_param(const EncoderParam& param) {
    if (const auto* value = std::get_if<bool>(&param.value)) {
        return *value;
    }
    throw std::runtime_error("parameter '" + param.name + "' must be a boolean");
}

void set_vvenc_param(vvenc_config& cfg, std::string_view name, std::string_view value) {
    const std::string nameString{name};
    const std::string valueString{value};

    const int ret = vvenc_set_param(&cfg, nameString.c_str(), valueString.c_str());

    if (ret == VVENC_PARAM_BAD_NAME) {
        throw std::runtime_error("unknown VVenC parameter: " + nameString);
    }

    if (ret == VVENC_PARAM_BAD_VALUE) {
        throw std::runtime_error(
            "invalid value for VVenC parameter '" + nameString + "': " + valueString
        );
    }

    if (ret != 0 && ret != VVENC_PARAM_INFO) {
        throw std::runtime_error("failed to set VVenC parameter: " + nameString);
    }
}

bool apply_vvenc_config_field(vvenc_config& cfg, const EncoderParam& param) {
    const std::string_view name = param.name;
    if (name == "IntraQPOffset") {
        cfg.m_intraQPOffset = static_cast<int>(require_int_param(param));
    } else if (name == "MIP") {
        cfg.m_MIP = require_bool_param(param);
    } else if (name == "ISP") {
        cfg.m_ISP = require_bool_param(param) ? 1 : 0;
    } else if (name == "MRL") {
        cfg.m_MRL = require_bool_param(param);
    } else if (name == "LFNST") {
        cfg.m_LFNST = require_bool_param(param) ? 1 : 0;
    } else if (name == "MTS") {
        cfg.m_MTS = static_cast<int>(require_int_param(param));
    } else if (name == "TS") {
        cfg.m_TS = require_bool_param(param) ? 1 : 0;
    } else if (name == "BDPCM") {
        cfg.m_useBDPCM = require_bool_param(param) ? 1 : 0;
    } else if (name == "LMChroma") {
        cfg.m_LMChroma = require_bool_param(param);
    } else if (name == "CbQpOffset") {
        cfg.m_chromaCbQpOffset = static_cast<int>(require_int_param(param));
    } else if (name == "CrQpOffset") {
        cfg.m_chromaCrQpOffset = static_cast<int>(require_int_param(param));
    } else if (name == "CbQpOffsetDualTree") {
        cfg.m_chromaCbQpOffsetDualTree = static_cast<int>(require_int_param(param));
    } else if (name == "CrQpOffsetDualTree") {
        cfg.m_chromaCrQpOffsetDualTree = static_cast<int>(require_int_param(param));
    } else if (name == "SliceChromaQPOffsetIntraOrPeriodic.0") {
        cfg.m_sliceChromaQpOffsetIntraOrPeriodic[0] = static_cast<int>(require_int_param(param));
    } else if (name == "SliceChromaQPOffsetIntraOrPeriodic.1") {
        cfg.m_sliceChromaQpOffsetIntraOrPeriodic[1] = static_cast<int>(require_int_param(param));
    } else if (name == "SAO") {
        cfg.m_bUseSAO = require_bool_param(param);
    } else if (name == "ALF") {
        cfg.m_alf = require_bool_param(param);
    } else if (name == "CCALF") {
        cfg.m_ccalf = require_bool_param(param);
    } else if (name == "LoopFilterDisable") {
        cfg.m_bLoopFilterDisable = require_bool_param(param);
    } else if (name == "LoopFilterBetaOffset_div2") {
        const int value = static_cast<int>(require_int_param(param));
        cfg.m_loopFilterBetaOffsetDiv2[0] = value;
        cfg.m_loopFilterBetaOffsetDiv2[1] = value;
        cfg.m_loopFilterBetaOffsetDiv2[2] = value;
    } else if (name == "LoopFilterTcOffset_div2") {
        const int value = static_cast<int>(require_int_param(param));
        cfg.m_loopFilterTcOffsetDiv2[0] = value;
        cfg.m_loopFilterTcOffsetDiv2[1] = value;
        cfg.m_loopFilterTcOffsetDiv2[2] = value;
    } else {
        return false;
    }
    return true;
}

void append_payload(std::vector<std::byte>& out, const vvencAccessUnit& au) {
    if (au.payloadUsedSize <= 0) {
        return;
    }

    const auto* first = reinterpret_cast<const std::byte*>(au.payload);
    out.insert(out.end(), first, first + au.payloadUsedSize);
}

void ensure_access_unit_payload(vvencAccessUnit& au, const int size) {
    if (au.payloadSize >= size) {
        return;
    }

    vvenc_accessUnit_free_payload(&au);
    vvenc_accessUnit_alloc_payload(&au, size);

    if (au.payload == nullptr || au.payloadSize < size) {
        throw std::runtime_error("failed to allocate VVenC access-unit payload");
    }
}

void checked_vvenc_call(
    vvencEncoder*     encoder,
    int               ret,
    std::string_view  operation
) {
    if (ret == VVENC_OK) {
        return;
    }

    std::string message = "VVenC ";
    message += operation;
    message += " failed";

    if (encoder != nullptr) {
        if (const char* lastError = vvenc_get_last_error(encoder);
            lastError != nullptr && lastError[0] != '\0') {
            message += ": ";
            message += lastError;
        }
    }

    if (const char* error = vvenc_get_error_msg(ret);
        error != nullptr && error[0] != '\0') {
        message += " [";
        message += error;
        message += "]";
    }
    if (!vvencLog.empty()) {
        message += ": ";
        message += vvencLog;
    }

    throw std::runtime_error(message);
}

void encode_call_collecting_payload(
    vvencEncoder*           encoder,
    vvencYUVBuffer*         input,
    vvencAccessUnit&        au,
    bool&                   done,
    std::vector<std::byte>& out
) {
    vvenc_accessUnit_reset(&au);

    int ret = vvenc_encode(encoder, input, &au, &done);

    if (ret == VVENC_NOT_ENOUGH_MEM) {
        const int required = std::max(au.payloadUsedSize, au.payloadSize * 2);
        ensure_access_unit_payload(au, required);

        vvenc_accessUnit_reset(&au);
        ret = vvenc_encode(encoder, nullptr, &au, &done);
    }

    checked_vvenc_call(encoder, ret, "encode");
    append_payload(out, au);
}

void validate_image(const RawImage& image, const FormatInfo& info) {
    if (image.width <= 0 || image.height <= 0) {
        throw std::runtime_error("image dimensions must be positive");
    }

    if (info.chromaFormat != VVENC_CHROMA_400 && info.chromaFormat != VVENC_CHROMA_420) {
        throw std::runtime_error("this VVenC build only supports monochrome and YUV420 still images");
    }

    if (info.chromaFormat == VVENC_CHROMA_420) {
        if ((image.width & 1) != 0 || (image.height & 1) != 0) {
            throw std::runtime_error("YUV420 images require even width and height");
        }
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
            static_cast<std::size_t>(src.strideBytes) *
            static_cast<std::size_t>(planeHeight);

        if (src.bytes.size() < requiredSize) {
            throw std::runtime_error("plane byte buffer is smaller than stride * height");
        }
    }
}

void copy_plane_u8_to_vvenc(
    const ImagePlane& src,
    vvencYUVPlane&    dst,
    const int         width,
    const int         height,
    const int         bitDepthShift
) {
    for (int y = 0; y < height; ++y) {
        const uint8_t* srcRow = src.bytes.data() + static_cast<std::size_t>(y) * src.strideBytes;
        int16_t*       dstRow = dst.ptr + static_cast<std::size_t>(y) * dst.stride;

        for (int x = 0; x < width; ++x) {
            int value = srcRow[x];
            if (bitDepthShift > 0) {
                value <<= bitDepthShift;
            } else if (bitDepthShift < 0) {
                const int shiftRight = -bitDepthShift;
                value = (value + (1 << (shiftRight - 1))) >> shiftRight;
            }
            dstRow[x] = static_cast<int16_t>(value);
        }
    }
}

void copy_plane_u16le_to_vvenc(
    const ImagePlane& src,
    vvencYUVPlane&    dst,
    const int         width,
    const int         height,
    const int         sourceBitDepth,
    const int         bitDepthShift
) {
    const uint16_t maxValue = static_cast<uint16_t>((1u << sourceBitDepth) - 1u);

    for (int y = 0; y < height; ++y) {
        const uint8_t* srcRow = src.bytes.data() + static_cast<std::size_t>(y) * src.strideBytes;
        int16_t*       dstRow = dst.ptr + static_cast<std::size_t>(y) * dst.stride;

        for (int x = 0; x < width; ++x) {
            const auto lo = static_cast<uint16_t>(srcRow[x * 2 + 0]);
            const auto hi = static_cast<uint16_t>(srcRow[x * 2 + 1]);
            int sample = std::min<int>(static_cast<int>((hi << 8u) | lo), maxValue);

            if (bitDepthShift > 0) {
                sample <<= bitDepthShift;
            } else if (bitDepthShift < 0) {
                const int shiftRight = -bitDepthShift;
                sample = (sample + (1 << (shiftRight - 1))) >> shiftRight;
            }

            dstRow[x] = static_cast<int16_t>(sample);
        }
    }
}

void copy_image_to_vvenc_yuv(
    const RawImage& image,
    const FormatInfo& info,
    const int targetBitDepth,
    vvencYUVBuffer& yuv
) {
    const int bitDepthShift = targetBitDepth - info.bitDepth;

    for (int plane = 0; plane < info.planeCount; ++plane) {
        const int planeWidth  = ceil_div(image.width, info.widthDiv[plane]);
        const int planeHeight = ceil_div(image.height, info.heightDiv[plane]);

        if (info.bitDepth <= 8) {
            copy_plane_u8_to_vvenc(
                image.planes[plane],
                yuv.planes[plane],
                planeWidth,
                planeHeight,
                bitDepthShift
            );
        } else {
            copy_plane_u16le_to_vvenc(
                image.planes[plane],
                yuv.planes[plane],
                planeWidth,
                planeHeight,
                info.bitDepth,
                bitDepthShift
            );
        }
    }

    yuv.sequenceNumber = 0;
    yuv.cts            = 0;
    yuv.ctsValid       = true;
}

void force_still_image_config(vvenc_config& cfg, const RawImage& image, const FormatInfo& info) {
    cfg.m_SourceWidth       = image.width;
    cfg.m_SourceHeight      = image.height;
    cfg.m_FrameRate         = 1;
    cfg.m_FrameScale        = 1;
    cfg.m_TicksPerSecond    = VVENC_TICKS_PER_SEC_DEF;
    cfg.m_framesToBeEncoded = 1;

    cfg.m_inputBitDepth[0]    = info.bitDepth;
    cfg.m_inputBitDepth[1]    = info.bitDepth;
    cfg.m_internChromaFormat = info.chromaFormat;

    cfg.m_IntraPeriod         = 1;
    cfg.m_IntraPeriodSec      = 0;
    cfg.m_GOPSize             = 1;
    cfg.m_DecodingRefreshType = VVENC_DRT_IDR_NO_RADL;
    cfg.m_poc0idr             = true;
    cfg.m_rewriteParamSets    = true;

    cfg.m_RCNumPasses         = 1;
    cfg.m_verbosity           = VVENC_SILENT;
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
    std::string help,
    bool        relevant = true,
    bool        directNumericInput = false
) {
    EncoderParamInfo p;
    p.name                  = std::move(name);
    p.label                 = std::move(label);
    p.group                 = std::move(group);
    p.kind                  = ParamKind::Int;
    p.defaultValue          = defaultValue;
    p.intRange              = range;
    p.help                  = std::move(help);
    p.relevantForStillImage = relevant;
    p.directNumericInput    = directNumericInput;
    return p;
}

[[nodiscard]] EncoderParamInfo enabled_when(
    EncoderParamInfo param,
    std::string dependency,
    std::vector<std::string> values,
    std::string explanation
) {
    param.enabledWhen.push_back({std::move(dependency), std::move(values), std::move(explanation)});
    return param;
}

[[nodiscard]] EncoderParamInfo enum_param(
    std::string            name,
    std::string            label,
    std::string            group,
    std::string            defaultValue,
    std::vector<EnumValue> values,
    std::string            help,
    bool                   relevant = true
) {
    EncoderParamInfo p;
    p.name                  = std::move(name);
    p.label                 = std::move(label);
    p.group                 = std::move(group);
    p.kind                  = ParamKind::Enum;
    p.defaultValue          = std::move(defaultValue);
    p.enumValues            = std::move(values);
    p.help                  = std::move(help);
    p.relevantForStillImage = relevant;
    return p;
}

[[nodiscard]] EncoderParamInfo string_param(
    std::string name,
    std::string label,
    std::string group,
    std::string defaultValue,
    std::string help,
    bool        relevant = true
) {
    EncoderParamInfo p;
    p.name                  = std::move(name);
    p.label                 = std::move(label);
    p.group                 = std::move(group);
    p.kind                  = ParamKind::String;
    p.defaultValue          = std::move(defaultValue);
    p.help                  = std::move(help);
    p.relevantForStillImage = relevant;
    return p;
}

[[nodiscard]] bool is_structural_param(std::string_view name) {
    static constexpr std::array<std::string_view, 16> structural = {
        "size",
        "sourcewidth",
        "sourceheight",
        "framerate",
        "framescale",
        "fps",
        "tickspersec",
        "inputbitdepth",
        "internalbitdepth",
        "framestobeencoded",
        "intraperiod",
        "refreshsec",
        "gopsize",
        "decodingrefreshtype",
        "profile",
        "segment",
    };

    return std::ranges::find(structural, name) != structural.end();
}

} // namespace

std::vector<EncoderParamInfo> query_vvenc_parameters() {
    return {
        enum_param(
            "preset",
            "Preset",
            "Speed / Quality",
            "medium",
            {
                { "faster",              "Faster" },
                { "fast",                "Fast" },
                { "medium",              "Medium" },
                { "slow",                "Slow" },
                { "slower",              "Slower" },
                { "medium_lowDecEnergy", "Medium, low decoder energy" },
            },
            "VVenC preset. This is applied before all other user parameters."
        ),

		enum_param(
			"rate-control",
			"Rate-control mode",
			"Rate Control",
			"qp",
			{{"qp", "Fixed QP"}, {"bitrate", "Target bitrate"}},
			"Select exactly one VVenC rate-control strategy. The inactive value is not submitted."
		),

		enabled_when(int_param(
            "qp",
            "QP",
            "Rate Control",
            32,
            { 0, 63, 1 },
            "Fixed QP for the key picture. Lower values improve quality and increase size."
		), "rate-control", {"qp"}, "QP is available only in Fixed QP mode."),

		enabled_when(int_param(
            "bitrate",
            "Target bitrate",
            "Rate Control",
            0,
            { 0, 2'000'000'000, 1'000 },
            "Target bitrate in bit/s.",
			true,
			true
		), "rate-control", {"bitrate"}, "Bitrate is available only in Target bitrate mode."),

        int_param(
            "passes",
            "Rate-control passes",
            "Rate Control",
            1,
            { 1, 2, 1 },
            "Number of rate-control passes. For single-image encoding, 1 is normally the useful setting."
            ,
            false
        ),

        bool_param(
            "qpa",
            "Perceptual QP adaptation",
            "Perceptual",
            true,
            "Enable perceptually motivated input-adaptive QP modification."
        ),

        int_param(
            "IntraQPOffset",
            "Intra QP offset",
            "Intra",
            0,
            { -12, 12, 1 },
            "QP offset for intra slices. For a still image, the only coded picture is intra."
        ),

        bool_param(
            "MIP",
            "Matrix-based intra prediction",
            "Intra Tools",
            true,
            "Enable matrix-based intra prediction."
        ),

        bool_param(
            "ISP",
            "Intra sub-partitions",
            "Intra Tools",
            true,
            "Enable intra sub-partitions."
        ),

        bool_param(
            "MRL",
            "Multiple reference lines",
            "Intra Tools",
            true,
            "Enable multiple-reference-line intra prediction."
        ),

        bool_param(
            "LFNST",
            "LFNST",
            "Intra Tools",
            true,
            "Enable low-frequency non-separable transform."
        ),

        int_param(
            "MTS",
            "Multiple transform selection",
            "Transform",
            1,
            { 0, 3, 1 },
            "Multiple transform selection mode."
        ),

        bool_param(
            "TS",
            "Transform skip",
            "Transform",
            false,
            "Enable transform skip. Often relevant for screen content and sharp synthetic material."
        ),

        bool_param(
            "BDPCM",
            "BDPCM",
            "Screen Content",
            false,
            "Enable block DPCM. Mostly useful for screen-content-like images."
        ),

        bool_param(
            "LMChroma",
            "LM chroma prediction",
            "Chroma",
            true,
            "Enable cross-component linear-model chroma prediction."
        ),

        int_param(
            "CbQpOffset",
            "Cb QP offset",
            "Chroma",
            0,
            { -12, 12, 1 },
            "Global Cb chroma QP offset."
        ),

        int_param(
            "CrQpOffset",
            "Cr QP offset",
            "Chroma",
            0,
            { -12, 12, 1 },
            "Global Cr chroma QP offset."
        ),

        int_param(
            "CbQpOffsetDualTree",
            "Cb QP offset, dual tree",
            "Chroma",
            0,
            { -12, 12, 1 },
            "Cb QP offset used when dual-tree coding is active."
        ),

        int_param(
            "CrQpOffsetDualTree",
            "Cr QP offset, dual tree",
            "Chroma",
            0,
            { -12, 12, 1 },
            "Cr QP offset used when dual-tree coding is active."
        ),

        int_param(
            "SliceChromaQPOffsetIntraOrPeriodic.0",
            "Intra slice Cb QP offset",
            "Chroma",
            0,
            { -12, 12, 1 },
            "Slice-level Cb QP offset for intra pictures."
        ),

        int_param(
            "SliceChromaQPOffsetIntraOrPeriodic.1",
            "Intra slice Cr QP offset",
            "Chroma",
            0,
            { -12, 12, 1 },
            "Slice-level Cr QP offset for intra pictures."
        ),

        bool_param(
            "SAO",
            "SAO",
            "Filters",
            true,
            "Enable sample adaptive offset."
        ),

        bool_param(
            "ALF",
            "ALF",
            "Filters",
            true,
            "Enable adaptive loop filter."
        ),

        bool_param(
            "CCALF",
            "CCALF",
            "Filters",
            true,
            "Enable cross-component adaptive loop filter."
        ),

        bool_param(
            "LoopFilterDisable",
            "Disable deblocking",
            "Filters",
            false,
            "Disable the deblocking loop filter."
        ),

        int_param(
            "LoopFilterBetaOffset_div2",
            "Deblocking beta offset / 2",
            "Filters",
            0,
            { -12, 12, 1 },
            "Deblocking beta offset divided by two."
        ),

        int_param(
            "LoopFilterTcOffset_div2",
            "Deblocking Tc offset / 2",
            "Filters",
            0,
            { -12, 12, 1 },
            "Deblocking Tc offset divided by two."
        ),

        enum_param(
            "hdr",
            "HDR mode",
            "VUI / SEI",
            "off",
            {
                { "off",       "Off" },
                { "pq",        "PQ" },
                { "hlg",       "HLG" },
                { "pq_2020",   "PQ + BT.2020" },
                { "hlg_2020",  "HLG + BT.2020" },
                { "sdr_709",   "SDR BT.709" },
                { "sdr_2020",  "SDR BT.2020" },
                { "sdr_470bg", "SDR BT.470 B/G" },
            },
			"HDR / transfer / colorimetry signaling mode.",
			false
        ),

        int_param(
            "threads",
            "Threads",
            "Execution",
            -1,
            { -1, 256, 1 },
            "Number of worker threads. -1 lets VVenC choose automatically."
        ),

        int_param(
            "tiles",
            "Tiles",
            "Execution",
            0,
            { 0, 4096, 1 },
            "Use VVenC's tile option format through string parameters if multi-dimensional tile control is needed.",
            false
        ),

        string_param(
            "additional",
            "Additional VVenC option",
            "Expert",
            "",
            "Reserved GUI escape hatch. This implementation intentionally does not parse this field.",
            false
        ),
    };
}

codec_gui::EncodedImage encode_vvenc_still_image(
    const RawImage& image,
    std::span<const EncoderParam> params
) {
    const FormatInfo info = format_info(image.format);
    validate_image(image, info);

    vvenc_config cfg{};
    vvencLog.clear();

    checked_vvenc_call(
        nullptr,
        vvenc_init_default(
            &cfg,
            image.width,
            image.height,
            1,
            0,
            32,
            parse_preset(find_string_param(params, "preset", "medium"))
        ),
        "init_default"
    );

    force_still_image_config(cfg, image, info);
    vvenc_set_msg_callback(&cfg, nullptr, collect_vvenc_log);

    for (const EncoderParam& param : params) {
		if (param.name == "preset" || param.name == "rate-control") {
            continue;
        }

        if (is_structural_param(param.name) || param.name == "pass" || param.name == "passes") {
            throw std::runtime_error(
                "parameter '" + param.name + "' is controlled by encode_vvenc_still_image"
            );
        }

        if (!apply_vvenc_config_field(cfg, param)) {
            set_vvenc_param(cfg, param.name, param_value_to_string(param.value));
        }
    }

    EncoderPtr encoder{ vvenc_encoder_create() };
    if (encoder == nullptr) {
        throw std::runtime_error("vvenc_encoder_create failed");
    }

    checked_vvenc_call(encoder.get(), vvenc_encoder_open(encoder.get(), &cfg), "open");
    checked_vvenc_call(encoder.get(), vvenc_init_pass(encoder.get(), 0, nullptr), "init_pass");

    YuvPtr yuv{ vvenc_YUVBuffer_alloc() };
    if (yuv == nullptr) {
        throw std::runtime_error("vvenc_YUVBuffer_alloc failed");
    }

    vvenc_YUVBuffer_alloc_buffer(yuv.get(), info.chromaFormat, image.width, image.height);
    if (yuv->planes[0].ptr == nullptr) {
        throw std::runtime_error("vvenc_YUVBuffer_alloc_buffer failed");
    }

    copy_image_to_vvenc_yuv(image, info, cfg.m_internalBitDepth[0], *yuv);

    AuPtr au{ vvenc_accessUnit_alloc() };
    if (au == nullptr) {
        throw std::runtime_error("vvenc_accessUnit_alloc failed");
    }

    ensure_access_unit_payload(*au, std::max(1 << 20, image.width * image.height * 8));

    EncodedImage result;

    vvenc_accessUnit_reset(au.get());
    int headerRet = vvenc_get_headers(encoder.get(), au.get());
    if (headerRet == VVENC_NOT_ENOUGH_MEM) {
        ensure_access_unit_payload(*au, std::max(au->payloadUsedSize, au->payloadSize * 2));
        vvenc_accessUnit_reset(au.get());
        headerRet = vvenc_get_headers(encoder.get(), au.get());
    }
    checked_vvenc_call(encoder.get(), headerRet, "get_headers");
    append_payload(result.hevcAnnexB, *au);

    bool done = false;
    encode_call_collecting_payload(encoder.get(), yuv.get(), *au, done, result.hevcAnnexB);

    while (!done) {
        encode_call_collecting_payload(encoder.get(), nullptr, *au, done, result.hevcAnnexB);
    }

    if (result.hevcAnnexB.empty()) {
        throw std::runtime_error("VVenC produced an empty bitstream");
    }

    return result;
}
} // namespace codec_gui
