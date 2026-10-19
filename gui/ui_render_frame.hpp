#pragma once
#include "paint_scene.hpp"
#include "text_atlas.hpp"
#include <cmath>
#include <limits>
namespace codec_gui::gui {
	// Four std430 vectors. Shapes and glyphs share the tiled compute compositor.
	struct UiInstance {
		float rect[4], uv[4], color[4], shape[4];
	};
	static_assert(sizeof(UiInstance) == 64);
	struct UiBatch {
		Rect        clip;
		uint32_t    first = 0, count = 0;
		std::size_t imageCommand = std::numeric_limits<std::size_t>::max();
		bool        image() const { return imageCommand != std::numeric_limits<std::size_t>::max(); }
	};
	struct UiRenderFrame {
		std::vector<UiInstance> instances;
		std::vector<UiBatch>    batches;
	};
	inline bool          same_clip(Rect a, Rect b) { return a.x == b.x && a.y == b.y && a.w == b.w && a.h == b.h; }
	inline UiRenderFrame compile_ui_frame(std::span<const DrawCommand> commands, const TextAtlas *atlas) {
		UiRenderFrame                               frame;
		std::vector<std::vector<const GlyphQuad *>> glyphs(commands.size());
		if (atlas)
			for (const auto &quad: atlas->quads)
				if (quad.commandIndex < commands.size()) glyphs[quad.commandIndex].push_back(&quad);
		auto append = [&](Rect rect, Rect uv, Color color, Rect clip, float kind, float radius = 0, float stroke = 0,
						  float angle = 0) {
			if (!ui::valid_rect(rect) || !ui::valid_rect(clip) || color.a <= 0 || !std::isfinite(angle)) return;
			if (frame.instances.size() >= std::numeric_limits<uint32_t>::max())
				throw std::length_error("Too many UI instances");
			if (frame.batches.empty() || frame.batches.back().image() || !same_clip(frame.batches.back().clip, clip))
				frame.batches.push_back({clip, static_cast<uint32_t>(frame.instances.size()), 0});
			++frame.batches.back().count;
			frame.instances.push_back({{rect.x, rect.y, rect.w, rect.h},
									   {uv.x, uv.y, uv.w, uv.h},
									   {color.r, color.g, color.b, color.a},
									   {kind, radius, stroke, angle}});
		};
		for (const auto &item: compile_paint_scene(commands)) {
			const auto &c     = commands[item.commandIndex];
			auto        color = c.color;
			color.a *= c.opacity;
			switch (c.kind) {
				case DrawCommandKind::Rect:
					append(c.rect, {}, color, item.clip, 0);
					break;
				case DrawCommandKind::RoundedRect:
					append(c.rect, {}, color, item.clip, 2, std::max(0.f, c.radius), std::max(0.f, c.strokeWidth));
					break;
				case DrawCommandKind::Border:
					append(c.rect, {}, color, item.clip, 2, std::max(0.f, c.radius),
						   c.strokeWidth > 0 ? c.strokeWidth : 1);
					break;
				case DrawCommandKind::Circle:
					append(c.rect, {}, color, item.clip, 3, 0, std::max(0.f, c.strokeWidth));
					break;
				case DrawCommandKind::Line: {
					const float dx = c.endPoint.x - c.rect.x, dy = c.endPoint.y - c.rect.y;
					const float length = std::hypot(dx, dy), thickness = std::max(1.f, c.strokeWidth);
					Rect        rect{(c.rect.x + c.endPoint.x - length - thickness) / 2,
									 (c.rect.y + c.endPoint.y - thickness) / 2, length + thickness, thickness};
					append(rect, {}, color, item.clip, 2, thickness / 2, 0, std::atan2(dy, dx));
					break;
				}
				case DrawCommandKind::Text:
					for (const auto *q: glyphs[item.commandIndex]) {
						auto ink = q->color;
						ink.a *= c.opacity;
						append(q->rect, q->uv, ink, ui::intersect(item.clip, q->clip), 1);
					}
					break;
				case DrawCommandKind::Image:
					if (ui::valid_rect(c.rect) && c.opacity > 0)
						frame.batches.push_back({item.clip, 0, 0, item.commandIndex});
					break;
				case DrawCommandKind::ScissorBegin:
				case DrawCommandKind::ScissorEnd:
					break;
			}
		}
		return frame;
	}
} // namespace codec_gui::gui
