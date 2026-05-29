#include "encoder_backends.hpp"

#include <algorithm>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

using namespace codec_gui::gui;

namespace {

codec_gui::RawImage make_yuv420_image() {
	codec_gui::RawImage image;
	image.width = 64;
	image.height = 64;
	image.format = codec_gui::PixelFormat::YUV420P8;
	image.planes[0].strideBytes = image.width;
	image.planes[1].strideBytes = image.width / 2;
	image.planes[2].strideBytes = image.width / 2;
	image.planes[0].bytes.resize(static_cast<std::size_t>(image.width * image.height), uint8_t{96});
	image.planes[1].bytes.resize(static_cast<std::size_t>((image.width / 2) * (image.height / 2)), uint8_t{128});
	image.planes[2].bytes.resize(static_cast<std::size_t>((image.width / 2) * (image.height / 2)), uint8_t{128});
	for (int y = 0; y < image.height; ++y) {
		for (int x = 0; x < image.width; ++x) {
			image.planes[0].bytes[static_cast<std::size_t>(y * image.planes[0].strideBytes + x)] = static_cast<uint8_t>((x + y) & 0xff);
		}
	}
	return image;
}

codec_gui::RawImage make_yuv420_image(int width, int height) {
	codec_gui::RawImage image;
	image.width = width;
	image.height = height;
	image.format = codec_gui::PixelFormat::YUV420P8;
	image.planes[0].strideBytes = image.width;
	image.planes[1].strideBytes = image.width / 2;
	image.planes[2].strideBytes = image.width / 2;
	image.planes[0].bytes.resize(static_cast<std::size_t>(image.width * image.height), uint8_t{96});
	image.planes[1].bytes.resize(static_cast<std::size_t>((image.width / 2) * (image.height / 2)), uint8_t{128});
	image.planes[2].bytes.resize(static_cast<std::size_t>((image.width / 2) * (image.height / 2)), uint8_t{128});
	return image;
}

codec_gui::RawImage make_yuv420_10_image() {
	codec_gui::RawImage image;
	image.width = 64;
	image.height = 64;
	image.format = codec_gui::PixelFormat::YUV420P10LE;
	image.planes[0].strideBytes = image.width * 2;
	image.planes[1].strideBytes = (image.width / 2) * 2;
	image.planes[2].strideBytes = (image.width / 2) * 2;
	image.planes[0].bytes.resize(static_cast<std::size_t>(image.planes[0].strideBytes * image.height));
	image.planes[1].bytes.resize(static_cast<std::size_t>(image.planes[1].strideBytes * (image.height / 2)));
	image.planes[2].bytes.resize(static_cast<std::size_t>(image.planes[2].strideBytes * (image.height / 2)));
	auto put = [](std::vector<uint8_t>& bytes, std::size_t sample, uint16_t value) {
		bytes[sample * 2] = static_cast<uint8_t>(value & 0xffu);
		bytes[sample * 2 + 1] = static_cast<uint8_t>(value >> 8u);
	};
	for (int y = 0; y < image.height; ++y) {
		for (int x = 0; x < image.width; ++x) {
			put(image.planes[0].bytes, static_cast<std::size_t>(y * image.width + x), static_cast<uint16_t>((x * 4 + y * 4) & 1023));
		}
	}
	for (std::size_t i = 0; i < image.planes[1].bytes.size() / 2; ++i) {
		put(image.planes[1].bytes, i, 512);
		put(image.planes[2].bytes, i, 512);
	}
	return image;
}

codec_gui::RawImage make_yuv422_image(int width, int height) {
	codec_gui::RawImage image;
	image.width = width;
	image.height = height;
	image.format = codec_gui::PixelFormat::YUV422P8;
	image.planes[0].strideBytes = image.width;
	image.planes[1].strideBytes = image.width / 2;
	image.planes[2].strideBytes = image.width / 2;
	image.planes[0].bytes.resize(static_cast<std::size_t>(image.width * image.height));
	image.planes[1].bytes.resize(static_cast<std::size_t>((image.width / 2) * image.height));
	image.planes[2].bytes.resize(static_cast<std::size_t>((image.width / 2) * image.height));
	for (int y = 0; y < image.height; ++y) {
		for (int x = 0; x < image.width; ++x) {
			image.planes[0].bytes[static_cast<std::size_t>(y * image.planes[0].strideBytes + x)] =
				static_cast<uint8_t>((x * 3 + y * 5) & 0xff);
		}
		for (int x = 0; x < image.width / 2; ++x) {
			image.planes[1].bytes[static_cast<std::size_t>(y * image.planes[1].strideBytes + x)] =
				static_cast<uint8_t>(80 + ((x + y) & 31));
			image.planes[2].bytes[static_cast<std::size_t>(y * image.planes[2].strideBytes + x)] =
				static_cast<uint8_t>(176 - ((x * 2 + y) & 31));
		}
	}
	return image;
}

codec_gui::RawImage make_yuv444_image(int width, int height) {
	codec_gui::RawImage image;
	image.width = width;
	image.height = height;
	image.format = codec_gui::PixelFormat::YUV444P8;
	for (int plane = 0; plane < 3; ++plane) {
		image.planes[plane].strideBytes = image.width;
		image.planes[plane].bytes.resize(static_cast<std::size_t>(image.width * image.height));
	}
	for (int y = 0; y < image.height; ++y) {
		for (int x = 0; x < image.width; ++x) {
			const std::size_t offset = static_cast<std::size_t>(y * image.width + x);
			image.planes[0].bytes[offset] = static_cast<uint8_t>((x * 7 + y * 3) & 0xff);
			image.planes[1].bytes[offset] = static_cast<uint8_t>(96 + ((x + y * 2) & 63));
			image.planes[2].bytes[offset] = static_cast<uint8_t>(160 - ((x * 2 + y) & 63));
		}
	}
	return image;
}

bool has_param(std::span<const codec_gui::EncoderParamInfo> params, const std::string& name) {
	return std::any_of(params.begin(), params.end(), [&](const codec_gui::EncoderParamInfo& param) {
		return param.name == name;
	});
}

} // namespace

