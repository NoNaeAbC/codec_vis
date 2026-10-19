#include "codec_gui_jpegxs.hpp"
#include "gui/raw_image_utils.hpp"

#include <SvtJpegxsEnc.h>
#include <SvtJpegxsDec.h>

#include <algorithm>
#include <cmath>
#include <chrono>
#include <cstring>
#include <limits>
#include <sstream>
#include <stdexcept>

namespace codec_gui {
namespace {
using namespace gui;
constexpr uint64_t apiMajor = SVT_JPEGXS_API_VER_MAJOR;
constexpr uint64_t apiMinor = SVT_JPEGXS_API_VER_MINOR;

void checked(SvtJxsErrorType_t error, const char* operation) {
    if (error == SvtJxsErrorNone) return;
    std::ostringstream text;
    text << "JPEG XS " << operation << " failed (SVT error 0x" << std::hex << static_cast<uint32_t>(error) << ')';
    throw std::runtime_error(text.str());
}
template<class T> T value(std::span<const EncoderParam> params, const std::string& key, T fallback) {
    for (const auto& p : params) if (p.name == key) {
        if (const auto* v = std::get_if<T>(&p.value)) return *v;
        throw std::invalid_argument("JPEG XS parameter has incorrect type: " + key);
    }
    return fallback;
}
uint32_t integer(std::span<const EncoderParam> params, const std::string& key, int fallback, int low, int high) {
    const int64_t n = value<int64_t>(params, key, fallback);
    if (n < low || n > high) throw std::invalid_argument("JPEG XS " + key + " is outside its supported range");
    return static_cast<uint32_t>(n);
}
ColourFormat_t colour_format(PixelFormat format) {
    if (is_420(format)) return COLOUR_FORMAT_PLANAR_YUV420;
    if (is_422(format)) return COLOUR_FORMAT_PLANAR_YUV422;
    if (is_444(format)) return COLOUR_FORMAT_PLANAR_YUV444;
    throw std::invalid_argument("JPEG XS encoding requires planar YUV 4:2:0, 4:2:2, or 4:4:4; explicitly convert the input format");
}
PixelFormat decoded_format(const svt_jpeg_xs_image_config_t& cfg) {
    const int d = cfg.bit_depth;
    if (d != 8 && d != 10 && d != 12) throw std::runtime_error("JPEG XS preview supports 8-, 10-, and 12-bit streams");
    if (cfg.format == COLOUR_FORMAT_PLANAR_YUV420) return d == 8 ? PixelFormat::YUV420P8 : d == 10 ? PixelFormat::YUV420P10LE : PixelFormat::YUV420P12LE;
    if (cfg.format == COLOUR_FORMAT_PLANAR_YUV422) return d == 8 ? PixelFormat::YUV422P8 : d == 10 ? PixelFormat::YUV422P10LE : PixelFormat::YUV422P12LE;
    if (cfg.format == COLOUR_FORMAT_PLANAR_YUV444) return d == 8 ? PixelFormat::YUV444P8 : d == 10 ? PixelFormat::YUV444P10LE : PixelFormat::YUV444P12LE;
    throw std::runtime_error("JPEG XS preview requires a three-component YUV codestream; RGB/alpha/monochrome streams are unsupported");
}
} // namespace

std::vector<EncoderParamInfo> query_jpegxs_parameters() {
    return {
        {.name="lossless", .label="Lossless", .group="Rate Control", .kind=ParamKind::Bool, .defaultValue=false,
         .help="Use the reversible MLS.12 coding path. Bitrate is ignored; output size depends on image content."},
        {.name="bpp", .label="Bits per pixel", .group="Rate Control", .kind=ParamKind::Float, .defaultValue=double{6.0},
         .floatRange=FloatRange{.min=1.0,.max=36.0,.step=0.25},
         .help="Target total coded bits per luma pixel, including all components. This is a compressed-size target, not bits per component. Very low budgets may be too small for headers or image content.",
         .enabledWhen={{.parameter="lossless",.acceptedValues={"false"},.explanation="Lossless output size is content-dependent."}}},
        {.name="quantization", .label="Quantization", .group="Coding", .kind=ParamKind::Enum, .defaultValue=std::string{"deadzone"},
         .enumValues={{.value="deadzone",.label="Deadzone"},{.value="uniform",.label="Uniform"}},
         .help="Select the JPEG XS coefficient truncation method.",
         .enabledWhen={{.parameter="lossless",.acceptedValues={"false"},.explanation="Lossless coding does not truncate coefficients."}}},
        {.name="slice-height", .label="Slice height", .group="Coding", .kind=ParamKind::Int, .defaultValue=int64_t{16},
         .intRange=IntRange{.min=4,.max=1024,.step=4}, .help="Slice height in luma rows, in multiples of four. Values above image height use one slice."},
        {.name="cpu-profile", .label="CPU profile", .group="Execution", .kind=ParamKind::Enum, .defaultValue=std::string{"latency"},
         .enumValues={{.value="latency",.label="Low latency"},{.value="cpu",.label="Low CPU usage"}}, .help="Choose SVT-JPEG-XS execution strategy."},
        {.name="threads", .label="Threads", .group="Execution", .kind=ParamKind::Int, .defaultValue=int64_t{4},
         .intRange=IntRange{.min=1,.max=64,.step=1}, .help="Encoder worker thread count."},
    };
}

EncodedImage encode_jpegxs_still_image(const RawImage& image, std::span<const EncoderParam> params) {
    for (const auto& param : params) {
        if (param.name != "lossless" && param.name != "bpp" && param.name != "quantization" && param.name != "cpu-profile" && param.name != "slice-height" && param.name != "threads")
            throw std::invalid_argument("JPEG XS unknown parameter: " + param.name);
    }
    const int depth = bit_depth(image.format);
    if (depth != 8 && depth != 10 && depth != 12) throw std::invalid_argument("JPEG XS supports 8-, 10-, and 12-bit input in this application");
    if (image.width < 64 || image.height < 4 || image.width > 65535 || image.height > 65535)
        throw std::invalid_argument("JPEG XS requires width 64..65535 and height 4..65535 for the selected decomposition");
    svt_jpeg_xs_encoder_api_t encoder{};
    checked(svt_jpeg_xs_encoder_load_default_parameters(apiMajor, apiMinor, &encoder), "default parameters");
    encoder.source_width = image.width;
    encoder.source_height = image.height;
    encoder.input_bit_depth = depth;
    encoder.colour_format = colour_format(image.format);
    if ((is_420(image.format) || is_422(image.format)) && (image.width % 2 || (is_420(image.format) && image.height % 2)))
        throw std::invalid_argument("JPEG XS subsampled input must be chroma-aligned; use the application padding transform");
    encoder.threads_num = integer(params, "threads", 4, 1, 64);
    encoder.slice_height = integer(params, "slice-height", 16, 4, 1024);
    if (encoder.slice_height % 4) throw std::invalid_argument("JPEG XS slice height must be a multiple of four");
    encoder.lossless_enable = value<bool>(params, "lossless", false);
    // The MLS.12 lossless profile requires raw-mode coding disabled.
    encoder.coding_raw_disable = encoder.lossless_enable ? 1 : 0;
    const auto quantization = value<std::string>(params, "quantization", "deadzone");
    if (quantization != "deadzone" && quantization != "uniform") throw std::invalid_argument("JPEG XS unknown quantization mode");
    encoder.quantization = quantization == "uniform" ? 1 : 0;
    const auto profile = value<std::string>(params, "cpu-profile", "latency");
    if (profile != "latency" && profile != "cpu") throw std::invalid_argument("JPEG XS unknown CPU profile");
    encoder.cpu_profile = profile == "cpu" ? 1 : 0;
    const double bpp = value<double>(params, "bpp", 6.0);
    if (!std::isfinite(bpp) || bpp < 1 || bpp > 36) throw std::invalid_argument("JPEG XS target bitrate must be 1..36 bits per pixel");
    encoder.bpp_numerator = static_cast<uint32_t>(std::lround(bpp * 1000));
    encoder.bpp_denominator = 1000;
    encoder.verbose = VERBOSE_NONE;
    // Fixed 5H/2V decomposition is valid for the exposed minimum geometry.
    encoder.ndecomp_h = 5;
    encoder.ndecomp_v = 2;
    svt_jpeg_xs_image_config_t config{};
    uint32_t allocation = 0;
    checked(svt_jpeg_xs_encoder_get_image_config(apiMajor, apiMinor, &encoder, &config, &allocation), "image configuration");
    std::vector<uint8_t> buffer(allocation);
    svt_jpeg_xs_frame_t input{};
    for (int p=0; p<3; ++p) {
        if (!plane_available(image,p) || image.planes[p].strideBytes < plane_width(image,p) * bytes_per_sample(image.format) || image.planes[p].strideBytes % bytes_per_sample(image.format))
            throw std::invalid_argument("JPEG XS input plane is incomplete or has an invalid sample stride");
        if (image.planes[p].bytes.size() > std::numeric_limits<uint32_t>::max()) throw std::invalid_argument("JPEG XS input plane exceeds API buffer limits");
        input.image.data_yuv[p] = const_cast<uint8_t*>(image.planes[p].bytes.data());
        input.image.stride[p] = image.planes[p].strideBytes / bytes_per_sample(image.format);
        input.image.alloc_size[p] = static_cast<uint32_t>(image.planes[p].bytes.size());
    }
    input.bitstream.buffer = buffer.data();
    input.bitstream.allocation_size = allocation;
    struct Close { svt_jpeg_xs_encoder_api_t* api; ~Close() { if (api->private_ptr) svt_jpeg_xs_encoder_close(api); } } close{&encoder};
    checked(svt_jpeg_xs_encoder_init(apiMajor, apiMinor, &encoder), "encoder initialization");
    checked(svt_jpeg_xs_encoder_send_picture(&encoder, &input, 1), "send picture");
    svt_jpeg_xs_frame_t output{};
    checked(svt_jpeg_xs_encoder_get_packet(&encoder, &output, 1), "receive codestream");
    if (!output.bitstream.buffer || output.bitstream.used_size == 0 || output.bitstream.used_size > allocation)
        throw std::runtime_error("JPEG XS encoder returned an empty or oversized codestream");
    EncodedImage result;
    const auto* first = reinterpret_cast<const std::byte*>(output.bitstream.buffer);
    result.hevcAnnexB.assign(first, first + output.bitstream.used_size);
    // Bare JPEG XS carries component sampling/precision, but no CICP color
    // description. Preserve the known application color context explicitly.
    result.codedColor = image.color;
    return result;
}

std::shared_ptr<const RawImage> decode_jpegxs_image(const EncodedImage& encoded,unsigned threads,double* codecSeconds) {
    if (encoded.hevcAnnexB.empty() || encoded.hevcAnnexB.size() > std::numeric_limits<uint32_t>::max())
        throw std::invalid_argument("JPEG XS codestream is empty or exceeds API buffer limits");
    const auto* bytes = reinterpret_cast<const uint8_t*>(encoded.hevcAnnexB.data());
    svt_jpeg_xs_image_config_t config{};
    uint32_t frameSize = 0;
    checked(svt_jpeg_xs_decoder_get_single_frame_size(bytes, encoded.hevcAnnexB.size(), &config, &frameSize, 0), "codestream validation");
    if (frameSize != encoded.hevcAnnexB.size()) throw std::invalid_argument("JPEG XS preview requires exactly one complete codestream");
    const PixelFormat format = decoded_format(config);
    if (config.components_num != 3) throw std::runtime_error("JPEG XS decoder reported unsupported component count");
    auto image = std::make_shared<RawImage>();
    image->width = static_cast<int>(config.width);
    image->height = static_cast<int>(config.height);
    image->format = format;
    if (encoded.codedColor) image->color = *encoded.codedColor;
    svt_jpeg_xs_frame_t input{};
    for (int p=0; p<3; ++p) {
        const int w = plane_width(*image,p), h = plane_height(*image,p);
        if (config.components[p].width != static_cast<uint32_t>(w) || config.components[p].height != static_cast<uint32_t>(h))
            throw std::runtime_error("JPEG XS decoder reported inconsistent component dimensions");
        image->planes[p].strideBytes = w * bytes_per_sample(format);
        const std::size_t byteCount = static_cast<std::size_t>(image->planes[p].strideBytes) * h;
        if (byteCount > std::numeric_limits<uint32_t>::max()) throw std::runtime_error("JPEG XS decoded plane exceeds API buffer limits");
        image->planes[p].bytes.resize(byteCount);
        input.image.data_yuv[p] = image->planes[p].bytes.data();
        input.image.stride[p] = w;
        input.image.alloc_size[p] = static_cast<uint32_t>(image->planes[p].bytes.size());
    }
    input.bitstream.buffer = const_cast<uint8_t*>(bytes);
    input.bitstream.used_size = frameSize;
    input.bitstream.allocation_size = frameSize;
    svt_jpeg_xs_decoder_api_t decoder{};
    decoder.use_cpu_flags = CPU_FLAGS_ALL;
    decoder.threads_num = std::clamp(threads,1u,64u);
    decoder.verbose = VERBOSE_NONE;
    struct Close { svt_jpeg_xs_decoder_api_t* api; ~Close() { if (api->private_ptr) svt_jpeg_xs_decoder_close(api); } } close{&decoder};
    checked(svt_jpeg_xs_decoder_init(apiMajor, apiMinor, &decoder, bytes, frameSize, &config), "decoder initialization");
    const auto codecStart = std::chrono::steady_clock::now();
    checked(svt_jpeg_xs_decoder_send_frame(&decoder, &input, 1), "send codestream");
    svt_jpeg_xs_frame_t output{};
    checked(svt_jpeg_xs_decoder_get_frame(&decoder, &output, 1), "receive decoded frame");
    if(codecSeconds) *codecSeconds = std::chrono::duration<double>(std::chrono::steady_clock::now()-codecStart).count();
    return image;
}
} // namespace codec_gui
