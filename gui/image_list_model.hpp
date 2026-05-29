#pragma once

#include "app_state.hpp"
#include "metrics.hpp"

#include <algorithm>
#include <cstdint>
#include <limits>
#include <string>
#include <vector>

namespace codec_gui::gui {

	[[nodiscard]] inline std::vector<const ImageObject*> ordered_images(const AppState& state) {
		std::vector<const ImageObject*> out;
		out.reserve(state.images.size());
		for (const ImageObject& image : state.images) {
			if (image.type == ImageObjectType::Derived) {
				continue;
			}
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
		const auto metric = [](const ImageObject* image) {
			if (!image->encoded) {
				return -1.0;
			}
			const QualityMetricRecord* record = primary_metric(*image->encoded);
			return record != nullptr && record->value ? *record->value : -1.0;
		};

		std::stable_sort(out.begin(), out.end(), [&](const ImageObject* a, const ImageObject* b) {
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
				case ImageSortKey::Metric:
					cmp = metric(a) < metric(b) ? -1 : (metric(a) > metric(b) ? 1 : 0);
					break;
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
