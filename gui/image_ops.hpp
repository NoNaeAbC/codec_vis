#pragma once

#include "../codec_gui_x265.hpp"

#include <memory>
#include <string>

namespace codec_gui::gui {

	struct DerivedImageResult {
		std::shared_ptr<RawImage> image;
		std::string error;
	};

	[[nodiscard]] DerivedImageResult compute_absolute_difference(
		const RawImage& a,
		const RawImage& b,
		double gain
	);

} // namespace codec_gui::gui
