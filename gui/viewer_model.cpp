#include "viewer_model.hpp"
#include "app_state.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <functional>
#include <optional>
#include <span>
#include <vector>

namespace codec_gui::gui {
namespace {

[[nodiscard]] bool contains_pane(std::span<const Pane> panes, PaneId id) {
	return std::ranges::any_of(panes, [id](const Pane& pane) {
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
	const auto it = std::ranges::find_if(panes, [id](const Pane& pane) {
		return pane.id == id;
	});
	return it == panes.end() ? nullptr : &*it;
}

ViewportTransform clamp_transform(ViewportTransform transform, const ImageObject& image, Rect paneRect) {
	if (image.width <= 0 || image.height <= 0 || paneRect.w <= 0 || paneRect.h <= 0 || transform.scale <= 0.0) {
		return transform;
	}
	const double halfVisibleW = static_cast<double>(paneRect.w) * 0.5 / (transform.scale * pixel_aspect_ratio(image).value());
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
	std::erase_if(mode.paneOrder, [&](PaneId id) {
			return !contains_pane(panes, id);
		});
	for (const Pane& pane : panes) {
		if (std::ranges::find(mode.paneOrder, pane.id) == mode.paneOrder.end()) {
			mode.paneOrder.push_back(pane.id);
		}
	}
	while (mode.paneOrder.size() < required_panes(mode.kind)) {
		mode.paneOrder.push_back(createPane());
	}
}

std::vector<PaneRect> compute_pane_rects(const ViewModeState& mode, std::span<const Pane> panes, Rect viewerRect) {
	std::vector<PaneId> ids;
	for (PaneId const id : mode.paneOrder) {
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
			out.push_back({.pane=ids.front(), .rect=viewerRect});
			break;
		case ViewModeKind::Split: {
			if (ids.size() < 2) {
				out.push_back({.pane=ids.front(), .rect=viewerRect});
				break;
			}
			const float split = viewerRect.x + viewerRect.w * static_cast<float>(std::clamp(mode.splitPosition, 0.0, 1.0));
			out.push_back({.pane=ids[0], .rect={.x=viewerRect.x, .y=viewerRect.y, .w=split - viewerRect.x, .h=viewerRect.h}});
			out.push_back({.pane=ids[1], .rect={.x=split, .y=viewerRect.y, .w=viewerRect.x + viewerRect.w - split, .h=viewerRect.h}});
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
				out.push_back({.pane=ids[i], .rect={.x=viewerRect.x + cellW * static_cast<float>(col), .y=viewerRect.y + cellH * static_cast<float>(row), .w=cellW, .h=cellH}});
			}
			break;
		}
		case ViewModeKind::Grid: {
			const std::size_t n = ids.size();
			const auto cols = static_cast<std::size_t>(std::ceil(std::sqrt(static_cast<double>(n))));
			const std::size_t rows = (n + cols - 1) / cols;
			const float cellW = viewerRect.w / static_cast<float>(cols);
			const float cellH = viewerRect.h / static_cast<float>(rows);
			for (std::size_t i = 0; i < n; ++i) {
				const std::size_t col = i % cols;
				const std::size_t row = i / cols;
				out.push_back({.pane=ids[i], .rect={.x=viewerRect.x + cellW * static_cast<float>(col), .y=viewerRect.y + cellH * static_cast<float>(row), .w=cellW, .h=cellH}});
			}
			break;
		}
	}
	return out;
}

ViewportTransform fit_transform(int imageWidth, int imageHeight, Rect paneRect, float outputScale, PixelAspectRatio pixelAspectRatio) {
	ViewportTransform transform;
	if (imageWidth <= 0 || imageHeight <= 0 || paneRect.w <= 0 || paneRect.h <= 0) {
		return transform;
	}
	const double scaleX = static_cast<double>(paneRect.w) / (static_cast<double>(imageWidth) * pixelAspectRatio.value()) / outputScale;
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
	const auto localX = static_cast<double>(pointer.x - (paneRect.x + paneRect.w * 0.5f));
	const auto localY = static_cast<double>(pointer.y - (paneRect.y + paneRect.h * 0.5f));
	const double imageX = transform.centerX + localX / (transform.scale * pixel_aspect_ratio(image).value());
	const double imageY = transform.centerY + localY / transform.scale;
	transform.scale = std::clamp(transform.scale * zoomFactor, 0.01, 256.0);
	transform.centerX = imageX - localX / (transform.scale * pixel_aspect_ratio(image).value());
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
	transform.centerX -= static_cast<double>(deltaX) / (transform.scale * pixel_aspect_ratio(image).value());
	transform.centerY -= static_cast<double>(deltaY) / transform.scale;
	return clamp_transform(transform, image, paneRect);
}

Rect image_rect_in_pane(const Pane& pane, const ImageObject& image, Rect paneRect) {
	if (image.width <= 0 || image.height <= 0 || pane.transform.scale <= 0.0) {
		return paneRect;
	}
	const auto w = static_cast<float>(static_cast<double>(image.width) * pane.transform.scale * pixel_aspect_ratio(image).value());
	const auto h = static_cast<float>(static_cast<double>(image.height) * pane.transform.scale);
	const float cx = paneRect.x + paneRect.w * 0.5f + static_cast<float>((static_cast<double>(image.width) * 0.5 - pane.transform.centerX) * pane.transform.scale * pixel_aspect_ratio(image).value());
	const float cy = paneRect.y + paneRect.h * 0.5f + static_cast<float>((static_cast<double>(image.height) * 0.5 - pane.transform.centerY) * pane.transform.scale);
	return Rect{.x=cx - w * 0.5f, .y=cy - h * 0.5f, .w=w, .h=h};
}

std::optional<ImagePixelCoord> pane_to_image_coord(const Pane& pane, const ImageObject& image, Rect paneRect, Point pointer) {
	if (image.width <= 0 || image.height <= 0 || pane.transform.scale <= 0.0) {
		return std::nullopt;
	}
	const auto localX = static_cast<double>(pointer.x - (paneRect.x + paneRect.w * 0.5f));
	const auto localY = static_cast<double>(pointer.y - (paneRect.y + paneRect.h * 0.5f));
	const int imageX = static_cast<int>(std::floor(pane.transform.centerX + localX / (pane.transform.scale * pixel_aspect_ratio(image).value())));
	const int imageY = static_cast<int>(std::floor(pane.transform.centerY + localY / pane.transform.scale));
	if (imageX < 0 || imageY < 0 || imageX >= image.width || imageY >= image.height) {
		return std::nullopt;
	}
	return ImagePixelCoord{.x=imageX, .y=imageY};
}

} // namespace codec_gui::gui
