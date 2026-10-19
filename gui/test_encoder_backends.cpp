#include "test_check.hpp"
#include "../codec_gui_x265.hpp"
#include "encoder_backends.hpp"
#include "raw_image_utils.hpp"

#include <algorithm>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <span>
#include <stdexcept>
#include <string>
#include <tuple>
#include <utility>
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
			const auto value = static_cast<uint16_t>(plane.bytes[i] | (plane.bytes[i + 1] << 8u));
			const auto scaled = static_cast<uint16_t>(value << shift);
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
	for (auto & plane : image.planes) {
		plane.strideBytes = image.width;
		plane.bytes.resize(static_cast<std::size_t>(image.width * image.height));
	}
	for (int y = 0; y < image.height; ++y) {
		for (int x = 0; x < image.width; ++x) {
			const auto offset = static_cast<std::size_t>(y * image.width + x);
			image.planes[0].bytes[offset] = static_cast<uint8_t>((x * 7 + y * 3) & 0xff);
			image.planes[1].bytes[offset] = static_cast<uint8_t>(96 + ((x + y * 2) & 63));
			image.planes[2].bytes[offset] = static_cast<uint8_t>(160 - ((x * 2 + y) & 63));
		}
	}
	return image;
}

bool has_param(std::span<const codec_gui::EncoderParamInfo> params, const std::string& name) {
	return std::ranges::any_of(params, [&](const codec_gui::EncoderParamInfo& param) {
		return param.name == name;
	});
}

} // namespace

