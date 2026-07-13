#include "text_atlas.hpp"

#include <cassert>
#include <cmath>
#include <filesystem>

using namespace codec_gui::gui;

int main() {
	const std::filesystem::path font = default_font_path();
	TextShaper shaper(font, 16.0f);
	DrawCommand text;
	text.kind = DrawCommandKind::Text;
	text.rect = Rect{8, 8, 200, 24};
	text.color = Color{1, 1, 1, 1};
	text.text = "codec_vis";
	TextAtlas atlas = build_text_atlas({text}, shaper, 128);
	assert(atlas.width == 128);
	assert(atlas.height > 0);
	assert(!atlas.alpha.empty());
	assert(!atlas.quads.empty());
	bool anyAlpha = false;
	for (uint8_t v : atlas.alpha) {
		anyAlpha = anyAlpha || v != 0;
	}
	assert(anyAlpha);

	DrawCommand longText = text;
	longText.rect = Rect{10, 12, 48, 24};
	longText.text = "very/long/path/with/backend/name/and/error/details";
	TextAtlas clipped = build_text_atlas({longText}, shaper, 128);
	assert(!clipped.quads.empty());
	bool sawElision = false;
	for (const GlyphQuad& quad : clipped.quads) {
		assert(quad.rect.x >= longText.rect.x);
		assert(quad.rect.y >= longText.rect.y);
		assert(quad.rect.x + quad.rect.w <= longText.rect.x + longText.rect.w + 0.01f);
		assert(quad.rect.y + quad.rect.h <= longText.rect.y + longText.rect.h + 0.01f);
		sawElision = sawElision || quad.elision;
	}
	assert(sawElision);

	DrawCommand clipBegin;
	clipBegin.kind = DrawCommandKind::ScissorBegin;
	clipBegin.rect = Rect{0, 40, 300, 80};
	DrawCommand escaped = text;
	escaped.rect = Rect{10, 10, 200, 24};
	escaped.text = "must not escape above inspector";
	DrawCommand visible = text;
	visible.rect = Rect{10, 50, 200, 24};
	visible.text = "visible inside inspector";
	DrawCommand clipEnd;
	clipEnd.kind = DrawCommandKind::ScissorEnd;
	TextAtlas scissored = build_text_atlas({clipBegin, escaped, visible, clipEnd}, shaper, 128);
	assert(!scissored.quads.empty());
	for (const GlyphQuad& quad : scissored.quads) {
		assert(quad.clip.x == clipBegin.rect.x);
		assert(quad.clip.y == clipBegin.rect.y);
		assert(quad.clip.w == clipBegin.rect.w);
		assert(quad.clip.h == clipBegin.rect.h);
		assert(quad.rect.y >= visible.rect.y);
	}

	constexpr float fractionalScale = 1.6f;
	TextShaper hidpiShaper(font, 16.0f * fractionalScale);
	TextAtlas hidpi = build_text_atlas({text}, hidpiShaper, 256, fractionalScale);
	assert(!hidpi.quads.empty());
	for (const GlyphQuad& quad : hidpi.quads) {
		assert(std::fabs(quad.rect.x * fractionalScale - std::round(quad.rect.x * fractionalScale)) < 0.001f);
		assert(std::fabs(quad.rect.y * fractionalScale - std::round(quad.rect.y * fractionalScale)) < 0.001f);
		assert(std::fabs(quad.rect.w * fractionalScale - std::round(quad.rect.w * fractionalScale)) < 0.001f);
		assert(std::fabs(quad.rect.h * fractionalScale - std::round(quad.rect.h * fractionalScale)) < 0.001f);
	}
	return 0;
}
