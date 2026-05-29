#pragma once

#include "app_state.hpp"

#include <functional>
#include <optional>
#include <span>
#include <vector>

namespace codec_gui::gui {

	struct PaneRect {
		PaneId pane;
		Rect rect;
	};

	struct ImagePixelCoord {
		int x = 0;
		int y = 0;
	};

	void apply_mode_transition(
		std::vector<Pane>& panes,
		ViewModeState& mode,
		const std::function<PaneId()>& createPane
	);

	[[nodiscard]] std::vector<PaneRect> compute_pane_rects(
		const ViewModeState& mode,
		std::span<const Pane> panes,
		Rect viewerRect
	);

	[[nodiscard]] ViewportTransform fit_transform(
		int imageWidth,
		int imageHeight,
		Rect paneRect,
		float outputScale
	);

	[[nodiscard]] ViewportTransform one_to_one_transform(
		int imageWidth,
		int imageHeight
	);

	[[nodiscard]] ViewportTransform zoom_transform(
		ViewportTransform transform,
		const ImageObject& image,
		Rect paneRect,
		Point pointer,
		double zoomFactor
	);

	[[nodiscard]] ViewportTransform pan_transform(
		ViewportTransform transform,
		const ImageObject& image,
		Rect paneRect,
		float deltaX,
		float deltaY
	);

	[[nodiscard]] Rect image_rect_in_pane(
		const Pane& pane,
		const ImageObject& image,
		Rect paneRect
	);

	[[nodiscard]] std::optional<ImagePixelCoord> pane_to_image_coord(
		const Pane& pane,
		const ImageObject& image,
		Rect paneRect,
		Point pointer
	);

} // namespace codec_gui::gui
