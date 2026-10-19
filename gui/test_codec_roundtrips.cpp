#include "encoder_backends.hpp"
#include "encoder_param_state.hpp"
#include "raw_image_utils.hpp"
#include "raw_image_conversion.hpp"

#include <algorithm>
#include <fstream>
#include <cstdlib>
#include <sstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <variant>

using namespace codec_gui;
using namespace codec_gui::gui;

namespace {
std::string clean(std::string s) {
    for (char& c : s) if (c == '\n' || c == '\r' || c == '\t') c = ' ';
    return s;
}
PixelFormat format_for(int depth, const std::string& chroma) {
    if (chroma == "444") return pixel_format_for(depth, true);
    if (chroma == "420") return pixel_format_for(depth, false);
    if (chroma == "422") {
        if (depth == 8) return PixelFormat::YUV422P8;
        if (depth == 10) return PixelFormat::YUV422P10LE;
        if (depth == 12) return PixelFormat::YUV422P12LE;
        if (depth == 14) return PixelFormat::YUV422P14LE;
    }
    if (chroma == "400") {
        if (depth == 8) return PixelFormat::Gray8;
        if (depth == 10) return PixelFormat::Gray10LE;
        if (depth == 12) return PixelFormat::Gray12LE;
        if (depth == 14) return PixelFormat::Gray14LE;
    }
    throw std::runtime_error("unrepresentable test pixel format");
}
std::string option(const char* name, std::string fallback = {}) {
    const char* value = std::getenv(name);
    return value ? std::string(value) : fallback;
}
void write_raw(const RawImage& image, const std::string& path) {
    std::ofstream file(path, std::ios::binary);
    for (int p=0; p<plane_count(image.format); ++p)
        for (int y=0; y<plane_height(image,p); ++y)
            file.write(reinterpret_cast<const char*>(image.planes[p].bytes.data() + static_cast<std::size_t>(y) * image.planes[p].strideBytes), plane_width(image,p) * bytes_per_sample(image.format));
    if (!file) throw std::runtime_error("raw sample export failed");
}
RawImage pattern(int depth, std::string chroma, int w, int h) {
    RawImage image;
    image.width = w; image.height = h; image.format = format_for(depth, chroma);
    image.color = {.primaries=ColorPrimaries::BT709, .transfer=TransferCharacteristics::SRGB,
                   .matrix=MatrixCoefficients::BT709, .range=ColorRange::Limited};
    if (chroma == "420") image.color.chroma420Location = Chroma420SampleLocation::LeftCenter;
    if (option("CODEC_VIS_TEST_COLOR") == "full") image.color.range = ColorRange::Full;
    if (option("CODEC_VIS_TEST_COLOR") == "pq2020") {
        image.color.primaries = ColorPrimaries::BT2020;
        image.color.transfer = TransferCharacteristics::PQ;
        image.color.matrix = MatrixCoefficients::BT2020NonConstant;
    }
    const std::string signal = option("CODEC_VIS_TEST_PATTERN", "ramp");
    if (signal != "ramp" && signal != "noise" && signal != "flat" && signal != "edges") throw std::runtime_error("unknown test pattern");
    uint32_t random = 0x5a17u;
    const int scale = 1 << (depth - 8);
    for (int p = 0; p < plane_count(image.format); ++p) {
        const int width = plane_width(image, p), height = plane_height(image, p);
        image.planes[p].strideBytes = width * bytes_per_sample(image.format) + std::stoi(option("CODEC_VIS_TEST_STRIDE_PAD", "0"));
        image.planes[p].bytes.resize(static_cast<std::size_t>(image.planes[p].strideBytes) * height);
        for (int y=0; y<height; ++y) for (int x=0; x<width; ++x) {
            random ^= random << 13; random ^= random >> 17; random ^= random << 5;
            int value = p == 0 ? 64 + (x*3 + y*5) % 128 : 120 + (x/7 + y/5 + p*3) % 16;
            if (signal == "noise") value = p == 0 ? 64 + int(random % 128) : 120 + int(random % 16);
            if (signal == "flat") value = 128;
            if (signal == "edges") value = p == 0 ? (((x/8 + y/8) & 1) ? 64 : 191) : (((x/8 + y/8 + p) & 1) ? 120 : 135);
            put_sample(image.planes[p], x, y, bytes_per_sample(image.format), value * scale + (signal == "flat" ? 0 : (x+y) % scale),
                       static_cast<uint32_t>(max_sample_value(image.format)));
        }
    }
    return image;
}
std::vector<std::string> enum_values(const CapabilityResult& caps, const std::string& key, std::string fallback) {
    for (const auto& p : caps.params) if (p.name == key) {
        std::vector<std::string> out;
        for (const auto& v : p.enumValues) if (v.value != "source") out.push_back(v.value);
        if (!out.empty()) return out;
    }
    return {std::move(fallback)};
}
}
int main(int argc, char** argv) {
    try {
        const auto backends = initial_encoder_backends();
        if (argc == 4 && std::string(argv[1]) == "--decode") {
            const auto* backend = find_backend(backends, BackendId{static_cast<uint64_t>(std::stoull(argv[2]))});
            if (!backend || !backend->decodePreview) throw std::runtime_error("decoder unavailable");
            std::ifstream file(argv[3], std::ios::binary);
            if (!file) throw std::runtime_error("cannot read bitstream");
            const std::vector<char> bytes((std::istreambuf_iterator<char>(file)), {});
            EncodedImage encoded;
            encoded.hevcAnnexB.resize(bytes.size());
            std::transform(bytes.begin(), bytes.end(), encoded.hevcAnnexB.begin(), [](char v) { return std::byte(static_cast<unsigned char>(v)); });
            const auto decoded = backend->decodePreview(encoded);
            if (!decoded.image) throw std::runtime_error(decoded.error);
            std::cout << "PASS\t" << decoded.image->width << 'x' << decoded.image->height << " depth=" << bit_depth(decoded.image->format) << '\n';
            return 0;
        }
        if (argc == 2 && std::string(argv[1]) == "--list") {
            for (const auto& b : backends) {
                const auto caps = b.queryCapabilities();
                auto join=[](const std::vector<std::string>& values) {
                    std::string out; for (const auto& s : values) { if (!out.empty()) out += ','; out += s; } return out;
                };
                const bool scc = std::ranges::any_of(caps.params, [](const auto& p) { return p.name == "scc"; });
                std::cout << b.id.value << '\t' << b.name << '\t' << caps.snapshot.available << '\t'
                          << join(enum_values(caps,"bit-depth","8")) << '\t'
                          << join(enum_values(caps,"chroma-subsampling","444")) << '\t' << scc << '\t'
                          << clean(caps.snapshot.error) << '\n';
            }
            return 0;
        }
        if (argc < 7) throw std::runtime_error("usage: test_codec_roundtrips BACKEND DEPTH CHROMA VARIANT WIDTH HEIGHT [BITSTREAM]");
        const int id = std::stoi(argv[1]), depth = std::stoi(argv[2]);
        const std::string chroma = argv[3], variant = argv[4];
        const auto* backend = find_backend(backends, BackendId{static_cast<uint64_t>(id)});
        if (!backend) throw std::runtime_error("backend not found");
        const auto caps = backend->queryCapabilities();
        if (!caps.snapshot.available) {
            std::cout << "UNAVAILABLE\t" << clean(caps.snapshot.error) << '\n'; return 77;
        }
        const RawImage source = pattern(depth, chroma, std::stoi(argv[5]), std::stoi(argv[6]));
        AppState state;
        EncoderConfig config; config.backend = backend->id;
        auto set=[&](std::string key, ParamValue value) {
            std::erase_if(config.params, [&](const EncoderParam& p) { return p.name == key; });
            config.params.push_back({.name=std::move(key), .value=std::move(value)});
        };
        set("bit-depth", std::to_string(depth)); set("chroma-subsampling", chroma);
        // Keep runs bounded without substituting a different coding tool set.
        if (id == 1 || id == 14) set("preset", std::string{"ultrafast"});
        if (id == 2) set("preset", std::string{"faster"});
        if (id == 3) set("preset", int64_t{12});
        if (id == 11) set("effort", int64_t{1});
        if (variant == "lossless") {
            if (id == 1 || id == 3 || id == 11 || id == 14) set("rate-control", std::string{"lossless"});
            else set("lossless", true);
        }
        if (variant == "alf-off") set("alf", false);
        if (variant == "alf") { set("alf", true); set("rdo", int64_t{0}); }
        if (variant == "scc") set("scc", true);
        if (variant == "progressive") set("progressive", true);
        if (variant == "arithmetic") set("arithmetic", true);
        if (variant == "near") set("near", int64_t{3});
        if (variant == "hp1" || variant == "hp2" || variant == "hp3") set("color-transform", variant);
        if (variant == "frequency") set("bitstream-order", variant);
        if (variant == "tiles") { set("tiles-x", int64_t{2}); set("tiles-y", int64_t{2}); }
        std::istringstream overrides(option("CODEC_VIS_TEST_PARAMS"));
        for (std::string assignment; std::getline(overrides, assignment, ';');) {
            const auto split = assignment.find('=');
            if (split == std::string::npos) throw std::runtime_error("invalid parameter override");
            const std::string key = assignment.substr(0,split), text = assignment.substr(split+1);
            const auto definition = std::ranges::find_if(caps.params, [&](const auto& p) { return p.name == key; });
            if (definition == caps.params.end()) throw std::runtime_error("unknown test parameter: " + key);
            if (definition->kind == ParamKind::Bool) {
                if (text != "true" && text != "false") throw std::runtime_error("invalid boolean override");
                set(key, text == "true");
            } else if (definition->kind == ParamKind::Int) set(key, int64_t{std::stoll(text)});
            else if (definition->kind == ParamKind::Float) set(key, std::stod(text));
            else set(key, text);
        }
        state.encoderConfigs.push_back(config);
        BackendInfo info; info.id=backend->id; info.params=caps.params;
        std::vector<EncoderParam> params;
        for (const auto& p : caps.params) if (parameter_is_enabled(state, info, p)) {
            auto value = resolved_param_value(state, backend->id, p);
            if (const auto* text = std::get_if<std::string>(&value); text && text->empty()) continue;
            params.push_back({.name=p.name, .value=std::move(value)});
        }
        for (const auto& p : params) std::cout << "PARAM\t" << p.name << '=' << param_value_text(p.value) << '\n';
        if (variant == "scc" && !std::ranges::any_of(params, [](const auto& p) { return p.name == "scc" && std::get<bool>(p.value); })) {
            std::cout << "UNAVAILABLE\tSCC is disabled by the advertised parameter dependencies for this format/rate control\n";
            return 77;
        }
        std::cout << "STAGE\tencode\n" << std::flush;
        const auto encoded = run_backend_encode(*backend, source, params);
        if (encoded.encoded.hevcAnnexB.empty()) throw std::runtime_error("empty bitstream");
        if (argc > 7) {
            std::ofstream file(argv[7],std::ios::binary);
            file.write(reinterpret_cast<const char*>(encoded.encoded.hevcAnnexB.data()), static_cast<std::streamsize>(encoded.encoded.hevcAnnexB.size()));
            if (!file) throw std::runtime_error("bitstream save failed");
        }
        // No encoder reconstruction/source fallback. This must parse output bytes.
        EncodedImage bytesOnly = encoded.encoded;
        bytesOnly.previewImage.reset();
        std::cout << "STAGE\tdecode\n" << std::flush;
        const auto decoded = backend->decodePreview(bytesOnly);
        if (!decoded.image) throw std::runtime_error("decode: " + decoded.error);
        if (decoded.image.get() == encoded.comparisonReference.get()) throw std::runtime_error("decoder aliased reference");
        const auto& out = *decoded.image;
        if (out.width != encoded.comparisonReference->width || out.height != encoded.comparisonReference->height) throw std::runtime_error("decoded dimensions differ");
        for (int p=0; p<plane_count(out.format); ++p) if (!plane_available(out,p)) throw std::runtime_error("decoded plane incomplete");
        const bool rgbCodec = id >= 8 && id <= 13;
        if (bit_depth(out.format) != depth) throw std::runtime_error("decoded precision " + std::to_string(bit_depth(out.format)) + " differs from requested " + std::to_string(depth));
        const auto& ref = *encoded.comparisonReference;
        if (argc > 7 && option("CODEC_VIS_TEST_EXPORT_RAW") == "1") {
            write_raw(ref, std::string(argv[7]) + ".reference.yuv");
            write_raw(out, std::string(argv[7]) + ".decoded.yuv");
        }
        if (option("CODEC_VIS_TEST_CHECK_COLOR") == "1" &&
            (out.color.primaries != ref.color.primaries || out.color.transfer != ref.color.transfer || out.color.matrix != ref.color.matrix || out.color.range != ref.color.range))
            throw std::runtime_error("decoded stream color description differs from input");
        if (!rgbCodec && out.format != ref.format) throw std::runtime_error("decoded chroma layout differs");
        if (variant == "lossless" && !rgbCodec) {
            for (int p=0;p<plane_count(out.format);++p) for (int y=0;y<plane_height(out,p);++y) for (int x=0;x<plane_width(out,p);++x)
                if (sample_at(out.planes[p],x,y,bytes_per_sample(out.format)) != sample_at(ref.planes[p],x,y,bytes_per_sample(ref.format)))
                    throw std::runtime_error("lossless decoded sample differs");
        }
        const bool rgbLossless = (id == 8 && variant != "near") || id == 13 || (rgbCodec && variant == "lossless");
        const bool boundedNear = id == 8 && variant == "near";
        if (rgbLossless || boundedNear) {
            if (out.format != ref.format && chroma != "400") throw std::runtime_error("lossless still preview changed sample layout");
            for (int p = 0; p < plane_count(ref.format); ++p)
                for (int y = 0; y < plane_height(out, p); ++y)
                    for (int x = 0; x < plane_width(out, p); ++x) {
                        const int delta = int(sample_at(out.planes[p],x,y,bytes_per_sample(out.format))) - int(sample_at(ref.planes[p],x,y,bytes_per_sample(ref.format)));
                        // The application converts YUV to RGB before coding and
                        // RGB back to YUV for preview. Allow only integer rounding.
                        const int tolerance = boundedNear ? 5 : 2;
                        if (std::abs(delta) > tolerance) throw std::runtime_error(boundedNear ? "JPEG-LS NEAR=3 exceeds sample-error bound after RGB/YUV rounding" : "lossless still sample differs beyond RGB/YUV rounding");
                    }
        }
        // Catch blank/constant planes and gross byte order/channel failures.
        const auto a=raw_image_to_rgba8(ref), b=raw_image_to_rgba8(out);
        uint64_t absolute=0; unsigned low=255,high=0,refLow=255,refHigh=0;
        for (std::size_t i=0;i<a.size();++i) if (i%4 != 3) {
            absolute += static_cast<uint64_t>(std::abs(int(a[i])-int(b[i])));
            refLow=std::min(refLow,unsigned(a[i])); refHigh=std::max(refHigh,unsigned(a[i]));
            low=std::min(low,unsigned(b[i])); high=std::max(high,unsigned(b[i]));
        }
        const double meanError = double(absolute) / (a.size()/4*3);
        std::cout << "PIXELS\tmean_abs_rgb8=" << meanError << " span=" << high-low << "\n";
        if ((refHigh-refLow >= 32 && high-low < 32) || meanError > 25) throw std::runtime_error("decoded pixels are blank or grossly corrupted");
        std::cout << "PASS\t" << encoded.encoded.hevcAnnexB.size() << '\t' << bit_depth(out.format) << '\t' << int(out.format) << '\n';
        return 0;
    } catch (const std::exception& e) { std::cout << "FAIL\t" << clean(e.what()) << '\n'; return 1; }
}
