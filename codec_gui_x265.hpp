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

	struct ParamCondition {
		std::string parameter;
		std::vector<std::string> acceptedValues;
		std::string explanation;
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
		// Every condition must match before the option is interactive or sent to
		// the encoder. This makes mutually exclusive codec modes explicit.
		std::vector<ParamCondition> enabledWhen;
		// Keep range validation, but edit the value as text when a slider would
		// have meaningless precision (for example a multi-gigabit bitrate).
		bool directNumericInput = false;
		std::optional<int64_t> automaticIntValue;
		std::string automaticLabel;
	};

	using ParamValue = std::variant<bool, int64_t, double, std::string>;

	struct EncoderParam {
		std::string name;
		ParamValue  value;
	};

	enum class PixelFormat {
		YUV420P8,
		YUV420P10LE,
		YUV420P12LE,
		YUV420P14LE,
		YUV422P8,
		YUV422P10LE,
		YUV422P12LE,
		YUV422P14LE,
		YUV444P8,
		YUV444P10LE,
		YUV444P12LE,
		YUV444P14LE,
		Gray8,
		Gray10LE,
		Gray12LE,
		Gray14LE,
	};

	// Numeric values follow ColourPrimaries in ITU-T H.273 (07/2024).
	enum class ColorPrimaries : uint8_t {
		BT709 = 1,
		Unspecified = 2,
		BT470M = 4,
		BT470BG = 5,
		BT601_525 = 6,
		Film = 8,
		BT2020 = 9,
		XYZ = 10,
		DCIP3 = 11,
		DisplayP3 = 12,
	};

	// Numeric values follow TransferCharacteristics in ITU-T H.273 (07/2024).
	enum class TransferCharacteristics : uint8_t {
		BT709 = 1,
		Unspecified = 2,
		Gamma22 = 4,
		Gamma28 = 5,
		BT601 = 6,
		Linear = 8,
		SRGB = 13,
		BT2020_10 = 14,
		BT2020_12 = 15,
		PQ = 16,
		HLG = 18,
	};

	// Numeric values follow MatrixCoefficients in ITU-T H.273 (07/2024).
	enum class MatrixCoefficients : uint8_t {
		Identity = 0,
		BT709 = 1,
		Unspecified = 2,
		BT601 = 6,
		YCgCo = 8,
		BT2020NonConstant = 9,
		BT2020Constant = 10,
		ChromaticityDerivedNonConstant = 12,
		ChromaticityDerivedConstant = 13,
		ICtCp = 14,
		IPTC2 = 15,
		YCgCoRe = 16,
		YCgCoRo = 17,
	};

	enum class Chroma420SampleLocation : uint8_t {
		LeftCenter = 0,
		Center = 1,
		TopLeft = 2,
		TopCenter = 3,
		BottomLeft = 4,
		BottomCenter = 5,
	};

	enum class ColorRange {
		Limited,
		Full,
	};

	struct ColorDescription {
		ColorPrimaries primaries = ColorPrimaries::Unspecified;
		TransferCharacteristics transfer = TransferCharacteristics::Unspecified;
		MatrixCoefficients matrix = MatrixCoefficients::Unspecified;
		ColorRange range = ColorRange::Limited;
		std::optional<Chroma420SampleLocation> chroma420Location;
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
		// Color description of the samples supplied to an RGB-based encoder. RGB
		// formats do not carry a YCbCr matrix or range, but the preview decoder
		// needs both to reconstruct comparable YCbCr samples for quality metrics.
		std::optional<ColorDescription> codedColor;
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

	struct VaapiDeviceInfo {
		std::string path;
		std::string vendor;
		std::string error;
		bool initialized = false;
		bool supportsHevcEncode = false;
		bool supportsAv1Encode = false;
	};

	[[nodiscard]] std::vector<VaapiDeviceInfo> discover_vaapi_devices();
	[[nodiscard]] std::vector<VaapiDeviceInfo> probe_vaapi_devices(std::span<const std::string> paths);

	std::vector<EncoderParamInfo> query_vaapi_hevc_parameters();
	std::vector<EncoderParamInfo> query_vaapi_hevc_parameters(std::string_view device);
	EncodedImage encode_vaapi_hevc_still_image(const RawImage& image, std::span<const EncoderParam> params);
	EncodedImage encode_vaapi_hevc_still_image(const RawImage& image, std::span<const EncoderParam> params, std::string_view device);

	std::vector<EncoderParamInfo> query_vaapi_av1_parameters();
	std::vector<EncoderParamInfo> query_vaapi_av1_parameters(std::string_view device);
	EncodedImage encode_vaapi_av1_still_image(const RawImage& image, std::span<const EncoderParam> params);
	EncodedImage encode_vaapi_av1_still_image(const RawImage& image, std::span<const EncoderParam> params, std::string_view device);

} // namespace codec_gui
