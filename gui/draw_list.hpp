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
	};

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
		TextureId texture;
		Rect uvRect{0, 0, 1, 1};
		float opacity = 1.0f;
	};

	[[nodiscard]] TextureId resolve_texture(const ResourceSnapshot& resources, ImageId image);
	[[nodiscard]] std::vector<DrawCommand> build_draw_list(
		const AppState& state,
		const LayoutResult& layout,
		const ResourceSnapshot& resources,
		double timeSeconds
	);

} // namespace codec_gui::gui
