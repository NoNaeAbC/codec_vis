#pragma once

#include "../codec_gui_x265.hpp"
#include "app_state.hpp"
#include "raw_image_conversion.hpp"

#include <optional>
#include <string>
#include <vector>
#include <span>

namespace codec_gui::gui {
	struct MetricDescriptor {
		std::string_view id, label, unit, accuracyNote;
		bool higherIsBetter;
		double (*evaluate)(const LinearBt2020Image&, const LinearBt2020Image&);
	};
	// This is the only list of offered metrics. Adding an adapter and its
	// conformance tests adds a metric; removing an entry removes it from the UI.
	[[nodiscard]] std::span<const MetricDescriptor> metric_catalog();
	[[nodiscard]] QualityMetricRecord compute_metric(std::string_view id, const LinearBt2020Image& reference, const LinearBt2020Image& candidate);

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
