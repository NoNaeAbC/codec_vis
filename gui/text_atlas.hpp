#pragma once

#include "draw_list.hpp"
#include "text_shaper.hpp"

#include <cstdint>
#include <vector>

namespace codec_gui::gui {

	struct GlyphQuad {
		Rect rect;
		Rect uv;
		Color color;
		bool elision = false;
		int layer = 0;
		Rect clip{-1000000.0f, -1000000.0f, 2000000.0f, 2000000.0f};
	};

	struct TextAtlas {
		int width = 0;
		int height = 0;
		std::vector<uint8_t> alpha;
		std::vector<GlyphQuad> quads;
	};

	[[nodiscard]] TextAtlas build_text_atlas(
		const std::vector<DrawCommand>& commands,
		TextShaper& shaper,
		int atlasWidth = 1024,
		float deviceScale = 1.0f
	);

} // namespace codec_gui::gui
