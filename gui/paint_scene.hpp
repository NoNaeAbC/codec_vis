#pragma once
#include "draw_list.hpp"
#include "ui_geometry.hpp"
#include <algorithm>
#include <span>
#include <stdexcept>
namespace codec_gui::gui {
	struct PaintItem {
		std::size_t commandIndex = 0;
		Rect        clip;
	};
	// Clips are structural, independent of z order. Sorting is stable within a layer;
	// text, images and geometry all follow this SAME painter order.
	inline std::vector<PaintItem> compile_paint_scene(std::span<const DrawCommand> commands) {
		std::vector<Rect>      clips{ui::unlimited_clip};
		std::vector<PaintItem> result;
		result.reserve(commands.size());
		for (std::size_t i = 0; i < commands.size(); ++i) {
			const auto &command = commands[i];
			if (command.kind == DrawCommandKind::ScissorBegin) {
				clips.push_back(ui::intersect(clips.back(), command.rect));
			} else if (command.kind == DrawCommandKind::ScissorEnd) {
				if (clips.size() == 1) throw std::invalid_argument("Unmatched UI clip end");
				clips.pop_back();
			} else if (ui::valid_rect(clips.back())) {
				result.push_back({i, clips.back()});
			}
		}
		if (clips.size() != 1) throw std::invalid_argument("Unclosed UI clip");
		std::stable_sort(result.begin(), result.end(), [&](const auto &a, const auto &b) {
			return commands[a.commandIndex].layer < commands[b.commandIndex].layer;
		});
		return result;
	}
} // namespace codec_gui::gui
