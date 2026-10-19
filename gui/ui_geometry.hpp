#pragma once
#include "app_state.hpp"
#include <algorithm>
#include <cmath>
namespace codec_gui::gui::ui {
	inline bool valid_rect(Rect r) {
		return std::isfinite(r.x) && std::isfinite(r.y) && std::isfinite(r.w) && std::isfinite(r.h) && r.w > 0 &&
			   r.h > 0;
	}
	inline Rect intersect(Rect a, Rect b) {
		const float x = std::max(a.x, b.x), y = std::max(a.y, b.y);
		return {x, y, std::max(0.f, std::min(a.x + a.w, b.x + b.w) - x),
				std::max(0.f, std::min(a.y + a.h, b.y + b.h) - y)};
	}
	inline bool contains(Rect r, Point p) {
		return valid_rect(r) && p.x >= r.x && p.y >= r.y && p.x < r.x + r.w && p.y < r.y + r.h;
	}
	inline Rect inset(Rect r, float padding) {
		return {r.x + padding, r.y + padding, std::max(0.f, r.w - 2 * padding), std::max(0.f, r.h - 2 * padding)};
	}
	inline constexpr Rect unlimited_clip{-1000000, -1000000, 2000000, 2000000};
} // namespace codec_gui::gui::ui
