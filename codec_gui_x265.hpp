// codec_gui_x265.hpp
#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

extern "C" {
#include <x265.h>
}

namespace codec_gui {

	enum class ParamKind {
		Bool,
		Int,
		Float,
		Enum,
		String,
	};

	struct IntRange {
		int64_t min;
		int64_t max;
		int64_t step = 1;
	};

	struct FloatRange {
		double min;
		double max;
		double step = 0.01;
	};

	struct EnumValue {
		std::string value;
		std::string label;
	};

	struct EncoderParamInfo {
		std::string name;  // x265 CLI/API parse name
		std::string label; // GUI label
		std::string group; // GUI grouping
		ParamKind   kind;

		std::variant<std::monostate, bool, int64_t, double, std::string> defaultValue;

		std::optional<IntRange>   intRange;
		std::optional<FloatRange> floatRange;
		std::vector<EnumValue>    enumValues;

		std::string help;
		bool        relevantForStillImage = true;
	};

	using ParamValue = std::variant<bool, int64_t, double, std::string>;

	struct EncoderParam {
		std::string name;
		ParamValue  value;
	};

	enum class PixelFormat {
		YUV420P8,
		YUV420P10LE,
		YUV422P8,
		YUV422P10LE,
		YUV444P8,
		YUV444P10LE,
		Gray8,
		Gray10LE,
	};

	enum class ColorPrimaries {
		Unspecified,
		BT709,
		BT2020,
	};

	enum class TransferCharacteristics {
		Unspecified,
		SRGB,
		BT709,
		PQ,
		HLG,
		Linear,
	};

	enum class MatrixCoefficients {
		Unspecified,
		BT709,
		BT2020NonConstant,
		Identity,
	};

	enum class ColorRange {
		Limited,
		Full,
	};

	struct ColorDescription {
		ColorPrimaries primaries = ColorPrimaries::BT709;
		TransferCharacteristics transfer = TransferCharacteristics::SRGB;
		MatrixCoefficients matrix = MatrixCoefficients::BT709;
		ColorRange range = ColorRange::Limited;
	};

	struct ImagePlane {
		std::vector<uint8_t> bytes;
		int                        strideBytes = 0;
	};

	struct RawImage {
		int         width  = 0;
		int         height = 0;
		PixelFormat format = PixelFormat::YUV420P8;
		ColorDescription color;
		ImagePlane  planes[3];
	};

	struct EncodedImage {
		// Encoded output bytes. HEVC/VVC are Annex B; AV1/AV2 are IVF for player autodetection.
		std::vector<std::byte> hevcAnnexB;
		// Optional reconstructed/decoded preview provided directly by an encoder.
		std::shared_ptr<const RawImage> previewImage;
	};

	std::vector<EncoderParamInfo> query_x265_parameters();

	EncodedImage encode_x265_still_image(const RawImage &image, std::span<const EncoderParam> params);


	std::vector<EncoderParamInfo> query_vvenc_parameters();

	EncodedImage encode_vvenc_still_image(const RawImage &image, std::span<const EncoderParam> params);

	std::vector<EncoderParamInfo> query_svt_av1_parameters();

	EncodedImage encode_svt_av1_still_image(const RawImage &image, std::span<const EncoderParam> params);

	std::vector<EncoderParamInfo> query_uvg266_parameters();

	EncodedImage encode_uvg266_still_image(const RawImage &image, std::span<const EncoderParam> params);

	std::vector<EncoderParamInfo> query_av2_parameters();

	EncodedImage encode_av2_still_image(const RawImage &image, std::span<const EncoderParam> params);

	std::vector<EncoderParamInfo> query_jpegls_parameters();
	EncodedImage encode_jpegls_still_image(const RawImage& image, std::span<const EncoderParam> params);

	std::vector<EncoderParamInfo> query_jpeg_parameters();
	EncodedImage encode_jpeg_still_image(const RawImage& image, std::span<const EncoderParam> params);

	std::vector<EncoderParamInfo> query_jpeg2000_parameters();
	EncodedImage encode_jpeg2000_still_image(const RawImage& image, std::span<const EncoderParam> params);

	std::vector<EncoderParamInfo> query_jpegxl_parameters();
	EncodedImage encode_jpegxl_still_image(const RawImage& image, std::span<const EncoderParam> params);

	std::vector<EncoderParamInfo> query_jpegxr_parameters();
	EncodedImage encode_jpegxr_still_image(const RawImage& image, std::span<const EncoderParam> params);

	std::vector<EncoderParamInfo> query_png_parameters();
	EncodedImage encode_png_still_image(const RawImage& image, std::span<const EncoderParam> params);

	std::vector<EncoderParamInfo> query_x264_parameters();
	EncodedImage encode_x264_intra_still_image(const RawImage& image, std::span<const EncoderParam> params);

		std::vector<EncoderParamInfo> query_vaapi_hevc_parameters();

		EncodedImage encode_vaapi_hevc_still_image(const RawImage &image, std::span<const EncoderParam> params);

		std::vector<EncoderParamInfo> query_vaapi_av1_parameters();

	EncodedImage encode_vaapi_av1_still_image(const RawImage &image, std::span<const EncoderParam> params);

} // namespace codec_gui
