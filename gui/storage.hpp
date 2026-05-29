#pragma once

#include "app_state.hpp"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <string>

namespace codec_gui::gui {

	[[nodiscard]] inline std::string encoded_extension(const ImageObject& image) {
		if (!image.encoded) {
			return ".bin";
		}
		std::string codec = image.encoded->codecName.empty() ? image.encoded->backendName : image.encoded->codecName;
		std::transform(codec.begin(), codec.end(), codec.begin(), [](unsigned char c) {
			return static_cast<char>(std::tolower(c));
		});
		if (codec.find("av1") != std::string::npos || codec.find("av2") != std::string::npos) {
			return ".ivf";
		}
		if (codec.find("hevc") != std::string::npos || codec.find("h265") != std::string::npos || codec.find("x265") != std::string::npos) {
			return ".h265";
		}
		if (codec.find("vvc") != std::string::npos || codec.find("h266") != std::string::npos || codec.find("vvenc") != std::string::npos) {
			return ".h266";
		}
		return ".bin";
	}

	[[nodiscard]] inline std::string sanitize_filename(std::string name) {
		for (char& c : name) {
			const unsigned char u = static_cast<unsigned char>(c);
			if (!(std::isalnum(u) || c == '-' || c == '_' || c == '.')) {
				c = '_';
			}
		}
		if (name.empty() || name == "." || name == "..") {
			return "codec_vis";
		}
		return name;
	}

	[[nodiscard]] inline std::filesystem::path default_export_path(const AppState& state, const ImageObject& image) {
		std::filesystem::path directory = state.storage.lastExportDirectory;
		if (directory.empty()) {
			directory = std::filesystem::current_path();
		}
		std::string sourceStem;
		if (!image.parents.empty()) {
			const ImageId sourceId = image.parents.front();
			const auto source = std::find_if(state.images.begin(), state.images.end(), [sourceId](const ImageObject& candidate) {
				return candidate.id == sourceId;
			});
			if (source != state.images.end()) {
				sourceStem = std::filesystem::path(source->displayName).stem().string();
			}
		}
		if (sourceStem.empty()) {
			sourceStem = std::filesystem::path(image.displayName.empty() ? "codec_vis" : image.displayName).stem().string();
		}
		std::string stem = sanitize_filename(sourceStem);
		if (image.encoded) {
			if (!image.encoded->backendName.empty()) {
				stem += "_" + sanitize_filename(image.encoded->backendName);
			} else if (valid(image.encoded->backend)) {
				stem += "_backend_" + std::to_string(image.encoded->backend.value);
			}
			for (const ParamSummary& param : image.encoded->keyParams) {
				if (!param.name.empty() && !param.value.empty()) {
					stem += "_" + sanitize_filename(param.name) + "-" + sanitize_filename(param.value);
				}
			}
		}
		stem += "_" + std::to_string(image.id.value);
		return directory / (stem + encoded_extension(image));
	}

} // namespace codec_gui::gui