int main() {
	const std::vector<EncoderBackend> backends = initial_encoder_backends();
	assert(backends.size() >= 6);

	bool sawVaapiHevc = false;
	bool sawVaapiHevcScc = false;
	bool sawVaapiAv1 = false;
	bool sawAv2 = false;
	for (const EncoderBackend& backend : backends) {
		assert(backend.id.value != 0);
		assert(!backend.name.empty());
		assert(!backend.codec.empty());
		assert(backend.queryCapabilities != nullptr);
		assert(backend.encode != nullptr);
		if (backend.name.starts_with("VA-API HEVC")) {
			sawVaapiHevc = true;
		}
		if (backend.name == "VA-API HEVC SCC") {
			sawVaapiHevcScc = true;
		}
		if (backend.name == "VA-API AV1") {
			sawVaapiAv1 = true;
		}
		if (backend.codec == "AV2") {
			sawAv2 = true;
		}
	}
	assert(sawVaapiHevc);
	assert(!sawVaapiHevcScc);
	assert(sawVaapiAv1);
	assert(sawAv2);

	const EncoderBackend* found = find_backend(backends, backends.front().id);
	assert(found != nullptr);
	assert(found->name == backends.front().name);
	assert(find_backend(backends, BackendId{9999}) == nullptr);
	CapabilityResult caps = found->queryCapabilities();
	assert(caps.snapshot.available);
	assert(!caps.snapshot.details.empty());

	const EncoderBackend* vaapiHevc = find_backend(backends, BackendId{4});
	assert(vaapiHevc != nullptr);
	CapabilityResult vaapiCaps = vaapiHevc->queryCapabilities();
	if (vaapiCaps.snapshot.available) {
		for (const char* name : {
			     "rate-control", "qpi", "bitrate-kbps", "target-usage", "scc", "level-idc", "high-tier",
			     "bit-depth", "chroma-subsampling", "color-primaries", "transfer", "matrix", "range",
			     "sao", "slice-sao-luma", "slice-sao-chroma", "strong-intra-smoothing",
			     "constrained-intra", "transform-skip", "sign-data-hiding", "cu-qp-delta",
			     "pps-cb-qp-offset", "pps-cr-qp-offset", "slice-cb-qp-offset",
			     "slice-cr-qp-offset", "slice-beta-offset-div2", "slice-tc-offset-div2",
			     "num-tile-cols", "num-tile-rows", "entropy-coding-sync",
			     "loop-filter-across-tiles", "loop-filter-across-slices"
		     }) {
			assert(has_param(vaapiCaps.params, name));
		}
		assert(!has_param(vaapiCaps.params, "trellis"));
		assert(!has_param(vaapiCaps.params, "num-slices"));
		assert(!has_param(vaapiCaps.params, "device"));
		assert(!has_param(vaapiCaps.params, "implementation"));

		const auto chroma = std::find_if(vaapiCaps.params.begin(), vaapiCaps.params.end(), [](const codec_gui::EncoderParamInfo& param) {
			return param.name == "chroma-subsampling";
		});
		assert(chroma != vaapiCaps.params.end());
		assert(std::any_of(chroma->enumValues.begin(), chroma->enumValues.end(), [](const codec_gui::EnumValue& value) { return value.value == "420"; }));
		assert(std::any_of(chroma->enumValues.begin(), chroma->enumValues.end(), [](const codec_gui::EnumValue& value) { return value.value == "422"; }));
		assert(std::any_of(chroma->enumValues.begin(), chroma->enumValues.end(), [](const codec_gui::EnumValue& value) { return value.value == "444"; }));
		const auto bitDepth = std::find_if(vaapiCaps.params.begin(), vaapiCaps.params.end(), [](const codec_gui::EncoderParamInfo& param) {
			return param.name == "bit-depth";
		});
		assert(bitDepth != vaapiCaps.params.end());
		assert(std::any_of(bitDepth->enumValues.begin(), bitDepth->enumValues.end(), [](const codec_gui::EnumValue& value) { return value.value == "source"; }));
		assert(std::any_of(bitDepth->enumValues.begin(), bitDepth->enumValues.end(), [](const codec_gui::EnumValue& value) { return value.value == "8"; }));
		assert(std::any_of(bitDepth->enumValues.begin(), bitDepth->enumValues.end(), [](const codec_gui::EnumValue& value) { return value.value == "10"; }));
	} else {
		assert(!vaapiCaps.snapshot.error.empty());
	}

	const EncoderBackend* vaapiAv1 = find_backend(backends, BackendId{5});
	assert(vaapiAv1 != nullptr);
	CapabilityResult av1Caps = vaapiAv1->queryCapabilities();
	if (av1Caps.snapshot.available) {
		assert(has_param(av1Caps.params, "rate-control"));
		assert(has_param(av1Caps.params, "bitrate-kbps"));
		assert(has_param(av1Caps.params, "level-idx"));
		assert(!has_param(av1Caps.params, "high-tier"));
		const auto av1Level = std::find_if(av1Caps.params.begin(), av1Caps.params.end(), [](const codec_gui::EncoderParamInfo& param) {
			return param.name == "level-idx";
		});
		assert(av1Level != av1Caps.params.end());
		assert(std::get<int64_t>(av1Level->defaultValue) == -1);
		assert(av1Level->intRange);
		assert(av1Level->intRange->min == -1);
		assert(av1Level->intRange->max == 20);
		std::vector<codec_gui::EncoderParam> av1Params = {
			{"rate-control", std::string{"cqp"}},
			{"qindex", int64_t{128}},
			{"bitrate-kbps", int64_t{10000}},
			{"level-idx", int64_t{-1}},
			{"bit-depth", std::string{"source"}},
			{"cdef", true},
			{"loop-filter-level", int64_t{0}},
			{"tx-mode", std::string{"largest"}},
			{"tile-columns", int64_t{1}},
			{"tile-rows", int64_t{1}},
		};
		const codec_gui::RawImage av1TestImage = make_yuv420_image(640, 480);
		codec_gui::EncodedImage av1Encoded = vaapiAv1->encode(av1TestImage, av1Params);
		assert(!av1Encoded.hevcAnnexB.empty());
		DecodeResult av1Preview = vaapiAv1->decodePreview(av1Encoded);
		assert(av1Preview.image);
		assert(av1Preview.image->width == av1TestImage.width);
		assert(av1Preview.image->height == av1TestImage.height);
	} else {
		assert(!av1Caps.snapshot.error.empty());
	}

	const EncoderBackend* x265 = find_backend(backends, BackendId{1});
	assert(x265 != nullptr);
	const codec_gui::RawImage smallImage = make_yuv420_image();
	std::vector<codec_gui::EncoderParam> defaultParams = {
		{"preset", std::string{"medium"}},
		{"profile", std::string{"mainstillpicture"}},
		{"qp", int64_t{22}},
	};
	codec_gui::EncodedImage defaultEncoded = x265->encode(smallImage, defaultParams);
	assert(!defaultEncoded.hevcAnnexB.empty());
	std::vector<codec_gui::EncoderParam> tunedParams = defaultParams;
	tunedParams.push_back({"tune", std::string{"psnr"}});
	codec_gui::EncodedImage tunedEncoded = x265->encode(smallImage, tunedParams);
	assert(!tunedEncoded.hevcAnnexB.empty());

	std::vector<codec_gui::EncoderParam> autoProfileParams = {
		{"preset", std::string{"medium"}},
		{"profile", std::string{"auto"}},
		{"qp", int64_t{22}},
	};
	codec_gui::EncodedImage autoProfile10Bit = x265->encode(make_yuv420_10_image(), autoProfileParams);
	assert(!autoProfile10Bit.hevcAnnexB.empty());

	const EncoderBackend* vvenc = find_backend(backends, BackendId{2});
	assert(vvenc != nullptr);
	std::vector<codec_gui::EncoderParam> vvencParams = {
		{"preset", std::string{"medium"}},
		{"qp", int64_t{32}},
		{"bitrate", int64_t{0}},
		{"qpa", true},
		{"IntraQPOffset", int64_t{0}},
		{"MIP", true},
		{"ISP", true},
		{"MRL", true},
		{"LFNST", true},
		{"MTS", int64_t{1}},
		{"TS", false},
		{"BDPCM", false},
		{"LMChroma", true},
		{"CbQpOffset", int64_t{0}},
		{"CrQpOffset", int64_t{0}},
		{"CbQpOffsetDualTree", int64_t{0}},
		{"CrQpOffsetDualTree", int64_t{0}},
		{"SliceChromaQPOffsetIntraOrPeriodic.0", int64_t{0}},
		{"SliceChromaQPOffsetIntraOrPeriodic.1", int64_t{0}},
		{"SAO", true},
		{"ALF", true},
		{"CCALF", true},
		{"LoopFilterDisable", false},
		{"LoopFilterBetaOffset_div2", int64_t{0}},
		{"LoopFilterTcOffset_div2", int64_t{0}},
		{"hdr", std::string{"off"}},
		{"threads", int64_t{-1}},
	};
	codec_gui::EncodedImage vvencEncoded = vvenc->encode(smallImage, vvencParams);
	assert(!vvencEncoded.hevcAnnexB.empty());
	DecodeResult vvencPreview = vvenc->decodePreview(vvencEncoded);
	assert(vvencPreview.image);
	assert(vvencPreview.image->width == smallImage.width);
	assert(vvencPreview.image->height == smallImage.height);

	std::vector<codec_gui::EncoderParam> vvencFastParams = {
		{"preset", std::string{"faster"}},
		{"qp", int64_t{36}},
		{"bitrate", int64_t{0}},
		{"qpa", false},
		{"threads", int64_t{1}},
	};
	const codec_gui::RawImage vvenc444Image = make_yuv444_image(48, 32);
	try {
		(void)vvenc->encode(vvenc444Image, vvencFastParams);
		assert(false && "VVenC should reject unsupported 4:4:4 input");
	} catch (const std::runtime_error&) {
	}

	const EncoderBackend* uvg266 = find_backend(backends, BackendId{6});
	assert(uvg266 != nullptr);
	std::vector<codec_gui::EncoderParam> uvg266Params = {
		{"preset", std::string{"medium"}},
		{"qp", int64_t{48}},
		{"rdo", int64_t{2}},
		{"rdoq", true},
		{"sao", true},
		{"alf", true},
		{"lossless", false},
		{"mip", true},
		{"lfnst", true},
		{"isp", true},
	};
	const codec_gui::RawImage nonSquareImage = make_yuv420_image(96, 64);
	codec_gui::EncodedImage uvg266Encoded = uvg266->encode(nonSquareImage, uvg266Params);
	assert(!uvg266Encoded.hevcAnnexB.empty());
	DecodeResult uvg266Preview = uvg266->decodePreview(uvg266Encoded);
	assert(uvg266Preview.image);
	assert(uvg266Preview.image->width == nonSquareImage.width);
	assert(uvg266Preview.image->height == nonSquareImage.height);

	std::vector<codec_gui::EncoderParam> uvg266FastParams = {
		{"preset", std::string{"faster"}},
		{"qp", int64_t{48}},
		{"rdo", int64_t{1}},
		{"rdoq", false},
		{"sao", true},
		{"alf", true},
		{"lossless", false},
		{"mip", true},
		{"lfnst", true},
		{"isp", true},
	};
	const codec_gui::RawImage uvg422Image = make_yuv422_image(64, 48);
	try {
		(void)uvg266->encode(uvg422Image, uvg266FastParams);
		assert(false && "uvg266 should reject unsupported 4:2:2 input");
	} catch (const std::runtime_error&) {
	}

	if (std::filesystem::exists("file.h265")) {
		std::ifstream in("file.h265", std::ios::binary);
		std::vector<char> chars((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
		codec_gui::EncodedImage encoded;
		encoded.hevcAnnexB.reserve(chars.size());
		for (char c : chars) {
			encoded.hevcAnnexB.push_back(static_cast<std::byte>(static_cast<unsigned char>(c)));
		}
		DecodeResult decoded = x265->decodePreview(encoded);
		assert(decoded.image);
		assert(decoded.image->width > 0);
		assert(decoded.image->height > 0);
	}

	return 0;
}
