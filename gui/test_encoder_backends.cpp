#include "encoder_backends.hpp"
#include "raw_image_utils.hpp"

#include <algorithm>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <span>
#include <stdexcept>
#include <string>
#include <tuple>
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

codec_gui::RawImage make_yuv420_high_depth_image(codec_gui::PixelFormat format, int shift) {
	codec_gui::RawImage image = make_yuv420_10_image();
	image.format = format;
	for (codec_gui::ImagePlane& plane : image.planes) {
		for (std::size_t i = 0; i + 1 < plane.bytes.size(); i += 2) {
			const uint16_t value = static_cast<uint16_t>(plane.bytes[i] | (plane.bytes[i + 1] << 8u));
			const uint16_t scaled = static_cast<uint16_t>(value << shift);
			plane.bytes[i] = static_cast<uint8_t>(scaled & 0xffu);
			plane.bytes[i + 1] = static_cast<uint8_t>(scaled >> 8u);
		}
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
		if (backend.name.starts_with("VA-API AV1")) {
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
	const EncoderBackend* av2 = find_backend(backends, BackendId{7});
	assert(av2 != nullptr);
	const codec_gui::RawImage av2Image = make_yuv420_image();
	const std::vector<codec_gui::EncoderParam> av2Params = {
		{"cpu-used", int64_t{9}},
		{"qp", int64_t{180}},
		{"lossless", false},
		{"enable-cdef", true},
		{"enable-restoration", true},
		{"enable-deblocking", true},
		{"enable-intrabc", false},
		{"enable-rect-partitions", false},
		{"enable-1to4-partitions", false},
		{"min-partition-size", int64_t{64}},
		{"max-partition-size", int64_t{128}},
		{"tile-columns", int64_t{0}},
		{"tile-rows", int64_t{0}},
	};
	const codec_gui::EncodedImage av2Encoded = av2->encode(av2Image, av2Params);
	assert(!av2Encoded.hevcAnnexB.empty());
	const DecodeResult av2Preview = av2->decodePreview(av2Encoded);
	if (!av2Preview.image) std::cerr << "AV2 dav2d decode failed: " << av2Preview.error << '\n';
	assert(av2Preview.image && av2Preview.error.empty());
	assert(av2Preview.image->width == av2Image.width);
	assert(av2Preview.image->height == av2Image.height);

	const EncoderBackend* jpegxl = find_backend(backends, BackendId{11});
	assert(jpegxl != nullptr);
	const CapabilityResult jpegxlCaps = jpegxl->queryCapabilities();
	assert(jpegxlCaps.snapshot.available);
	const auto rateControl = std::find_if(jpegxlCaps.params.begin(), jpegxlCaps.params.end(), [](const codec_gui::EncoderParamInfo& parameter) { return parameter.name == "rate-control"; });
	const auto quality = std::find_if(jpegxlCaps.params.begin(), jpegxlCaps.params.end(), [](const codec_gui::EncoderParamInfo& parameter) { return parameter.name == "quality"; });
	const auto distance = std::find_if(jpegxlCaps.params.begin(), jpegxlCaps.params.end(), [](const codec_gui::EncoderParamInfo& parameter) { return parameter.name == "distance"; });
	assert(rateControl != jpegxlCaps.params.end() && quality != jpegxlCaps.params.end() && distance != jpegxlCaps.params.end());
	assert(!quality->enabledWhen.empty() && quality->enabledWhen.front().acceptedValues == std::vector<std::string>{"quality"});
	assert(!distance->enabledWhen.empty() && distance->enabledWhen.front().acceptedValues == std::vector<std::string>{"distance"});
	assert(!has_param(jpegxlCaps.params, "lossless"));

	for (const auto [format, shift, expectedDepth] : {
		     std::tuple{codec_gui::PixelFormat::YUV420P8, 0, 8},
		     std::tuple{codec_gui::PixelFormat::YUV420P10LE, 0, 10},
		     std::tuple{codec_gui::PixelFormat::YUV420P12LE, 2, 12},
		     std::tuple{codec_gui::PixelFormat::YUV420P14LE, 4, 14},
	     }) {
		codec_gui::RawImage image = expectedDepth == 8 ? make_yuv420_image() :
			(expectedDepth == 10 ? make_yuv420_10_image() : make_yuv420_high_depth_image(format, shift));
		image.color.primaries = codec_gui::ColorPrimaries::BT709;
		image.color.transfer = codec_gui::TransferCharacteristics::SRGB;
		image.color.matrix = codec_gui::MatrixCoefficients::BT709;
		image.color.range = codec_gui::ColorRange::Limited;
		const std::vector<codec_gui::EncoderParam> params = {
			{"rate-control", std::string{"distance"}},
			{"distance", 1.0},
			{"effort", int64_t{1}},
			{"bit-depth", std::string{"source"}},
			{"chroma-subsampling", std::string{"source"}},
			{"color-primaries", std::string{"source"}},
			{"transfer", std::string{"source"}},
			{"matrix", std::string{"source"}},
			{"range", std::string{"source"}},
			{"tone-map", std::string{"none"}},
		};
		const EncodeResult encoded = run_backend_encode(*jpegxl, image, params);
		assert(!encoded.encoded.hevcAnnexB.empty());
		assert(encoded.encoded.codedColor.has_value());
		const DecodeResult preview = jpegxl->decodePreview(encoded.encoded);
		if (!preview.image) std::cerr << "JPEG XL " << expectedDepth << "-bit decode failed: " << preview.error << '\n';
		assert(preview.image && preview.error.empty());
		assert(preview.image->width == image.width && preview.image->height == image.height);
		assert(bit_depth(preview.image->format) == expectedDepth);
		assert(preview.image->color.primaries == image.color.primaries);
		assert(preview.image->color.transfer == image.color.transfer);
		assert(preview.image->color.matrix == image.color.matrix);
		assert(preview.image->color.range == image.color.range);
	}

	codec_gui::RawImage jpegxlLosslessImage = make_yuv420_10_image();
	jpegxlLosslessImage.color.primaries = codec_gui::ColorPrimaries::BT2020;
	jpegxlLosslessImage.color.transfer = codec_gui::TransferCharacteristics::PQ;
	jpegxlLosslessImage.color.matrix = codec_gui::MatrixCoefficients::BT2020NonConstant;
	jpegxlLosslessImage.color.range = codec_gui::ColorRange::Full;
	const std::vector<codec_gui::EncoderParam> jpegxlLosslessParams = {
		{"rate-control", std::string{"lossless"}},
		{"effort", int64_t{1}},
	};
	const codec_gui::EncodedImage jpegxlLossless = jpegxl->encode(jpegxlLosslessImage, jpegxlLosslessParams);
	assert(!jpegxlLossless.hevcAnnexB.empty());
	const DecodeResult jpegxlLosslessPreview = jpegxl->decodePreview(jpegxlLossless);
	assert(jpegxlLosslessPreview.image && jpegxlLosslessPreview.error.empty());
	assert(bit_depth(jpegxlLosslessPreview.image->format) == 10);

	for (const auto [format, depth] : {
		     std::pair{codec_gui::PixelFormat::Gray10LE, 10},
		     std::pair{codec_gui::PixelFormat::Gray12LE, 12},
		     std::pair{codec_gui::PixelFormat::Gray14LE, 14},
	     }) {
		codec_gui::RawImage gray;
		gray.width = 4;
		gray.height = 1;
		gray.format = format;
		gray.color.transfer = codec_gui::TransferCharacteristics::SRGB;
		gray.color.range = codec_gui::ColorRange::Full;
		gray.planes[0].strideBytes = gray.width * 2;
		gray.planes[0].bytes.resize(static_cast<std::size_t>(gray.planes[0].strideBytes));
		const uint16_t maximum = static_cast<uint16_t>((1u << depth) - 1u);
		const uint16_t samples[] = {0, 1, static_cast<uint16_t>(maximum / 3), maximum};
		for (std::size_t sample = 0; sample < 4; ++sample) {
			gray.planes[0].bytes[sample * 2] = static_cast<uint8_t>(samples[sample] & 0xffu);
			gray.planes[0].bytes[sample * 2 + 1] = static_cast<uint8_t>(samples[sample] >> 8u);
		}
		const codec_gui::EncodedImage encoded = jpegxl->encode(gray, jpegxlLosslessParams);
		const DecodeResult decoded = jpegxl->decodePreview(encoded);
		assert(decoded.image && decoded.error.empty());
		assert(decoded.image->format == format);
		for (std::size_t sample = 0; sample < 4; ++sample) {
			const uint16_t actual = static_cast<uint16_t>(decoded.image->planes[0].bytes[sample * 2] |
			                                              (static_cast<uint16_t>(decoded.image->planes[0].bytes[sample * 2 + 1]) << 8u));
			assert(actual == samples[sample]);
		}
	}

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
			     "rate-control", "qpi", "level-idc", "high-tier",
			     "bit-depth", "chroma-subsampling", "color-primaries", "transfer", "matrix", "range",
			     "sao", "slice-sao-luma", "slice-sao-chroma", "strong-intra-smoothing",
			     "constrained-intra", "transform-skip", "sign-data-hiding", "cu-qp-delta",
			     "pps-cb-qp-offset", "pps-cr-qp-offset", "slice-cb-qp-offset",
			     "slice-cr-qp-offset", "slice-beta-offset-div2", "slice-tc-offset-div2",
			     "entropy-coding-sync", "loop-filter-across-slices"
		     }) {
			assert(has_param(vaapiCaps.params, name));
		}
		assert(!has_param(vaapiCaps.params, "trellis"));
		assert(!has_param(vaapiCaps.params, "num-slices"));
		assert(!has_param(vaapiCaps.params, "device"));
		assert(!has_param(vaapiCaps.params, "implementation"));
		assert(!vaapiCaps.snapshot.details.empty());
		assert(vaapiCaps.snapshot.details.front().starts_with("Render node: "));

		const auto rc = std::find_if(vaapiCaps.params.begin(), vaapiCaps.params.end(), [](const codec_gui::EncoderParamInfo& param) {
			return param.name == "rate-control";
		});
		assert(rc != vaapiCaps.params.end() && !rc->enumValues.empty());
		for (const codec_gui::EnumValue& value : rc->enumValues) {
			assert(value.value == "cqp" || value.value == "icq" || value.value == "vbr" || value.value == "cbr");
		}

		const auto chroma = std::find_if(vaapiCaps.params.begin(), vaapiCaps.params.end(), [](const codec_gui::EncoderParamInfo& param) {
			return param.name == "chroma-subsampling";
		});
		assert(chroma != vaapiCaps.params.end());
		assert(!chroma->enumValues.empty());
		for (const codec_gui::EnumValue& value : chroma->enumValues) {
			assert(value.value == "420" || value.value == "422" || value.value == "444");
		}
		const auto bitDepth = std::find_if(vaapiCaps.params.begin(), vaapiCaps.params.end(), [](const codec_gui::EncoderParamInfo& param) {
			return param.name == "bit-depth";
		});
		assert(bitDepth != vaapiCaps.params.end());
		assert(std::any_of(bitDepth->enumValues.begin(), bitDepth->enumValues.end(), [](const codec_gui::EnumValue& value) { return value.value == "source"; }));
		assert(bitDepth->enumValues.size() >= 2);

		bool encodedOneAdvertisedFormat = false;
		for (const codec_gui::EnumValue& depth : bitDepth->enumValues) {
			if (depth.value == "source") continue;
			for (const codec_gui::EnumValue& subsampling : chroma->enumValues) {
				try {
					const std::vector<codec_gui::EncoderParam> hevcParams{
						{"rate-control", rc->enumValues.front().value},
						{"qpi", int64_t{35}},
						{"bit-depth", depth.value},
						{"chroma-subsampling", subsampling.value},
					};
					const codec_gui::EncodedImage encoded = vaapiHevc->encode(make_yuv420_image(640, 480), hevcParams);
					encodedOneAdvertisedFormat = !encoded.hevcAnnexB.empty();
				} catch (const std::exception&) {
				}
				if (encodedOneAdvertisedFormat) break;
			}
			if (encodedOneAdvertisedFormat) break;
		}
		assert(encodedOneAdvertisedFormat);
	} else {
		assert(!vaapiCaps.snapshot.error.empty());
	}

	const EncoderBackend* vaapiAv1 = find_backend(backends, BackendId{5});
	assert(vaapiAv1 != nullptr);
	CapabilityResult av1Caps = vaapiAv1->queryCapabilities();
	if (av1Caps.snapshot.available) {
		assert(has_param(av1Caps.params, "rate-control"));
		assert(has_param(av1Caps.params, "level-idx"));
		assert(!has_param(av1Caps.params, "high-tier"));
		const auto av1Level = std::find_if(av1Caps.params.begin(), av1Caps.params.end(), [](const codec_gui::EncoderParamInfo& param) {
			return param.name == "level-idx";
		});
		assert(av1Level != av1Caps.params.end());
		assert(std::get<int64_t>(av1Level->defaultValue) == -1);
		assert(av1Level->intRange);
		assert(av1Level->intRange->min == -1);
		assert(av1Level->intRange->max == 23);
		assert(av1Level->automaticIntValue == -1);
		assert(av1Level->automaticLabel == "Auto");
		const auto av1RateControl = std::find_if(av1Caps.params.begin(), av1Caps.params.end(), [](const codec_gui::EncoderParamInfo& param) {
			return param.name == "rate-control";
		});
		const auto av1BitDepth = std::find_if(av1Caps.params.begin(), av1Caps.params.end(), [](const codec_gui::EncoderParamInfo& param) {
			return param.name == "bit-depth";
		});
		assert(av1RateControl != av1Caps.params.end() && !av1RateControl->enumValues.empty());
		assert(av1BitDepth != av1Caps.params.end() && av1BitDepth->enumValues.size() >= 2);
		std::vector<codec_gui::EncoderParam> av1Params = {
			{"rate-control", av1RateControl->enumValues.front().value},
			{"qindex", int64_t{128}},
			{"bitrate-kbps", int64_t{10000}},
			{"level-idx", int64_t{-1}},
			{"bit-depth", av1BitDepth->enumValues[1].value},
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
	codec_gui::EncodedImage autoProfile12Bit = x265->encode(make_yuv420_high_depth_image(codec_gui::PixelFormat::YUV420P12LE, 2), autoProfileParams);
	assert(!autoProfile12Bit.hevcAnnexB.empty());
	try {
		const codec_gui::EncodedImage encoded14 = x265->encode(make_yuv420_high_depth_image(codec_gui::PixelFormat::YUV420P14LE, 4), autoProfileParams);
		assert(!encoded14.hevcAnnexB.empty());
	} catch (const std::runtime_error& error) {
		assert(std::string{error.what()}.find("14-bit") != std::string::npos);
	}

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
	codec_gui::RawImage vvencGuiImage = smallImage;
	vvencGuiImage.color.primaries = codec_gui::ColorPrimaries::BT709;
	vvencGuiImage.color.transfer = codec_gui::TransferCharacteristics::SRGB;
	vvencGuiImage.color.matrix = codec_gui::MatrixCoefficients::BT709;
	vvencGuiImage.color.range = codec_gui::ColorRange::Limited;
	const std::vector<codec_gui::EncoderParam> vvencGuiParams = {
		{"preset", std::string{"faster"}},
		{"rate-control", std::string{"qp"}},
		{"qp", int64_t{36}},
		{"qpa", false},
		{"threads", int64_t{1}},
		{"bit-depth", std::string{"source"}},
		{"chroma-subsampling", std::string{"source"}},
		{"color-primaries", std::string{"source"}},
		{"transfer", std::string{"source"}},
		{"matrix", std::string{"source"}},
		{"range", std::string{"source"}},
		{"tone-map", std::string{"none"}},
	};
	EncodeResult vvencGuiEncoded = run_backend_encode(*vvenc, vvencGuiImage, vvencGuiParams);
	assert(!vvencGuiEncoded.encoded.hevcAnnexB.empty());
	DecodeResult vvencGuiPreview = vvenc->decodePreview(vvencGuiEncoded.encoded);
	assert(vvencGuiPreview.image && vvencGuiPreview.error.empty());

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
