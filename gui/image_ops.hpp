#pragma once

#include "../codec_gui_x265.hpp"

#include <memory>
#include <string>

namespace codec_gui::gui {

	struct DerivedImageResult {
		std::shared_ptr<RawImage> image;
		std::string               error;
	};

	[[nodiscard]] DerivedImageResult compute_absolute_difference(const RawImage &a, const RawImage &b, double gain);

	// The workbench compares both interpretations in floating-point linear BT.2020.
	// Only the resulting diagnostic display heatmap is quantized/clipped.
	[[nodiscard]] DerivedImageResult compute_linear_difference(const RawImage &, const RawImage &, double gain);

} // namespace codec_gui::gui
