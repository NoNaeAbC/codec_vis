#pragma once

#include "app_state.hpp"
#include "layout.hpp"

#include <span>
#include <string>
#include <vector>

namespace codec_gui::gui {

	enum class DrawCommandKind {
		Rect,
		Border,
		Text,
		Image,
		ScissorBegin,
		ScissorEnd,
		RoundedRect,
		Circle,
		Line,
	};
	enum class TextAlign { Left, Center, Right };
	enum class TextOverflow { Ellipsis, Clip, Wrap };
	enum class FontRole { Interface, Monospace };

	struct TextureBinding {
		ImageId image;
		TextureId texture;
	};

	struct ResourceSnapshot {
		std::vector<TextureBinding> textures;
	};

	struct DrawCommand {
		DrawCommandKind kind = DrawCommandKind::Rect;
		Rect rect;
		Color color;
		std::string text;
		ImageId image;
		ImageId comparisonImage; // Optional on-GPU max absolute linear RGB difference.
		float differenceGain=1;
		TextureId texture;
		Rect uvRect{.x=0, .y=0, .w=1, .h=1};
		float opacity = 1.0f;
		int layer = 0;
		Point endPoint; // Line starts at rect.x/y; other shapes use rect.
		float radius = 0;
		float strokeWidth = 0; // zero fills a shape; Line defaults to one pixel.
		float fontSize = 0; // zero uses the caller's default TextShaper.
		float lineHeight = 0;
		TextAlign textAlign = TextAlign::Left;
		TextOverflow textOverflow = TextOverflow::Ellipsis;
		FontRole fontRole = FontRole::Interface;
	};

	[[nodiscard]] TextureId resolve_texture(const ResourceSnapshot& resources, ImageId image);
	[[nodiscard]] std::vector<DrawCommand> build_draw_list(
		const AppState& state,
		const LayoutResult& layout,
		const ResourceSnapshot& resources,
		double timeSeconds
	);

} // namespace codec_gui::gui
