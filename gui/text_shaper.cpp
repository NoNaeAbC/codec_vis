#include "text_shaper.hpp"

#include <ft2build.h>
#include FT_FREETYPE_H
#include <fontconfig/fontconfig.h>
#include <hb-ft.h>
#include <hb.h>

#include <array>
#include <algorithm>
#include <filesystem>
#include <stdexcept>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

namespace codec_gui::gui {
namespace {

void checked_ft(FT_Error error, const char* operation) {
	if (error != 0) {
		throw std::runtime_error(operation);
	}
}

std::filesystem::path match_font_file(const char* patternName) {
	FcConfig* config = FcInitLoadConfigAndFonts();
	if (config == nullptr) {
		return {};
	}
	FcPattern* pattern = FcNameParse(reinterpret_cast<const FcChar8*>(patternName));
	if (pattern == nullptr) {
		FcConfigDestroy(config);
		return {};
	}
	FcConfigSubstitute(config, pattern, FcMatchPattern);
	FcDefaultSubstitute(pattern);
	FcResult result = FcResultNoMatch;
	FcPattern* match = FcFontMatch(config, pattern, &result);
	std::filesystem::path path;
	if (match != nullptr) {
		FcChar8* file = nullptr;
		if (FcPatternGetString(match, FC_FILE, 0, &file) == FcResultMatch && file != nullptr) {
			path = reinterpret_cast<const char*>(file);
		}
		FcPatternDestroy(match);
	}
	FcPatternDestroy(pattern);
	FcConfigDestroy(config);
	return path;
}

std::vector<std::filesystem::path> secondary_font_candidates() {
	static constexpr std::array<const char*, 5> Candidates{
		"sans",
		"Noto Sans",
		"Noto Sans Symbols",
		"DejaVu Sans",
		"FreeSans",
	};
	std::vector<std::filesystem::path> out;
	for (const char* candidate : Candidates) {
		const std::filesystem::path path = match_font_file(candidate);
		if (!path.empty() && std::filesystem::exists(path) && std::find(out.begin(), out.end(), path) == out.end()) {
			out.emplace_back(path);
		}
	}
	return out;
}

std::filesystem::path resolve_font_path(const std::filesystem::path& requested) {
	if (!requested.empty() && std::filesystem::exists(requested)) {
		return requested;
	}
	const std::vector<std::filesystem::path> candidates = secondary_font_candidates();
	if (!candidates.empty()) {
		return candidates.front();
	}
	throw std::runtime_error("no usable UI font found");
}

} // namespace

std::filesystem::path default_font_path() {
	return resolve_font_path({});
}

struct TextShaper::Impl {
	FT_Library ft = nullptr;
	std::vector<FT_Face> faces;
	std::vector<hb_font_t*> fonts;
	float pixelSize = 0.0f;
};

TextShaper::TextShaper(const std::filesystem::path& fontPath, float pixelSize) : impl_(new Impl) {
	impl_->pixelSize = pixelSize;
	checked_ft(FT_Init_FreeType(&impl_->ft), "FT_Init_FreeType failed");

	std::vector<std::filesystem::path> paths{resolve_font_path(fontPath)};
	for (const std::filesystem::path& candidate : secondary_font_candidates()) {
		if (std::find(paths.begin(), paths.end(), candidate) == paths.end()) {
			paths.push_back(candidate);
		}
	}
	for (const std::filesystem::path& path : paths) {
		FT_Face face = nullptr;
		if (FT_New_Face(impl_->ft, path.c_str(), 0, &face) != 0) {
			continue;
		}
		if (FT_Set_Pixel_Sizes(face, 0, static_cast<FT_UInt>(pixelSize)) != 0) {
			FT_Done_Face(face);
			continue;
		}
		hb_font_t* font = hb_ft_font_create_referenced(face);
		if (font == nullptr) {
			FT_Done_Face(face);
			continue;
		}
		impl_->faces.push_back(face);
		impl_->fonts.push_back(font);
	}
	if (impl_->fonts.empty()) {
		throw std::runtime_error("no usable UI font found");
	}
}

TextShaper::TextShaper(TextShaper&& other) noexcept : impl_(std::exchange(other.impl_, nullptr)) {}

TextShaper& TextShaper::operator=(TextShaper&& other) noexcept {
	if (this != &other) {
		this->~TextShaper();
		impl_ = std::exchange(other.impl_, nullptr);
	}
	return *this;
}

TextShaper::~TextShaper() {
	if (impl_ == nullptr) {
		return;
	}
	for (hb_font_t* font : impl_->fonts) {
		hb_font_destroy(font);
	}
	for (FT_Face face : impl_->faces) {
		FT_Done_Face(face);
	}
	if (impl_->ft != nullptr) {
		FT_Done_FreeType(impl_->ft);
	}
	delete impl_;
}

ShapedText TextShaper::shape(std::string_view utf8) const {
	if (impl_ == nullptr || impl_->fonts.empty()) {
		throw std::runtime_error("TextShaper is not initialized");
	}
	auto shape_with_font = [](hb_font_t* font, std::string_view text, uint32_t fontIndex) {
		ShapedText out;
		hb_buffer_t* buffer = hb_buffer_create();
		hb_buffer_add_utf8(buffer, text.data(), static_cast<int>(text.size()), 0, static_cast<int>(text.size()));
		hb_buffer_guess_segment_properties(buffer);
		hb_shape(font, buffer, nullptr, 0);

		unsigned int count = 0;
		const hb_glyph_info_t* infos = hb_buffer_get_glyph_infos(buffer, &count);
		const hb_glyph_position_t* positions = hb_buffer_get_glyph_positions(buffer, &count);
		out.glyphs.reserve(count);
		for (unsigned int i = 0; i < count; ++i) {
			ShapedGlyph glyph;
			glyph.glyphIndex = infos[i].codepoint;
			glyph.fontIndex = fontIndex;
			glyph.advanceX = static_cast<float>(positions[i].x_advance) / 64.0f;
			glyph.advanceY = static_cast<float>(positions[i].y_advance) / 64.0f;
			glyph.offsetX = static_cast<float>(positions[i].x_offset) / 64.0f;
			glyph.offsetY = static_cast<float>(positions[i].y_offset) / 64.0f;
			out.width += glyph.advanceX;
			out.glyphs.push_back(glyph);
		}
		hb_buffer_destroy(buffer);
		return out;
	};

	hb_buffer_t* buffer = hb_buffer_create();
	hb_buffer_add_utf8(buffer, utf8.data(), static_cast<int>(utf8.size()), 0, static_cast<int>(utf8.size()));
	hb_buffer_guess_segment_properties(buffer);
	hb_shape(impl_->fonts.front(), buffer, nullptr, 0);

	unsigned int count = 0;
	const hb_glyph_info_t* infos = hb_buffer_get_glyph_infos(buffer, &count);
	const hb_glyph_position_t* positions = hb_buffer_get_glyph_positions(buffer, &count);

	ShapedText out;
	out.glyphs.reserve(count);
	for (unsigned int i = 0; i < count; ++i) {
		bool usedSecondaryFont = false;
		if (infos[i].codepoint == 0 && impl_->fonts.size() > 1) {
			const unsigned int begin = infos[i].cluster;
			unsigned int end = static_cast<unsigned int>(utf8.size());
			for (unsigned int j = 0; j < count; ++j) {
				if (infos[j].cluster > begin) {
					end = std::min(end, infos[j].cluster);
				}
			}
			const std::string_view cluster = utf8.substr(begin, end - begin);
			for (uint32_t fontIndex = 1; fontIndex < impl_->fonts.size(); ++fontIndex) {
				ShapedText replacement = shape_with_font(impl_->fonts[fontIndex], cluster, fontIndex);
				const bool usable = std::any_of(replacement.glyphs.begin(), replacement.glyphs.end(), [](const ShapedGlyph& glyph) {
					return glyph.glyphIndex != 0;
				});
					if (usable) {
						out.width += replacement.width;
						out.glyphs.insert(out.glyphs.end(), replacement.glyphs.begin(), replacement.glyphs.end());
						usedSecondaryFont = true;
						break;
					}
				}
			}
		if (usedSecondaryFont) {
			continue;
		}
			ShapedGlyph glyph;
		glyph.glyphIndex = infos[i].codepoint;
		glyph.fontIndex = 0;
		glyph.advanceX = static_cast<float>(positions[i].x_advance) / 64.0f;
		glyph.advanceY = static_cast<float>(positions[i].y_advance) / 64.0f;
		glyph.offsetX = static_cast<float>(positions[i].x_offset) / 64.0f;
		glyph.offsetY = static_cast<float>(positions[i].y_offset) / 64.0f;
			out.width += glyph.advanceX;
			out.glyphs.push_back(glyph);
	}
	out.height = impl_->pixelSize;
	hb_buffer_destroy(buffer);
	return out;
}

GlyphBitmap TextShaper::rasterize_glyph(uint32_t glyphIndex) const {
	ShapedGlyph glyph;
	glyph.glyphIndex = glyphIndex;
	glyph.fontIndex = 0;
	return rasterize_glyph(glyph);
}

GlyphBitmap TextShaper::rasterize_glyph(const ShapedGlyph& glyph) const {
	if (impl_ == nullptr || glyph.fontIndex >= impl_->faces.size()) {
		throw std::runtime_error("TextShaper is not initialized");
	}
	FT_Face face = impl_->faces[glyph.fontIndex];
	checked_ft(FT_Load_Glyph(face, glyph.glyphIndex, FT_LOAD_DEFAULT), "FT_Load_Glyph failed");
	checked_ft(FT_Render_Glyph(face->glyph, FT_RENDER_MODE_NORMAL), "FT_Render_Glyph failed");
	const FT_GlyphSlot slot = face->glyph;
	GlyphBitmap out;
	out.glyphIndex = glyph.glyphIndex;
	out.fontIndex = glyph.fontIndex;
	out.width = static_cast<int>(slot->bitmap.width);
	out.height = static_cast<int>(slot->bitmap.rows);
	out.bearingX = slot->bitmap_left;
	out.bearingY = slot->bitmap_top;
	out.alpha.resize(static_cast<std::size_t>(out.width) * static_cast<std::size_t>(out.height));
	for (int y = 0; y < out.height; ++y) {
		const auto* src = slot->bitmap.buffer + static_cast<std::size_t>(y) * static_cast<std::size_t>(slot->bitmap.pitch);
		auto* dst = out.alpha.data() + static_cast<std::size_t>(y) * static_cast<std::size_t>(out.width);
		std::memcpy(dst, src, static_cast<std::size_t>(out.width));
	}
	return out;
}

RasterizedText TextShaper::rasterize(std::string_view utf8) const {
	ShapedText shaped = shape(utf8);
	RasterizedText out;
	out.width = shaped.width;
	out.height = shaped.height;
	out.glyphs.reserve(shaped.glyphs.size());
	float penX = 0.0f;
	float penY = 0.0f;
	for (const ShapedGlyph& glyph : shaped.glyphs) {
		RasterizedGlyph raster;
		raster.shape = glyph;
		raster.bitmap = rasterize_glyph(glyph);
		raster.penX = penX;
		raster.penY = penY;
		out.glyphs.push_back(std::move(raster));
		penX += glyph.advanceX;
		penY += glyph.advanceY;
	}
	return out;
}

} // namespace codec_gui::gui