int main() {
	const std::vector<EncoderBackend> backends = initial_encoder_backends();
	CODEC_VIS_CHECK(backends.size() >= 6);

	bool sawVaapiHevc = false;
	bool sawVaapiHevcScc = false;
	bool sawVaapiAv1 = false;
	bool sawAv2 = false;
	for (const EncoderBackend& backend : backends) {
		CODEC_VIS_CHECK(backend.id.value != 0);
		CODEC_VIS_CHECK(!backend.name.empty());
		CODEC_VIS_CHECK(!backend.codec.empty());
		CODEC_VIS_CHECK(backend.queryCapabilities != nullptr);
		CODEC_VIS_CHECK(backend.encode != nullptr);
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
	CODEC_VIS_CHECK(sawVaapiHevc);
	CODEC_VIS_CHECK(!sawVaapiHevcScc);
	CODEC_VIS_CHECK(sawVaapiAv1);
	CODEC_VIS_CHECK(sawAv2);
	const EncoderBackend* av2 = find_backend(backends, BackendId{7});
	CODEC_VIS_CHECK(av2 != nullptr);
	const codec_gui::RawImage av2Image = make_yuv420_image();
	const std::vector<codec_gui::EncoderParam> av2Params = {
		{.name="cpu-used", .value=int64_t{9}},
		{.name="qp", .value=int64_t{180}},
		{.name="lossless", .value=false},
		{.name="enable-cdef", .value=true},
		{.name="enable-restoration", .value=true},
		{.name="enable-deblocking", .value=true},
		{.name="enable-intrabc", .value=false},
		{.name="enable-rect-partitions", .value=false},
		{.name="enable-1to4-partitions", .value=false},
		{.name="min-partition-size", .value=int64_t{64}},
		{.name="max-partition-size", .value=int64_t{128}},
		{.name="tile-columns", .value=int64_t{0}},
		{.name="tile-rows", .value=int64_t{0}},
	};
	if (av2->queryCapabilities().snapshot.available) {
		const codec_gui::EncodedImage av2Encoded = av2->encode(av2Image, av2Params);
		CODEC_VIS_CHECK(!av2Encoded.hevcAnnexB.empty());
		const DecodeResult av2Preview = av2->decodePreview(av2Encoded);
		if (!av2Preview.image) std::cerr << "AV2 dav2d decode failed: " << av2Preview.error << '\n';
		CODEC_VIS_CHECK(av2Preview.image && av2Preview.error.empty());
		CODEC_VIS_CHECK(av2Preview.image->width == av2Image.width);
		CODEC_VIS_CHECK(av2Preview.image->height == av2Image.height);
	} else {
		CODEC_VIS_CHECK(!av2->queryCapabilities().snapshot.error.empty());
		bool rejected = false;
		try { (void)av2->encode(av2Image, av2Params); }
		catch (const std::runtime_error& error) {
			rejected = std::string(error.what()).find("TFLite") != std::string::npos;
		}
		CODEC_VIS_CHECK(rejected);
		std::cerr << "AV2 unavailable: " << av2->queryCapabilities().snapshot.error << '\n';
	}

	const EncoderBackend* jpegxl = find_backend(backends, BackendId{11});
	CODEC_VIS_CHECK(jpegxl != nullptr);
	const CapabilityResult jpegxlCaps = jpegxl->queryCapabilities();
	CODEC_VIS_CHECK(jpegxlCaps.snapshot.available);
	const auto rateControl = std::ranges::find_if(jpegxlCaps.params, [](const codec_gui::EncoderParamInfo& parameter) { return parameter.name == "rate-control"; });
	const auto quality = std::ranges::find_if(jpegxlCaps.params, [](const codec_gui::EncoderParamInfo& parameter) { return parameter.name == "quality"; });
	const auto distance = std::ranges::find_if(jpegxlCaps.params, [](const codec_gui::EncoderParamInfo& parameter) { return parameter.name == "distance"; });
	CODEC_VIS_CHECK(rateControl != jpegxlCaps.params.end() && quality != jpegxlCaps.params.end() && distance != jpegxlCaps.params.end());
	CODEC_VIS_CHECK(!quality->enabledWhen.empty() && quality->enabledWhen.front().acceptedValues == std::vector<std::string>{"quality"});
	CODEC_VIS_CHECK(!distance->enabledWhen.empty() && distance->enabledWhen.front().acceptedValues == std::vector<std::string>{"distance"});
	CODEC_VIS_CHECK(!has_param(jpegxlCaps.params, "lossless"));

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
			{.name="rate-control", .value=std::string{"distance"}},
			{.name="distance", .value=1.0},
			{.name="effort", .value=int64_t{1}},
			{.name="bit-depth", .value=std::string{"source"}},
			{.name="chroma-subsampling", .value=std::string{"source"}},
			{.name="color-primaries", .value=std::string{"source"}},
			{.name="transfer", .value=std::string{"source"}},
			{.name="matrix", .value=std::string{"source"}},
			{.name="range", .value=std::string{"source"}},
			{.name="tone-map", .value=std::string{"none"}},
		};
		const EncodeResult encoded = run_backend_encode(*jpegxl, image, params);
		CODEC_VIS_CHECK(!encoded.encoded.hevcAnnexB.empty());
		CODEC_VIS_CHECK(encoded.encoded.codedColor.has_value());
		const DecodeResult preview = jpegxl->decodePreview(encoded.encoded);
		if (!preview.image) std::cerr << "JPEG XL " << expectedDepth << "-bit decode failed: " << preview.error << '\n';
		CODEC_VIS_CHECK(preview.image && preview.error.empty());
		CODEC_VIS_CHECK(preview.image->width == image.width && preview.image->height == image.height);
		CODEC_VIS_CHECK(bit_depth(preview.image->format) == expectedDepth);
		CODEC_VIS_CHECK(preview.image->color.primaries == image.color.primaries);
		CODEC_VIS_CHECK(preview.image->color.transfer == image.color.transfer);
		CODEC_VIS_CHECK(preview.image->color.matrix == image.color.matrix);
		CODEC_VIS_CHECK(preview.image->color.range == image.color.range);
	}

	codec_gui::RawImage jpegxlLosslessImage = make_yuv420_10_image();
	jpegxlLosslessImage.color.primaries = codec_gui::ColorPrimaries::BT2020;
	jpegxlLosslessImage.color.transfer = codec_gui::TransferCharacteristics::PQ;
	jpegxlLosslessImage.color.matrix = codec_gui::MatrixCoefficients::BT2020NonConstant;
	jpegxlLosslessImage.color.range = codec_gui::ColorRange::Full;
	const std::vector<codec_gui::EncoderParam> jpegxlLosslessParams = {
		{.name="rate-control", .value=std::string{"lossless"}},
		{.name="effort", .value=int64_t{1}},
	};
	const codec_gui::EncodedImage jpegxlLossless = jpegxl->encode(jpegxlLosslessImage, jpegxlLosslessParams);
	CODEC_VIS_CHECK(!jpegxlLossless.hevcAnnexB.empty());
	const DecodeResult jpegxlLosslessPreview = jpegxl->decodePreview(jpegxlLossless);
	CODEC_VIS_CHECK(jpegxlLosslessPreview.image && jpegxlLosslessPreview.error.empty());
	CODEC_VIS_CHECK(bit_depth(jpegxlLosslessPreview.image->format) == 10);

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
		const auto maximum = static_cast<uint16_t>((1u << depth) - 1u);
		const uint16_t samples[] = {0, 1, static_cast<uint16_t>(maximum / 3), maximum};
		for (std::size_t sample = 0; sample < 4; ++sample) {
			gray.planes[0].bytes[sample * 2] = static_cast<uint8_t>(samples[sample] & 0xffu);
			gray.planes[0].bytes[sample * 2 + 1] = static_cast<uint8_t>(samples[sample] >> 8u);
		}
		const codec_gui::EncodedImage encoded = jpegxl->encode(gray, jpegxlLosslessParams);
		const DecodeResult decoded = jpegxl->decodePreview(encoded);
		CODEC_VIS_CHECK(decoded.image && decoded.error.empty());
		CODEC_VIS_CHECK(decoded.image->format == format);
		for (std::size_t sample = 0; sample < 4; ++sample) {
			const auto actual = static_cast<uint16_t>(decoded.image->planes[0].bytes[sample * 2] |
			                                              (static_cast<uint16_t>(decoded.image->planes[0].bytes[sample * 2 + 1]) << 8u));
			CODEC_VIS_CHECK(actual == samples[sample]);
		}
	}

	const EncoderBackend* found = find_backend(backends, backends.front().id);
	CODEC_VIS_CHECK(found != nullptr);
	CODEC_VIS_CHECK(found->name == backends.front().name);
	CODEC_VIS_CHECK(find_backend(backends, BackendId{9999}) == nullptr);
	CapabilityResult const caps = found->queryCapabilities();
	CODEC_VIS_CHECK(caps.snapshot.available);
	CODEC_VIS_CHECK(!caps.snapshot.details.empty());

	const EncoderBackend* vaapiHevc = find_backend(backends, BackendId{4});
	CODEC_VIS_CHECK(vaapiHevc != nullptr);
	CapabilityResult vaapiCaps = vaapiHevc->queryCapabilities();
	if (vaapiCaps.snapshot.available) {
		for (const char* name : {
			     "rate-control", "qpi", "level-idc", "high-tier",
			     "bit-depth", "chroma-subsampling", "color-primaries", "transfer", "matrix", "range",
			     "slice-sao-luma", "slice-sao-chroma",
			     "pps-cb-qp-offset", "pps-cr-qp-offset", "slice-cb-qp-offset",
			     "slice-cr-qp-offset", "slice-beta-offset-div2", "slice-tc-offset-div2",
			     "entropy-coding-sync", "loop-filter-across-slices"
		     }) {
			CODEC_VIS_CHECK(has_param(vaapiCaps.params, name));
		}
		// Drivers may require or forbid these flags, so only editable flags
		// appear as controls. Their presence is not portable across profiles.
		for (const char* name : {"sao", "strong-intra-smoothing", "constrained-intra",
		                         "transform-skip", "sign-data-hiding", "cu-qp-delta", "deblock"}) {
			const auto param = std::ranges::find_if(vaapiCaps.params, [&](const auto& p) { return p.name == name; });
			if (param != vaapiCaps.params.end()) {
				CODEC_VIS_CHECK(param->kind == codec_gui::ParamKind::Bool);
				CODEC_VIS_CHECK(std::holds_alternative<bool>(param->defaultValue));
			}
		}
		CODEC_VIS_CHECK(!has_param(vaapiCaps.params, "trellis"));
		CODEC_VIS_CHECK(!has_param(vaapiCaps.params, "num-slices"));
		CODEC_VIS_CHECK(!has_param(vaapiCaps.params, "device"));
		CODEC_VIS_CHECK(!has_param(vaapiCaps.params, "implementation"));
		CODEC_VIS_CHECK(!vaapiCaps.snapshot.details.empty());
		CODEC_VIS_CHECK(vaapiCaps.snapshot.details.front().starts_with("Render node: "));

		const auto rc = std::ranges::find_if(vaapiCaps.params, [](const codec_gui::EncoderParamInfo& param) {
			return param.name == "rate-control";
		});
		CODEC_VIS_CHECK(rc != vaapiCaps.params.end() && !rc->enumValues.empty());
		for (const codec_gui::EnumValue& value : rc->enumValues) {
			CODEC_VIS_CHECK(value.value == "cqp" || value.value == "icq" || value.value == "vbr" || value.value == "cbr");
		}

		const auto chroma = std::ranges::find_if(vaapiCaps.params, [](const codec_gui::EncoderParamInfo& param) {
			return param.name == "chroma-subsampling";
		});
		CODEC_VIS_CHECK(chroma != vaapiCaps.params.end());
		CODEC_VIS_CHECK(!chroma->enumValues.empty());
		for (const codec_gui::EnumValue& value : chroma->enumValues) {
			CODEC_VIS_CHECK(value.value == "420" || value.value == "422" || value.value == "444");
		}
		const auto bitDepth = std::ranges::find_if(vaapiCaps.params, [](const codec_gui::EncoderParamInfo& param) {
			return param.name == "bit-depth";
		});
		CODEC_VIS_CHECK(bitDepth != vaapiCaps.params.end());
		CODEC_VIS_CHECK(!bitDepth->enumValues.empty());
		for (const codec_gui::EnumValue& value : bitDepth->enumValues) {
			CODEC_VIS_CHECK(value.value == "8" || value.value == "10");
		}

		bool encodedOneAdvertisedFormat = false;
		for (const codec_gui::EnumValue& depth : bitDepth->enumValues) {
			if (depth.value == "source") continue;
			for (const codec_gui::EnumValue& subsampling : chroma->enumValues) {
				try {
					const std::vector<codec_gui::EncoderParam> hevcParams{
						{.name="rate-control", .value=rc->enumValues.front().value},
						{.name="qpi", .value=int64_t{35}},
						{.name="bit-depth", .value=depth.value},
						{.name="chroma-subsampling", .value=subsampling.value},
					};
					const codec_gui::EncodedImage encoded = vaapiHevc->encode(make_yuv420_image(640, 480), hevcParams);
					encodedOneAdvertisedFormat = !encoded.hevcAnnexB.empty();
				} catch (const std::exception&) {
				}
				if (encodedOneAdvertisedFormat) break;
			}
			if (encodedOneAdvertisedFormat) break;
		}
		CODEC_VIS_CHECK(encodedOneAdvertisedFormat);
	} else {
		CODEC_VIS_CHECK(!vaapiCaps.snapshot.error.empty());
	}

	const EncoderBackend* vaapiAv1 = find_backend(backends, BackendId{5});
	CODEC_VIS_CHECK(vaapiAv1 != nullptr);
	CapabilityResult av1Caps = vaapiAv1->queryCapabilities();
	if (av1Caps.snapshot.available) {
		CODEC_VIS_CHECK(has_param(av1Caps.params, "rate-control"));
		CODEC_VIS_CHECK(has_param(av1Caps.params, "level-idx"));
		CODEC_VIS_CHECK(!has_param(av1Caps.params, "high-tier"));
		const auto av1Level = std::ranges::find_if(av1Caps.params, [](const codec_gui::EncoderParamInfo& param) {
			return param.name == "level-idx";
		});
		CODEC_VIS_CHECK(av1Level != av1Caps.params.end());
		CODEC_VIS_CHECK(std::get<int64_t>(av1Level->defaultValue) == -1);
		CODEC_VIS_CHECK(av1Level->intRange);
		CODEC_VIS_CHECK(av1Level->intRange->min == -1);
		CODEC_VIS_CHECK(av1Level->intRange->max == 23);
		CODEC_VIS_CHECK(av1Level->automaticIntValue == -1);
		CODEC_VIS_CHECK(av1Level->automaticLabel == "Auto");
		const auto av1RateControl = std::ranges::find_if(av1Caps.params, [](const codec_gui::EncoderParamInfo& param) {
			return param.name == "rate-control";
		});
		const auto av1BitDepth = std::ranges::find_if(av1Caps.params, [](const codec_gui::EncoderParamInfo& param) {
			return param.name == "bit-depth";
		});
		CODEC_VIS_CHECK(av1RateControl != av1Caps.params.end() && !av1RateControl->enumValues.empty());
		CODEC_VIS_CHECK(av1BitDepth != av1Caps.params.end() && av1BitDepth->enumValues.size() >= 2);
		const codec_gui::RawImage av1TestImage = make_yuv420_image(640, 480);
		for (const codec_gui::EnumValue& rateControl : av1RateControl->enumValues) {
			const std::vector<codec_gui::EncoderParam> av1Params = {
				{.name="rate-control", .value=rateControl.value},
				{.name="qindex", .value=int64_t{128}},
				{.name="bitrate-kbps", .value=int64_t{10000}},
				{.name="level-idx", .value=int64_t{-1}},
				{.name="bit-depth", .value=av1BitDepth->enumValues[1].value},
				{.name="cdef", .value=true},
				{.name="loop-filter-level", .value=int64_t{0}},
				{.name="tx-mode", .value=std::string{"largest"}},
				{.name="tile-columns", .value=int64_t{1}},
				{.name="tile-rows", .value=int64_t{1}},
			};
			codec_gui::EncodedImage const av1Encoded = vaapiAv1->encode(av1TestImage, av1Params);
			CODEC_VIS_CHECK(!av1Encoded.hevcAnnexB.empty());
			DecodeResult const av1Preview = vaapiAv1->decodePreview(av1Encoded);
			CODEC_VIS_CHECK(av1Preview.image);
			CODEC_VIS_CHECK(av1Preview.image->width == av1TestImage.width);
			CODEC_VIS_CHECK(av1Preview.image->height == av1TestImage.height);
		}
	} else {
		CODEC_VIS_CHECK(!av1Caps.snapshot.error.empty());
	}

	const EncoderBackend* x265 = find_backend(backends, BackendId{1});
	CODEC_VIS_CHECK(x265 != nullptr);
	const codec_gui::RawImage smallImage = make_yuv420_image();
	std::vector<codec_gui::EncoderParam> defaultParams = {
		{.name="preset", .value=std::string{"medium"}},
		{.name="profile", .value=std::string{"mainstillpicture"}},
		{.name="qp", .value=int64_t{22}},
	};
	codec_gui::EncodedImage const defaultEncoded = x265->encode(smallImage, defaultParams);
	CODEC_VIS_CHECK(!defaultEncoded.hevcAnnexB.empty());
	std::vector<codec_gui::EncoderParam> tunedParams = defaultParams;
	tunedParams.push_back({.name="tune", .value=std::string{"psnr"}});
	codec_gui::EncodedImage const tunedEncoded = x265->encode(smallImage, tunedParams);
	CODEC_VIS_CHECK(!tunedEncoded.hevcAnnexB.empty());

	std::vector<codec_gui::EncoderParam> autoProfileParams = {
		{.name="preset", .value=std::string{"medium"}},
		{.name="profile", .value=std::string{"auto"}},
		{.name="qp", .value=int64_t{22}},
	};
	codec_gui::EncodedImage const autoProfile10Bit = x265->encode(make_yuv420_10_image(), autoProfileParams);
	CODEC_VIS_CHECK(!autoProfile10Bit.hevcAnnexB.empty());
	codec_gui::EncodedImage const autoProfile12Bit = x265->encode(make_yuv420_high_depth_image(codec_gui::PixelFormat::YUV420P12LE, 2), autoProfileParams);
	CODEC_VIS_CHECK(!autoProfile12Bit.hevcAnnexB.empty());
	try {
		const codec_gui::EncodedImage encoded14 = x265->encode(make_yuv420_high_depth_image(codec_gui::PixelFormat::YUV420P14LE, 4), autoProfileParams);
		CODEC_VIS_CHECK(!encoded14.hevcAnnexB.empty());
	} catch (const std::runtime_error& error) {
		CODEC_VIS_CHECK(std::string{error.what()}.find("14-bit") != std::string::npos);
	}

	const EncoderBackend* vvenc = find_backend(backends, BackendId{2});
	CODEC_VIS_CHECK(vvenc != nullptr);
	const CapabilityResult vvencCaps = vvenc->queryCapabilities();
	const auto vvencBdpcm = std::ranges::find_if(vvencCaps.params, [](const codec_gui::EncoderParamInfo& param) {
		return param.name == "BDPCM";
	});
	CODEC_VIS_CHECK(vvencBdpcm != vvencCaps.params.end());
	CODEC_VIS_CHECK(vvencBdpcm->enabledWhen.size() == 1);
	CODEC_VIS_CHECK(vvencBdpcm->enabledWhen.front().parameter == "TS");
	CODEC_VIS_CHECK(vvencBdpcm->enabledWhen.front().acceptedValues == std::vector<std::string>{"true"});
	for (const std::string& name : std::vector<std::string>{
		"rate-control",
		"bitrate",
		"passes",
		"IntraQPOffset",
		"SliceChromaQPOffsetIntraOrPeriodic.0",
		"SliceChromaQPOffsetIntraOrPeriodic.1",
	}) {
		const auto parameter = std::ranges::find_if(vvencCaps.params, [&](const codec_gui::EncoderParamInfo& candidate) {
			return candidate.name == name;
		});
		CODEC_VIS_CHECK(parameter != vvencCaps.params.end());
		CODEC_VIS_CHECK(!parameter->relevantForStillImage);
	}
	const auto vvencMts = std::ranges::find_if(vvencCaps.params, [](const codec_gui::EncoderParamInfo& param) {
		return param.name == "MTS";
	});
	CODEC_VIS_CHECK(vvencMts != vvencCaps.params.end());
	CODEC_VIS_CHECK(vvencMts->intRange.has_value());
	CODEC_VIS_CHECK(vvencMts->intRange->min == 0);
	CODEC_VIS_CHECK(vvencMts->intRange->max == 1);
	CODEC_VIS_CHECK(vvencMts->help.find("0 disables MTS; 1 enables") != std::string::npos);
	std::vector<codec_gui::EncoderParam> vvencParams = {
		{.name="preset", .value=std::string{"medium"}},
		{.name="qp", .value=int64_t{32}},
		{.name="bitrate", .value=int64_t{0}},
		{.name="qpa", .value=true},
		{.name="IntraQPOffset", .value=int64_t{0}},
		{.name="MIP", .value=true},
		{.name="ISP", .value=true},
		{.name="MRL", .value=true},
		{.name="LFNST", .value=true},
		{.name="MTS", .value=int64_t{1}},
		{.name="TS", .value=false},
		{.name="BDPCM", .value=false},
		{.name="LMChroma", .value=true},
		{.name="CbQpOffset", .value=int64_t{0}},
		{.name="CrQpOffset", .value=int64_t{0}},
		{.name="CbQpOffsetDualTree", .value=int64_t{0}},
		{.name="CrQpOffsetDualTree", .value=int64_t{0}},
		{.name="SliceChromaQPOffsetIntraOrPeriodic.0", .value=int64_t{0}},
		{.name="SliceChromaQPOffsetIntraOrPeriodic.1", .value=int64_t{0}},
		{.name="SAO", .value=true},
		{.name="ALF", .value=true},
		{.name="CCALF", .value=true},
		{.name="LoopFilterDisable", .value=false},
		{.name="LoopFilterBetaOffset_div2", .value=int64_t{0}},
		{.name="LoopFilterTcOffset_div2", .value=int64_t{0}},
		{.name="hdr", .value=std::string{"off"}},
		{.name="threads", .value=int64_t{-1}},
	};
	codec_gui::EncodedImage const vvencEncoded = vvenc->encode(smallImage, vvencParams);
	CODEC_VIS_CHECK(!vvencEncoded.hevcAnnexB.empty());
	const std::vector<codec_gui::EncoderParam> vvencSlowerTransformSkipOff = {
		{.name="preset", .value=std::string{"slower"}},
		{.name="qp", .value=int64_t{32}},
		{.name="TS", .value=false},
		// Simulate a queued configuration created before BDPCM became disabled.
		{.name="BDPCM", .value=true},
	};
	codec_gui::EncodedImage const vvencSlowerEncoded = vvenc->encode(smallImage, vvencSlowerTransformSkipOff);
	CODEC_VIS_CHECK(!vvencSlowerEncoded.hevcAnnexB.empty());
	DecodeResult const vvencPreview = vvenc->decodePreview(vvencEncoded);
	CODEC_VIS_CHECK(vvencPreview.image);
	CODEC_VIS_CHECK(vvencPreview.image->width == smallImage.width);
	CODEC_VIS_CHECK(vvencPreview.image->height == smallImage.height);
	codec_gui::RawImage vvencGuiImage = smallImage;
	vvencGuiImage.color.primaries = codec_gui::ColorPrimaries::BT709;
	vvencGuiImage.color.transfer = codec_gui::TransferCharacteristics::SRGB;
	vvencGuiImage.color.matrix = codec_gui::MatrixCoefficients::BT709;
	vvencGuiImage.color.range = codec_gui::ColorRange::Limited;
	const std::vector<codec_gui::EncoderParam> vvencGuiParams = {
		{.name="preset", .value=std::string{"faster"}},
		{.name="rate-control", .value=std::string{"qp"}},
		{.name="qp", .value=int64_t{36}},
		{.name="qpa", .value=false},
		{.name="threads", .value=int64_t{1}},
		{.name="bit-depth", .value=std::string{"source"}},
		{.name="chroma-subsampling", .value=std::string{"source"}},
		{.name="color-primaries", .value=std::string{"source"}},
		{.name="transfer", .value=std::string{"source"}},
		{.name="matrix", .value=std::string{"source"}},
		{.name="range", .value=std::string{"source"}},
		{.name="tone-map", .value=std::string{"none"}},
	};
	EncodeResult const vvencGuiEncoded = run_backend_encode(*vvenc, vvencGuiImage, vvencGuiParams);
	CODEC_VIS_CHECK(!vvencGuiEncoded.encoded.hevcAnnexB.empty());
	DecodeResult const vvencGuiPreview = vvenc->decodePreview(vvencGuiEncoded.encoded);
	CODEC_VIS_CHECK(vvencGuiPreview.image && vvencGuiPreview.error.empty());

	std::vector<codec_gui::EncoderParam> vvencFastParams = {
		{.name="preset", .value=std::string{"faster"}},
		{.name="qp", .value=int64_t{36}},
		{.name="bitrate", .value=int64_t{0}},
		{.name="qpa", .value=false},
		{.name="threads", .value=int64_t{1}},
	};
	const codec_gui::RawImage vvenc444Image = make_yuv444_image(48, 32);
	try {
		(void)vvenc->encode(vvenc444Image, vvencFastParams);
		CODEC_VIS_CHECK(false && "VVenC should reject unsupported 4:4:4 input");
	} catch (const std::runtime_error&) {
	}

	{
		auto input = make_yuv420_image(64, 64);
		input.color = {.primaries=codec_gui::ColorPrimaries::BT2020,
		               .transfer=codec_gui::TransferCharacteristics::PQ,
		               .matrix=codec_gui::MatrixCoefficients::BT2020NonConstant,
		               .range=codec_gui::ColorRange::Full};
		const std::vector<codec_gui::EncoderParam> params{{.name="preset", .value=std::string{"ultrafast"}}};
		auto encoded = x265->encode(input, params);
		// Decoder must observe the stream even when an out-of-band hint is wrong.
		encoded.codedColor = codec_gui::ColorDescription{};
		const auto decoded = x265->decodePreview(encoded);
		CODEC_VIS_CHECK(decoded.image);
		CODEC_VIS_CHECK(decoded.image->color.primaries == input.color.primaries);
		CODEC_VIS_CHECK(decoded.image->color.transfer == input.color.transfer);
		CODEC_VIS_CHECK(decoded.image->color.matrix == input.color.matrix);
		CODEC_VIS_CHECK(decoded.image->color.range == input.color.range);
	}
	const EncoderBackend* uvg266 = find_backend(backends, BackendId{6});
	CODEC_VIS_CHECK(uvg266 != nullptr);
	std::vector<codec_gui::EncoderParam> uvg266Params = {
		{.name="preset", .value=std::string{"medium"}},
		{.name="qp", .value=int64_t{48}},
		{.name="rdo", .value=int64_t{0}},
		{.name="rdoq", .value=true},
		{.name="sao", .value=true},
		{.name="alf", .value=false},
		{.name="lossless", .value=false},
		{.name="mip", .value=true},
		{.name="lfnst", .value=true},
		{.name="isp", .value=true},
	};
	const codec_gui::RawImage nonSquareImage = make_yuv420_image(96, 64);
    auto unsafeAlf = uvg266Params;
    for (auto& p : unsafeAlf) if (p.name == "rdo") p.value = int64_t{2};
    for (auto& p : unsafeAlf) if (p.name == "alf") p.value = true;
    bool rejectedAlf = false;
    try { (void)uvg266->encode(nonSquareImage, unsafeAlf); }
    catch (const std::invalid_argument& error) { rejectedAlf = std::string(error.what()).find("ALF") != std::string::npos; }
    CODEC_VIS_CHECK(rejectedAlf);
    auto unsupportedLossless = uvg266Params;
    for (auto& p : unsupportedLossless) if (p.name == "lossless") p.value = true;
    bool rejectedLossless = false;
    try { (void)uvg266->encode(nonSquareImage, unsupportedLossless); }
    catch (const std::invalid_argument& error) { rejectedLossless = std::string(error.what()).find("lossless") != std::string::npos; }
    CODEC_VIS_CHECK(rejectedLossless);
	codec_gui::EncodedImage const uvg266Encoded = uvg266->encode(nonSquareImage, uvg266Params);
	CODEC_VIS_CHECK(!uvg266Encoded.hevcAnnexB.empty());
	DecodeResult const uvg266Preview = uvg266->decodePreview(uvg266Encoded);
	if (!uvg266Preview.image) std::cerr << "uvg266 preview failed: " << uvg266Preview.error << '\n';
	CODEC_VIS_CHECK(uvg266Preview.image);
	CODEC_VIS_CHECK(uvg266Preview.image->width == nonSquareImage.width);
	CODEC_VIS_CHECK(uvg266Preview.image->height == nonSquareImage.height);

	std::vector<codec_gui::EncoderParam> uvg266FastParams = {
		{.name="preset", .value=std::string{"faster"}},
		{.name="qp", .value=int64_t{48}},
		{.name="rdo", .value=int64_t{1}},
		{.name="rdoq", .value=false},
		{.name="sao", .value=true},
		{.name="alf", .value=true},
		{.name="lossless", .value=false},
		{.name="mip", .value=true},
		{.name="lfnst", .value=true},
		{.name="isp", .value=true},
	};
	const codec_gui::RawImage uvg422Image = make_yuv422_image(64, 48);
	try {
		(void)uvg266->encode(uvg422Image, uvg266FastParams);
		CODEC_VIS_CHECK(false && "uvg266 should reject unsupported 4:2:2 input");
	} catch (const std::runtime_error&) {
	}

	return 0;
}
