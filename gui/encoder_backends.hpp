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
	};

	struct CapabilityResult {
		CapabilitySnapshot snapshot;
		std::vector<EncoderParamInfo> params;
	};

	struct EncodeResult {
		EncodedImage encoded;
		EncodedMetadata metadata;
		std::shared_ptr<const RawImage> comparisonReference;
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

} // namespace codec_gui::gui
