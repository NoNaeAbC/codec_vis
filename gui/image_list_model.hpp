#pragma once

#include "app_state.hpp"
#include "metrics.hpp"

#include <algorithm>
#include <cstdint>
#include <cmath>
#include <limits>
#include <string>
#include <vector>

namespace codec_gui::gui {
	[[nodiscard]] inline std::string_view comparison_metric_id(const AppState& state) {
		for(const auto& descriptor:metric_catalog()) if(descriptor.id==state.imageList.metricId) return descriptor.id;
		return metric_catalog().empty()?std::string_view{}:metric_catalog().front().id;
	}

	inline constexpr float ImageListHeaderHeight = 52.0f;
	inline constexpr float ImageListRowHeight = 68.0f;
	inline constexpr float ImageListRowStep = 72.0f;
	inline constexpr float ImageListPaneButtonWidth = 38.0f;
	inline constexpr float ImageListPaneButtonHeight = 18.0f;
	inline constexpr float ImageListActionGap = 4.0f;

	[[nodiscard]] inline Rect image_list_row_rect(Rect list, float y) {
		return Rect{.x=list.x + 6.0f, .y=y - 2.0f, .w=list.w - 12.0f, .h=ImageListRowHeight};
	}

	[[nodiscard]] inline Rect image_list_pane_button_rect(Rect row, std::size_t index) {
		const float x = row.x + row.w - (ImageListPaneButtonWidth + ImageListActionGap) * static_cast<float>(index + 1u);
		return Rect{.x=x, .y=row.y + 46.0f, .w=ImageListPaneButtonWidth, .h=ImageListPaneButtonHeight};
	}

	[[nodiscard]] inline Rect image_list_difference_button_rect(Rect row, std::size_t paneButtonCount) {
		constexpr float width = 46.0f;
		const float x = row.x + row.w - (ImageListPaneButtonWidth + ImageListActionGap) * static_cast<float>(paneButtonCount) - width - ImageListActionGap;
		return Rect{.x=x, .y=row.y + 46.0f, .w=width, .h=ImageListPaneButtonHeight};
	}

	[[nodiscard]] inline std::vector<const ImageObject*> ordered_images(const AppState& state) {
		std::vector<const ImageObject*> out;
		out.reserve(state.images.size());
		for (const ImageObject& image : state.images) {
			out.push_back(&image);
		}

		const auto source_rank = [](const ImageObject* image) {
			return image->type == ImageObjectType::Source ? 0 : 1;
		};
		const auto encoded_size = [](const ImageObject* image) {
			return image->encoded ? image->encoded->byteSize : std::numeric_limits<uint64_t>::max();
		};
		const auto backend = [](const ImageObject* image) -> std::string {
			return image->encoded ? image->encoded->backendName : std::string{};
		};
		const auto metric = [&](const ImageObject* image) -> const QualityMetricRecord* {
			if (!image->encoded) {
				return nullptr;
			}
			const QualityMetricRecord* record = metric_by_id(*image->encoded, comparison_metric_id(state));
			return record != nullptr && record->value && !std::isnan(*record->value) ? record : nullptr;
		};

		std::ranges::stable_sort(out, [&](const ImageObject* a, const ImageObject* b) {
			if (source_rank(a) != source_rank(b)) {
				return source_rank(a) < source_rank(b);
			}
			if (a->type == ImageObjectType::Source || b->type == ImageObjectType::Source) {
				return a->id.value < b->id.value;
			}
			int cmp = 0;
			switch (state.imageList.sortKey) {
				case ImageSortKey::CreationTime:
					cmp = a->id.value < b->id.value ? -1 : (a->id.value > b->id.value ? 1 : 0);
					break;
				case ImageSortKey::EncodedSize:
					cmp = encoded_size(a) < encoded_size(b) ? -1 : (encoded_size(a) > encoded_size(b) ? 1 : 0);
					break;
				case ImageSortKey::Backend:
					cmp = backend(a) < backend(b) ? -1 : (backend(a) > backend(b) ? 1 : 0);
					break;
				case ImageSortKey::Metric: {
					const QualityMetricRecord* am = metric(a);
					const QualityMetricRecord* bm = metric(b);
					// Missing values stay last in either direction; never compare a
					// fallback PSNR/SSIM value against an MS-SSIM value.
					if ((am == nullptr) != (bm == nullptr)) return am != nullptr;
					if (am != nullptr && bm != nullptr) cmp = *am->value < *bm->value ? -1 : (*am->value > *bm->value ? 1 : 0);
					break;
				}
			}
			if (cmp == 0) {
				cmp = a->id.value < b->id.value ? -1 : (a->id.value > b->id.value ? 1 : 0);
			}
			return state.imageList.ascending ? cmp < 0 : cmp > 0;
		});
		return out;
	}

	[[nodiscard]] inline ImageSortKey next_sort_key(ImageSortKey key) {
		switch (key) {
			case ImageSortKey::CreationTime:
				return ImageSortKey::EncodedSize;
			case ImageSortKey::EncodedSize:
				return ImageSortKey::Backend;
			case ImageSortKey::Backend:
				return ImageSortKey::Metric;
			case ImageSortKey::Metric:
				return ImageSortKey::CreationTime;
		}
		return ImageSortKey::CreationTime;
	}

	[[nodiscard]] inline const char* sort_key_label(ImageSortKey key) {
		switch (key) {
			case ImageSortKey::CreationTime:
				return "creation";
			case ImageSortKey::EncodedSize:
				return "size";
			case ImageSortKey::Backend:
				return "backend";
			case ImageSortKey::Metric:
				return "metric";
		}
		return "creation";
	}

} // namespace codec_gui::gui
