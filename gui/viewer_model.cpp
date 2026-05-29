#include "viewer_model.hpp"

#include <algorithm>
#include <cmath>
#include <iterator>

namespace codec_gui::gui {
namespace {

[[nodiscard]] bool contains_pane(std::span<const Pane> panes, PaneId id) {
	return std::any_of(panes.begin(), panes.end(), [id](const Pane& pane) {
		return pane.id == id;
	});
}

[[nodiscard]] std::size_t required_panes(ViewModeKind kind) {
	switch (kind) {
		case ViewModeKind::Single:
			return 1;
		case ViewModeKind::SideBySide:
			return 2;
		case ViewModeKind::Split:
		case ViewModeKind::Blink:
		case ViewModeKind::Difference:
			return 2;
		case ViewModeKind::Grid:
			return 1;
	}
	return 1;
}

[[nodiscard]] const Pane* pane_by_id(std::span<const Pane> panes, PaneId id) {
	const auto it = std::find_if(panes.begin(), panes.end(), [id](const Pane& pane) {
		return pane.id == id;
	});
	return it == panes.end() ? nullptr : &*it;
}

ViewportTransform clamp_transform(ViewportTransform transform, const ImageObject& image, Rect paneRect) {
	if (image.width <= 0 || image.height <= 0 || paneRect.w <= 0 || paneRect.h <= 0 || transform.scale <= 0.0) {
		return transform;
	}
	const double halfVisibleW = static_cast<double>(paneRect.w) * 0.5 / transform.scale;
	const double halfVisibleH = static_cast<double>(paneRect.h) * 0.5 / transform.scale;
	const double marginX = std::min(static_cast<double>(image.width) * 0.5, halfVisibleW);
	const double marginY = std::min(static_cast<double>(image.height) * 0.5, halfVisibleH);
	transform.centerX = std::clamp(transform.centerX, -marginX, static_cast<double>(image.width) + marginX);
	transform.centerY = std::clamp(transform.centerY, -marginY, static_cast<double>(image.height) + marginY);
	transform.scale = std::clamp(transform.scale, 0.01, 256.0);
	return transform;
}

} // namespace

void apply_mode_transition(std::vector<Pane>& panes, ViewModeState& mode, const std::function<PaneId()>& createPane) {
	mode.paneOrder.erase(
		std::remove_if(mode.paneOrder.begin(), mode.paneOrder.end(), [&](PaneId id) {
			return !contains_pane(panes, id);
		}),
		mode.paneOrder.end()
	);
	for (const Pane& pane : panes) {
		if (std::find(mode.paneOrder.begin(), mode.paneOrder.end(), pane.id) == mode.paneOrder.end()) {
			mode.paneOrder.push_back(pane.id);
		}
	}
	while (mode.paneOrder.size() < required_panes(mode.kind)) {
		mode.paneOrder.push_back(createPane());
	}
}

std::vector<PaneRect> compute_pane_rects(const ViewModeState& mode, std::span<const Pane> panes, Rect viewerRect) {
	std::vector<PaneId> ids;
	for (PaneId id : mode.paneOrder) {
		if (pane_by_id(panes, id) != nullptr) {
			ids.push_back(id);
		}
	}
	if (ids.empty()) {
		return {};
	}

	std::vector<PaneRect> out;
	switch (mode.kind) {
		case ViewModeKind::Single:
		case ViewModeKind::Blink:
		case ViewModeKind::Difference:
			out.push_back({ids.front(), viewerRect});
			break;
		case ViewModeKind::Split: {
			if (ids.size() < 2) {
				out.push_back({ids.front(), viewerRect});
				break;
			}
			const float split = viewerRect.x + viewerRect.w * static_cast<float>(std::clamp(mode.splitPosition, 0.0, 1.0));
			out.push_back({ids[0], {viewerRect.x, viewerRect.y, split - viewerRect.x, viewerRect.h}});
			out.push_back({ids[1], {split, viewerRect.y, viewerRect.x + viewerRect.w - split, viewerRect.h}});
			break;
		}
		case ViewModeKind::SideBySide: {
			const std::size_t visible = ids.size();
			const std::size_t cols = std::min<std::size_t>(visible, 4);
			const std::size_t rows = (visible + cols - 1) / cols;
			const float cellW = viewerRect.w / static_cast<float>(cols);
			const float cellH = viewerRect.h / static_cast<float>(rows);
			for (std::size_t i = 0; i < visible; ++i) {
				const std::size_t col = i % cols;
				const std::size_t row = i / cols;
				out.push_back({ids[i], {viewerRect.x + cellW * static_cast<float>(col), viewerRect.y + cellH * static_cast<float>(row), cellW, cellH}});
			}
			break;
		}
		case ViewModeKind::Grid: {
			const std::size_t n = ids.size();
			const std::size_t cols = static_cast<std::size_t>(std::ceil(std::sqrt(static_cast<double>(n))));
			const std::size_t rows = (n + cols - 1) / cols;
			const float cellW = viewerRect.w / static_cast<float>(cols);
			const float cellH = viewerRect.h / static_cast<float>(rows);
			for (std::size_t i = 0; i < n; ++i) {
				const std::size_t col = i % cols;
				const std::size_t row = i / cols;
				out.push_back({ids[i], {viewerRect.x + cellW * col, viewerRect.y + cellH * row, cellW, cellH}});
			}
			break;
		}
	}
	return out;
}

ViewportTransform fit_transform(int imageWidth, int imageHeight, Rect paneRect, float outputScale) {
	ViewportTransform transform;
	if (imageWidth <= 0 || imageHeight <= 0 || paneRect.w <= 0 || paneRect.h <= 0) {
		return transform;
	}
	const double scaleX = static_cast<double>(paneRect.w) / static_cast<double>(imageWidth) / outputScale;
	const double scaleY = static_cast<double>(paneRect.h) / static_cast<double>(imageHeight) / outputScale;
	transform.scale = std::min(scaleX, scaleY);
	transform.centerX = static_cast<double>(imageWidth) * 0.5;
	transform.centerY = static_cast<double>(imageHeight) * 0.5;
	return transform;
}

ViewportTransform one_to_one_transform(int imageWidth, int imageHeight) {
	ViewportTransform transform;
	transform.scale = 1.0;
	transform.centerX = static_cast<double>(imageWidth) * 0.5;
	transform.centerY = static_cast<double>(imageHeight) * 0.5;
	return transform;
}

ViewportTransform zoom_transform(
	ViewportTransform transform,
	const ImageObject& image,
	Rect paneRect,
	Point pointer,
	double zoomFactor
) {
	if (zoomFactor <= 0.0 || transform.scale <= 0.0) {
		return transform;
	}
	const double localX = static_cast<double>(pointer.x - (paneRect.x + paneRect.w * 0.5f));
	const double localY = static_cast<double>(pointer.y - (paneRect.y + paneRect.h * 0.5f));
	const double imageX = transform.centerX + localX / transform.scale;
	const double imageY = transform.centerY + localY / transform.scale;
	transform.scale = std::clamp(transform.scale * zoomFactor, 0.01, 256.0);
	transform.centerX = imageX - localX / transform.scale;
	transform.centerY = imageY - localY / transform.scale;
	return clamp_transform(transform, image, paneRect);
}

ViewportTransform pan_transform(
	ViewportTransform transform,
	const ImageObject& image,
	Rect paneRect,
	float deltaX,
	float deltaY
) {
	if (transform.scale <= 0.0) {
		return transform;
	}
	transform.centerX -= static_cast<double>(deltaX) / transform.scale;
	transform.centerY -= static_cast<double>(deltaY) / transform.scale;
	return clamp_transform(transform, image, paneRect);
}

Rect image_rect_in_pane(const Pane& pane, const ImageObject& image, Rect paneRect) {
	if (image.width <= 0 || image.height <= 0 || pane.transform.scale <= 0.0) {
		return paneRect;
	}
	const float w = static_cast<float>(static_cast<double>(image.width) * pane.transform.scale);
	const float h = static_cast<float>(static_cast<double>(image.height) * pane.transform.scale);
	const float cx = paneRect.x + paneRect.w * 0.5f + static_cast<float>((static_cast<double>(image.width) * 0.5 - pane.transform.centerX) * pane.transform.scale);
	const float cy = paneRect.y + paneRect.h * 0.5f + static_cast<float>((static_cast<double>(image.height) * 0.5 - pane.transform.centerY) * pane.transform.scale);
	return Rect{cx - w * 0.5f, cy - h * 0.5f, w, h};
}

std::optional<ImagePixelCoord> pane_to_image_coord(const Pane& pane, const ImageObject& image, Rect paneRect, Point pointer) {
	if (image.width <= 0 || image.height <= 0 || pane.transform.scale <= 0.0) {
		return std::nullopt;
	}
	const double localX = static_cast<double>(pointer.x - (paneRect.x + paneRect.w * 0.5f));
	const double localY = static_cast<double>(pointer.y - (paneRect.y + paneRect.h * 0.5f));
	const int imageX = static_cast<int>(std::floor(pane.transform.centerX + localX / pane.transform.scale));
	const int imageY = static_cast<int>(std::floor(pane.transform.centerY + localY / pane.transform.scale));
	if (imageX < 0 || imageY < 0 || imageX >= image.width || imageY >= image.height) {
		return std::nullopt;
	}
	return ImagePixelCoord{imageX, imageY};
}

} // namespace codec_gui::gui
