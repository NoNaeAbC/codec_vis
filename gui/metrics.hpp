#pragma once

#include "../codec_gui_x265.hpp"
#include "app_state.hpp"

#include <optional>
#include <string>
#include <vector>

namespace codec_gui::gui {

	struct MetricResult {
		std::optional<double> psnrY;
		std::optional<double> psnrAll;
		std::vector<QualityMetricRecord> metrics;
		std::string unavailableReason;
	};

	[[nodiscard]] MetricResult compute_psnr(const RawImage& reference, const RawImage& candidate);
	[[nodiscard]] MetricResult compute_quality_metrics(const RawImage& reference, const RawImage& candidate);
	[[nodiscard]] const QualityMetricRecord* metric_by_id(const EncodedMetadata& metadata, std::string_view id);
	[[nodiscard]] const QualityMetricRecord* primary_metric(const EncodedMetadata& metadata);

} // namespace codec_gui::gui
