#pragma once

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace native_video::av1::vaapi {
	inline constexpr char             kRenderNodeEnvironmentVariable[] = "CODEC_VIS_VAAPI_RENDER_NODE";
	inline constexpr std::string_view kFallbackRenderNode              = "/dev/dri/renderD128";

	[[nodiscard]] inline bool looks_like_render_node(const std::filesystem::path &path) {
		const auto filename = path.filename().string();
		return filename.starts_with("renderD");
	}

	[[nodiscard]] inline std::optional<std::filesystem::path> find_render_node() {
		if (const char *override_path = std::getenv(kRenderNodeEnvironmentVariable);
			override_path != nullptr && override_path[0] != '\0') {
			return std::filesystem::path{override_path};
		}

		const std::filesystem::path fallback{kFallbackRenderNode};
		std::error_code             error;
		if (std::filesystem::exists(fallback, error)) { return fallback; }

		const std::filesystem::path dri_directory{"/dev/dri"};
		if (!std::filesystem::exists(dri_directory, error)) { return std::nullopt; }

		std::vector<std::filesystem::path> candidates;
		for (std::filesystem::directory_iterator it{dri_directory, error};
			 !error && it != std::filesystem::directory_iterator{}; it.increment(error)) {
			if (!looks_like_render_node(it->path())) { continue; }
			if (std::filesystem::exists(it->path(), error)) { candidates.push_back(it->path()); }
		}
		std::ranges::sort(candidates);
		if (candidates.empty()) { return std::nullopt; }
		return candidates.front();
	}

	[[nodiscard]] inline std::filesystem::path resolve_render_node_or_default() {
		auto render_node = find_render_node();
		if (render_node.has_value()) { return *render_node; }
		// Preserve the conventional render-node path so the device open reports its precise errno.
		// Discovery returning no candidate must not turn a concrete open failure into an empty path.
		return std::filesystem::path{kFallbackRenderNode};
	}
} // namespace native_video::av1::vaapi
