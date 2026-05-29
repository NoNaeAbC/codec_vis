#include "text_shaper.hpp"

#include <cassert>
#include <cstddef>
#include <filesystem>

using namespace codec_gui::gui;

int main() {
	const std::filesystem::path font = default_font_path();
	assert(std::filesystem::exists(font));
	TextShaper shaper(font, 16.0f);
	const ShapedText text = shaper.shape("codec_vis");
	assert(!text.glyphs.empty());
	assert(text.width > 0.0f);
	assert(text.height == 16.0f);
	const GlyphBitmap glyph = shaper.rasterize_glyph(text.glyphs.front().glyphIndex);
	assert(glyph.width > 0);
	assert(glyph.height > 0);
	assert(glyph.alpha.size() == static_cast<std::size_t>(glyph.width * glyph.height));
	const RasterizedText rasterized = shaper.rasterize("codec_vis");
	assert(rasterized.glyphs.size() == text.glyphs.size());
	assert(rasterized.width == text.width);
	assert(rasterized.glyphs.front().bitmap.width > 0);

	TextShaper substitute(std::filesystem::temp_directory_path() / "codec-vis-missing-font.ttf", 16.0f);
	const ShapedText substituteText = substitute.shape("substitute");
	assert(!substituteText.glyphs.empty());
	return 0;
}
