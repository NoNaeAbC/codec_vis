#pragma once

#include "app_state.hpp"
#include "layout.hpp"

#include <algorithm>
#include <string>
#include <string_view>
#include <vector>

namespace codec_gui::gui {

	enum class WidgetKind {
		Button,
		Toggle,
		SliderInt,
		SliderFloat,
		NumericInput,
		SelectionPane,
		TextInput,
		TableList,
		Splitter,
		PaneView,
		ProgressRow,
		InlineError,
	};

	struct WidgetInfo {
		std::string id;
		WidgetKind kind = WidgetKind::Button;
		Rect rect;
		bool enabled = true;
		bool hovered = false;
		bool active = false;
		bool focused = false;
	};

	[[nodiscard]] inline ImageId selected_encode_source(const AppState& state) {
		const ImageObject* image = find_image(state, state.selection.selectedImage);
		// A result resolves to its own source. Derived comparisons have multiple
		// parents and must not silently choose one of them for encoding.
		for (std::size_t depth = 0; image != nullptr && depth <= state.images.size(); ++depth) {
			if (image->type == ImageObjectType::Source) return image->decoded ? image->id : ImageId{};
			if (image->type != ImageObjectType::EncodedResult || image->parents.size() != 1) return {};
			image = find_image(state, image->parents.front());
		}
		return {};
	}

	[[nodiscard]] inline bool command_is_enabled(const AppState& state, std::string_view name) {
		if (name == "import") return !state.interaction.importPending;
		if (name == "metrics") {
			const auto* image=find_image(state,state.selection.selectedImage);
			if(!image || !image->decoded || !image->encoded || image->parents.empty() || image->decoder.running) return false;
			const auto* source=find_image(state,image->parents.front());
			return source && source->decoded && !source->decoder.running && (image->encoded->metricState==MetricState::NotRequested || image->encoded->metricState==MetricState::Failed);
		}
		if (name == "encode") {
			const bool hasSource = valid(selected_encode_source(state));
			const auto selected = std::ranges::find_if(state.backends, [&](const BackendInfo& backend) {
				return backend.id == state.selection.selectedBackend;
			});
			const BackendInfo* backend = selected != state.backends.end()
				? &*selected
				: (state.backends.empty() ? nullptr : &state.backends.front());
			return hasSource && backend != nullptr && backend->capabilities.available;
		}
		if (name == "cancel") {
			return std::ranges::any_of(state.encodeRuns, [&](const EncodeRun& run) {
				return run.id == state.selection.selectedRun && (run.state == EncodeRunState::Queued || run.state == EncodeRunState::Running);
			});
		}
		if (name == "save") {
			const ImageObject* image = find_image(state, state.selection.selectedImage);
			return image != nullptr && image->encoded.has_value();
		}
		if (name == "delete") return valid(state.selection.selectedImage) && find_image(state, state.selection.selectedImage) != nullptr;
		if (name == "fit" || name == "100") {
			const Pane* pane = find_pane(state, state.selection.activePane);
			return pane != nullptr && pane->image.has_value();
		}
		if (name == "single" || name == "side" || name == "split" || name == "blink" || name == "diff" || name == "grid") {
			return !state.images.empty();
		}
		return true;
	}
	[[nodiscard]] std::vector<WidgetInfo> collect_widgets(const AppState& state, const LayoutResult& layout);

} // namespace codec_gui::gui
