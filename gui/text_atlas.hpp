#pragma once

#include "draw_list.hpp"
#include "text_shaper.hpp"

#include <cstdint>
#include <vector>
#include <memory>
#include <limits>

namespace codec_gui::gui {

	struct GlyphQuad {
		Rect rect;
		Rect uv;
		Color color;
		bool elision = false;
		int layer = 0;
		Rect clip{.x=-1000000.0f, .y=-1000000.0f, .w=2000000.0f, .h=2000000.0f};
		std::size_t commandIndex = std::numeric_limits<std::size_t>::max();
	};
	struct TextAtlas;
	struct TextLine { std::string text; float width = 0; };
	[[nodiscard]] std::vector<TextLine> wrap_text(TextShaper& shaper, std::string_view text, float physicalWidth);
	// One instance per GUI view. Glyph locations stay stable as labels/numbers change.
	class TextAtlasCache {
		struct Impl;
		std::unique_ptr<Impl> impl_;
	public:
		TextAtlasCache();
		~TextAtlasCache();
		void clear();
		[[nodiscard]] TextAtlas& atlas();
		[[nodiscard]] uint64_t rasterized_glyph_count() const;
		const TextAtlas& build(const std::vector<DrawCommand>& commands, TextShaper& defaultShaper,
							  int atlasWidth = 1024, float deviceScale = 1);
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
