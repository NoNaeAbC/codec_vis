#include "text_atlas.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <unordered_map>

namespace codec_gui::gui {
namespace {

struct PackedGlyph {
	int x = 0;
	int y = 0;
	int w = 0;
	int h = 0;
	int bearingX = 0;
	int bearingY = 0;
};

struct GlyphCacheValue {
	GlyphBitmap bitmap;
	PackedGlyph packed;
};

uint64_t glyph_key(const ShapedGlyph& glyph) {
	return (static_cast<uint64_t>(glyph.fontIndex) << 32u) | glyph.glyphIndex;
}

void cache_rasterized_glyphs(std::unordered_map<uint64_t, GlyphCacheValue>& glyphs, const RasterizedText& rasterized) {
	for (const RasterizedGlyph& glyph : rasterized.glyphs) {
		if (glyph.bitmap.width <= 0 || glyph.bitmap.height <= 0) {
			continue;
		}
		glyphs.try_emplace(glyph_key(glyph.shape), GlyphCacheValue{glyph.bitmap, {}});
	}
}

void append_glyph_quad(
	TextAtlas& atlas,
	std::vector<GlyphQuad>& quads,
	const std::unordered_map<uint64_t, GlyphCacheValue>& glyphs,
	const DrawCommand& command,
	const RasterizedText& rasterized,
	const RasterizedGlyph& glyph,
	float xOffset,
	bool elision
) {
	const auto it = glyphs.find(glyph_key(glyph.shape));
	if (it == glyphs.end()) {
		return;
	}
	const PackedGlyph& packed = it->second.packed;
	const float x = command.rect.x + xOffset + glyph.penX + glyph.shape.offsetX + static_cast<float>(packed.bearingX);
	const float y = command.rect.y + glyph.shape.offsetY + rasterized.height - static_cast<float>(packed.bearingY);
	Rect rect{x, y, static_cast<float>(packed.w), static_cast<float>(packed.h)};
	Rect uv{
		static_cast<float>(packed.x) / static_cast<float>(atlas.width),
		static_cast<float>(packed.y) / static_cast<float>(atlas.height),
		static_cast<float>(packed.w) / static_cast<float>(atlas.width),
		static_cast<float>(packed.h) / static_cast<float>(atlas.height),
	};
	if (rect.x < command.rect.x) {
		const float trim = command.rect.x - rect.x;
		rect.x = command.rect.x;
		rect.w = std::max(0.0f, rect.w - trim);
		uv.x += trim / static_cast<float>(atlas.width);
		uv.w = rect.w / static_cast<float>(atlas.width);
	}
	if (rect.x + rect.w > command.rect.x + command.rect.w) {
		rect.w = std::max(0.0f, command.rect.x + command.rect.w - rect.x);
		uv.w = rect.w / static_cast<float>(atlas.width);
	}
	if (rect.y < command.rect.y) {
		const float trim = command.rect.y - rect.y;
		rect.y = command.rect.y;
		rect.h = std::max(0.0f, rect.h - trim);
		uv.y += trim / static_cast<float>(atlas.height);
		uv.h = rect.h / static_cast<float>(atlas.height);
	}
	if (rect.y + rect.h > command.rect.y + command.rect.h) {
		rect.h = std::max(0.0f, command.rect.y + command.rect.h - rect.y);
		uv.h = rect.h / static_cast<float>(atlas.height);
	}
	if (rect.w > 0.0f && rect.h > 0.0f) {
		quads.push_back({rect, uv, command.color, elision});
	}
}

} // namespace

TextAtlas build_text_atlas(const std::vector<DrawCommand>& commands, TextShaper& shaper, int atlasWidth) {
	atlasWidth = std::max(64, atlasWidth);
	std::unordered_map<uint64_t, GlyphCacheValue> glyphs;
	std::vector<const DrawCommand*> textCommands;
	const RasterizedText ellipsis = shaper.rasterize("...");
	for (const DrawCommand& command : commands) {
		if (command.kind != DrawCommandKind::Text || command.text.empty() || command.rect.w <= 0.0f || command.rect.h <= 0.0f) {
			continue;
		}
		textCommands.push_back(&command);
		RasterizedText rasterized = shaper.rasterize(command.text);
		cache_rasterized_glyphs(glyphs, rasterized);
	}
	cache_rasterized_glyphs(glyphs, ellipsis);

	const int padding = 1;
	int cursorX = padding;
	int cursorY = padding;
	int rowH = 0;
	for (auto& [_, value] : glyphs) {
		const int w = value.bitmap.width + padding * 2;
		const int h = value.bitmap.height + padding * 2;
		if (cursorX + w > atlasWidth) {
			cursorX = padding;
			cursorY += rowH + padding;
			rowH = 0;
		}
		value.packed.x = cursorX + padding;
		value.packed.y = cursorY + padding;
		value.packed.w = value.bitmap.width;
		value.packed.h = value.bitmap.height;
		value.packed.bearingX = value.bitmap.bearingX;
		value.packed.bearingY = value.bitmap.bearingY;
		cursorX += w;
		rowH = std::max(rowH, h);
	}

	TextAtlas atlas;
	atlas.width = atlasWidth;
	atlas.height = std::max(1, cursorY + rowH + padding);
	atlas.alpha.assign(static_cast<std::size_t>(atlas.width) * static_cast<std::size_t>(atlas.height), 0);
	for (const auto& [_, value] : glyphs) {
		for (int y = 0; y < value.bitmap.height; ++y) {
			const auto srcOff = static_cast<std::size_t>(y) * static_cast<std::size_t>(value.bitmap.width);
			const auto dstOff = static_cast<std::size_t>(value.packed.y + y) * static_cast<std::size_t>(atlas.width) + static_cast<std::size_t>(value.packed.x);
			std::copy_n(value.bitmap.alpha.data() + srcOff, static_cast<std::size_t>(value.bitmap.width), atlas.alpha.data() + dstOff);
		}
	}

	for (const DrawCommand* command : textCommands) {
		RasterizedText rasterized = shaper.rasterize(command->text);
		const bool shouldElide = rasterized.width > command->rect.w && !ellipsis.glyphs.empty();
		const float textLimit = shouldElide ? std::max(0.0f, command->rect.w - ellipsis.width) : command->rect.w;
		float visibleAdvance = 0.0f;
		for (const RasterizedGlyph& glyph : rasterized.glyphs) {
			const float glyphEnd = glyph.penX + std::max(0.0f, glyph.shape.advanceX);
			if (shouldElide && glyphEnd > textLimit) {
				break;
			}
			append_glyph_quad(atlas, atlas.quads, glyphs, *command, rasterized, glyph, 0.0f, false);
			visibleAdvance = std::max(visibleAdvance, glyphEnd);
		}
		if (shouldElide) {
			const float ellipsisOffset = std::min(visibleAdvance, std::max(0.0f, command->rect.w - ellipsis.width));
			for (const RasterizedGlyph& glyph : ellipsis.glyphs) {
				append_glyph_quad(atlas, atlas.quads, glyphs, *command, ellipsis, glyph, ellipsisOffset, true);
			}
		}
	}
	return atlas;
}

} // namespace codec_gui::gui
