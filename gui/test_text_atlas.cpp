#include "text_atlas.hpp"

#include <cassert>
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
	return 0;
}
