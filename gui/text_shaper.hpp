#pragma once

#include <cstdint>
#include <filesystem>
#include <string_view>
#include <vector>

namespace codec_gui::gui {

	[[nodiscard]] std::filesystem::path default_font_path();

	struct ShapedGlyph {
		uint32_t glyphIndex = 0;
		uint32_t fontIndex = 0;
		float advanceX = 0.0f;
		float advanceY = 0.0f;
		float offsetX = 0.0f;
		float offsetY = 0.0f;
	};

	struct GlyphBitmap {
		uint32_t glyphIndex = 0;
		uint32_t fontIndex = 0;
		int width = 0;
		int height = 0;
		int bearingX = 0;
		int bearingY = 0;
		std::vector<uint8_t> alpha;
	};

	struct RasterizedGlyph {
		ShapedGlyph shape;
		GlyphBitmap bitmap;
		float penX = 0.0f;
		float penY = 0.0f;
	};

	struct ShapedText {
		std::vector<ShapedGlyph> glyphs;
		float width = 0.0f;
		float height = 0.0f;
	};

	struct RasterizedText {
		std::vector<RasterizedGlyph> glyphs;
		float width = 0.0f;
		float height = 0.0f;
	};

	class TextShaper {
	public:
		TextShaper(const std::filesystem::path& fontPath, float pixelSize);
		TextShaper(const TextShaper&) = delete;
		TextShaper& operator=(const TextShaper&) = delete;
		TextShaper(TextShaper&& other) noexcept;
		TextShaper& operator=(TextShaper&& other) noexcept;
		~TextShaper();

		[[nodiscard]] ShapedText shape(std::string_view utf8) const;
		[[nodiscard]] GlyphBitmap rasterize_glyph(uint32_t glyphIndex) const;
		[[nodiscard]] GlyphBitmap rasterize_glyph(const ShapedGlyph& glyph) const;
		[[nodiscard]] RasterizedText rasterize(std::string_view utf8) const;

	private:
		struct Impl;
		Impl* impl_ = nullptr;
	};

} // namespace codec_gui::gui
