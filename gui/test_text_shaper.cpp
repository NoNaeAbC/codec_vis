#include "test_check.hpp"
#include "text_shaper.hpp"

#include <cassert>
#include <cstddef>
#include <filesystem>

using namespace codec_gui::gui;

int main() {
	const std::filesystem::path font = default_font_path();
	CODEC_VIS_CHECK(font == ":embedded-sans:");
	TextShaper const shaper(font, 16.0f);
	const ShapedText text = shaper.shape("codec_vis");
	CODEC_VIS_CHECK(!text.glyphs.empty());
	CODEC_VIS_CHECK(text.width > 0.0f);
	CODEC_VIS_CHECK(text.height == 16.0f);
	const GlyphBitmap glyph = shaper.rasterize_glyph(text.glyphs.front().glyphIndex);
	CODEC_VIS_CHECK(glyph.width > 0);
	CODEC_VIS_CHECK(glyph.height > 0);
	CODEC_VIS_CHECK(glyph.alpha.size() == static_cast<std::size_t>(glyph.width * glyph.height));
	const RasterizedText rasterized = shaper.rasterize("codec_vis");
	CODEC_VIS_CHECK(rasterized.glyphs.size() == text.glyphs.size());
	CODEC_VIS_CHECK(rasterized.width == text.width);
	CODEC_VIS_CHECK(rasterized.glyphs.front().bitmap.width > 0);

	TextShaper const substitute(std::filesystem::temp_directory_path() / "codec-vis-missing-font.ttf", 16.0f);
	const ShapedText substituteText = substitute.shape("substitute");
	CODEC_VIS_CHECK(!substituteText.glyphs.empty());
	return 0;
}
