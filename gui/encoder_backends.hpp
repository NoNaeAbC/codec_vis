#pragma once

#include "app_state.hpp"

#include <chrono>
#include <functional>
#include <span>
#include <string>
#include <vector>

namespace codec_gui::gui {

	struct DecodeResult {
		std::shared_ptr<const RawImage> image;
		std::string error;
		// Source import can pass an embedded RGB/gray ICC profile to the common
		// color-management path instead of guessing a display interpretation.
		std::vector<uint8_t> iccProfile;
		// Completed codec work, excluding device/context setup and CPU readback.
		std::optional<double> codecSeconds;
	};

	struct CapabilityResult {
		CapabilitySnapshot snapshot;
		std::vector<EncoderParamInfo> params;
	};

	[[nodiscard]] inline bool backend_supports_pixel_aspect_ratio(BackendId id) {
		return id.value == 1 || id.value == 2 || id.value == 6 || id.value == 14 || id.value == 16 || id.value == 9 || id.value == 13;
	}

	enum class InputGeometryPolicy {
		Preserve,
		PadToChromaAlignment,
	};

	struct EncodeResult {
		EncodedImage encoded;
		EncodedMetadata metadata;
		std::shared_ptr<const RawImage> comparisonReference;
	};

	// One immutable source/preparation retained per worker. Codec-only settings
	// (QP, speed, bitrate) never invalidate this cache.
	struct EncodePreparationCache {
		std::shared_ptr<const RawImage> source, prepared;
		BackendId backend;
		std::vector<std::pair<std::string,ParamValue>> conversionKey;
	};

	struct EncoderBackend {
		BackendId id;
		std::string name;
		std::string codec;
		BackendKind kind = BackendKind::Software;

		std::function<CapabilityResult()> queryCapabilities;
		std::function<EncodedImage(const RawImage&, std::span<const EncoderParam>)> encode;
		DecodeResult (*decodePreview)(const EncodedImage&);
		std::vector<ParamSummary> (*summarizeParams)(std::span<const EncoderParam>);
		InputGeometryPolicy inputGeometry = InputGeometryPolicy::Preserve;
		std::string device;
	};

	[[nodiscard]] std::vector<EncoderBackend> initial_encoder_backends();
	[[nodiscard]] std::vector<EncoderBackend> initial_encoder_backends(std::span<const VaapiDeviceInfo> vaapiDevices);
	[[nodiscard]] const EncoderBackend* find_backend(std::span<const EncoderBackend> backends, BackendId id);
	[[nodiscard]] std::vector<BackendInfo> query_backend_infos(std::span<const EncoderBackend> backends);
	[[nodiscard]] EncodeResult run_backend_encode(
		const EncoderBackend& backend,
		const RawImage& image,
		std::span<const EncoderParam> params
	);
	[[nodiscard]] EncodeResult run_backend_encode(const EncoderBackend&, std::shared_ptr<const RawImage>,
		std::span<const EncoderParam>, EncodePreparationCache&);
	[[nodiscard]] DecodeResult decode_backend_preview(const EncoderBackend&,const EncodedImage&);

} // namespace codec_gui::gui
