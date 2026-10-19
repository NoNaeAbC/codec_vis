#include "test_check.hpp"
#include "draw_list.hpp"
#include "text_atlas.hpp"
#include "text_shaper.hpp"

#include <cassert>
#include <cmath>
#include <cstdint>
#include <filesystem>

using namespace codec_gui::gui;

int main() {
	const std::filesystem::path font = default_font_path();
	TextShaper shaper(font, 16.0f);
	DrawCommand text;
	text.kind = DrawCommandKind::Text;
	text.rect = Rect{.x=8, .y=8, .w=200, .h=24};
	text.color = Color{.r=1, .g=1, .b=1, .a=1};
	text.text = "codec_vis";
	TextAtlas const atlas = build_text_atlas({text}, shaper, 128);
	CODEC_VIS_CHECK(atlas.width == 128);
	CODEC_VIS_CHECK(atlas.height > 0);
	CODEC_VIS_CHECK(!atlas.alpha.empty());
	CODEC_VIS_CHECK(!atlas.quads.empty());
	bool anyAlpha = false;
	for (uint8_t const v : atlas.alpha) {
		anyAlpha = anyAlpha || v != 0;
	}
	CODEC_VIS_CHECK(anyAlpha);
	TextAtlasCache persistent;
	persistent.build({text},shaper,128);
	const auto initialRasterizations=persistent.rasterized_glyph_count();
	const auto initialUV=persistent.atlas().quads.front().uv;
	auto moved=text;moved.rect.x+=100;
	persistent.build({moved},shaper,128);
	CODEC_VIS_CHECK(persistent.rasterized_glyph_count()==initialRasterizations);
	CODEC_VIS_CHECK(persistent.atlas().quads.front().uv.x==initialUV.x);
	auto lines=wrap_text(shaper,"alpha beta gamma",50);
	CODEC_VIS_CHECK(lines.size()>=3);
	for(const auto& line:lines)CODEC_VIS_CHECK(line.width<=50);
	CODEC_VIS_CHECK(wrap_text(shaper,"one\n\nthree",200).size()==3);
	auto aligned=text;aligned.textAlign=TextAlign::Right;
	auto right=build_text_atlas({aligned},shaper,128);
	CODEC_VIS_CHECK(right.quads.front().rect.x>atlas.quads.front().rect.x+80);
	auto heading=text;heading.fontSize=32;heading.rect.h=48;
	auto large=build_text_atlas({heading},shaper,256);
	CODEC_VIS_CHECK(large.quads.front().rect.h>atlas.quads.front().rect.h);
	auto paragraph=text;paragraph.text="alpha beta gamma delta epsilon";paragraph.textOverflow=TextOverflow::Wrap;paragraph.rect={0,0,65,150};
	auto wrapped=build_text_atlas({paragraph},shaper,256);
	CODEC_VIS_CHECK(wrapped.quads.back().rect.y>wrapped.quads.front().rect.y+20);
	for(const auto& quad:wrapped.quads)CODEC_VIS_CHECK(quad.rect.x+quad.rect.w<=65.01f);

	DrawCommand longText = text;
	longText.rect = Rect{.x=10, .y=12, .w=48, .h=24};
	longText.text = "very/long/path/with/backend/name/and/error/details";
	TextAtlas const clipped = build_text_atlas({longText}, shaper, 128);
	CODEC_VIS_CHECK(!clipped.quads.empty());
	bool sawElision = false;
	for (const GlyphQuad& quad : clipped.quads) {
		CODEC_VIS_CHECK(quad.rect.x >= longText.rect.x);
		CODEC_VIS_CHECK(quad.rect.y >= longText.rect.y);
		CODEC_VIS_CHECK(quad.rect.x + quad.rect.w <= longText.rect.x + longText.rect.w + 0.01f);
		CODEC_VIS_CHECK(quad.rect.y + quad.rect.h <= longText.rect.y + longText.rect.h + 0.01f);
		sawElision = sawElision || quad.elision;
	}
	CODEC_VIS_CHECK(sawElision);

	DrawCommand clipBegin;
	clipBegin.kind = DrawCommandKind::ScissorBegin;
	clipBegin.rect = Rect{.x=0, .y=40, .w=300, .h=80};
	DrawCommand escaped = text;
	escaped.rect = Rect{.x=10, .y=10, .w=200, .h=24};
	escaped.text = "must not escape above inspector";
	DrawCommand visible = text;
	visible.rect = Rect{.x=10, .y=50, .w=200, .h=24};
	visible.text = "visible inside inspector";
	DrawCommand clipEnd;
	clipEnd.kind = DrawCommandKind::ScissorEnd;
	TextAtlas const scissored = build_text_atlas({clipBegin, escaped, visible, clipEnd}, shaper, 128);
	CODEC_VIS_CHECK(!scissored.quads.empty());
	for (const GlyphQuad& quad : scissored.quads) {
		CODEC_VIS_CHECK(quad.clip.x == clipBegin.rect.x);
		CODEC_VIS_CHECK(quad.clip.y == clipBegin.rect.y);
		CODEC_VIS_CHECK(quad.clip.w == clipBegin.rect.w);
		CODEC_VIS_CHECK(quad.clip.h == clipBegin.rect.h);
		CODEC_VIS_CHECK(quad.rect.y >= visible.rect.y);
	}

	constexpr float fractionalScale = 1.6f;
	TextShaper hidpiShaper(font, 16.0f * fractionalScale);
	TextAtlas const hidpi = build_text_atlas({text}, hidpiShaper, 256, fractionalScale);
	CODEC_VIS_CHECK(!hidpi.quads.empty());
	for (const GlyphQuad& quad : hidpi.quads) {
		CODEC_VIS_CHECK(std::fabs(quad.rect.x * fractionalScale - std::round(quad.rect.x * fractionalScale)) < 0.001f);
		CODEC_VIS_CHECK(std::fabs(quad.rect.y * fractionalScale - std::round(quad.rect.y * fractionalScale)) < 0.001f);
		CODEC_VIS_CHECK(std::fabs(quad.rect.w * fractionalScale - std::round(quad.rect.w * fractionalScale)) < 0.001f);
		CODEC_VIS_CHECK(std::fabs(quad.rect.h * fractionalScale - std::round(quad.rect.h * fractionalScale)) < 0.001f);
	}
	return 0;
}
