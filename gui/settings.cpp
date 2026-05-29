#include "settings.hpp"

#include <algorithm>
#include <charconv>
#include <cstdint>
#include <cstdlib>
#include <iterator>
#include <sstream>
#include <string>
#include <string_view>
#include <type_traits>
#include <unordered_map>
#include <vector>

namespace codec_gui::gui {
namespace {

std::string escape(std::string_view value) {
	std::string out;
	for (unsigned char c : value) {
		if (c == '%' || c == '\t' || c == '\n' || c == '\r') {
			const char* hex = "0123456789ABCDEF";
			out.push_back('%');
			out.push_back(hex[c >> 4]);
			out.push_back(hex[c & 0x0f]);
		} else {
			out.push_back(static_cast<char>(c));
		}
	}
	return out;
}

std::string unescape(std::string_view value) {
	std::string out;
	out.reserve(value.size());
	for (std::size_t i = 0; i < value.size(); ++i) {
		if (value[i] == '%' && i + 2 < value.size()) {
			char hex[3] = {static_cast<char>(value[i + 1]), static_cast<char>(value[i + 2]), 0};
			char* end = nullptr;
			const long parsed = std::strtol(hex, &end, 16);
			if (end != nullptr && *end == '\0') {
				out.push_back(static_cast<char>(parsed));
				i += 2;
				continue;
			}
		}
		out.push_back(value[i]);
	}
	return out;
}

std::string param_value_type(const ParamValue& value) {
	return std::visit([](const auto& v) -> std::string {
		using T = std::decay_t<decltype(v)>;
		if constexpr (std::is_same_v<T, bool>) return "bool";
		if constexpr (std::is_same_v<T, int64_t>) return "int";
		if constexpr (std::is_same_v<T, double>) return "float";
		return "string";
	}, value);
}

std::string param_value_text(const ParamValue& value) {
	return std::visit([](const auto& v) -> std::string {
		using T = std::decay_t<decltype(v)>;
		if constexpr (std::is_same_v<T, bool>) {
			return v ? "1" : "0";
		} else if constexpr (std::is_same_v<T, int64_t>) {
			return std::to_string(v);
		} else if constexpr (std::is_same_v<T, double>) {
			std::ostringstream oss;
			oss.precision(17);
			oss << v;
			return oss.str();
		} else {
			return v;
		}
	}, value);
}

ParamValue parse_param_value(std::string_view type, std::string_view text) {
	if (type == "bool") {
		return text == "1" || text == "true";
	}
	if (type == "int") {
		int64_t value = 0;
		std::from_chars(text.data(), text.data() + text.size(), value);
		return value;
	}
	if (type == "float") {
		return std::strtod(std::string(text).c_str(), nullptr);
	}
	return unescape(text);
}

ViewModeKind parse_mode(std::string_view text) {
	if (text == "side") return ViewModeKind::SideBySide;
	if (text == "split") return ViewModeKind::Split;
	if (text == "blink") return ViewModeKind::Blink;
	if (text == "difference") return ViewModeKind::Difference;
	if (text == "grid") return ViewModeKind::Grid;
	return ViewModeKind::Single;
}

std::string mode_text(ViewModeKind mode) {
	switch (mode) {
		case ViewModeKind::Single: return "single";
		case ViewModeKind::SideBySide: return "side";
		case ViewModeKind::Split: return "split";
		case ViewModeKind::Blink: return "blink";
		case ViewModeKind::Difference: return "difference";
		case ViewModeKind::Grid: return "grid";
	}
	return "single";
}

std::vector<std::string_view> split_tabs(std::string_view line) {
	std::vector<std::string_view> fields;
	std::size_t begin = 0;
	while (begin <= line.size()) {
		const std::size_t end = line.find('\t', begin);
		if (end == std::string_view::npos) {
			fields.push_back(line.substr(begin));
			break;
		}
		fields.push_back(line.substr(begin, end - begin));
		begin = end + 1;
	}
	return fields;
}

uint64_t parse_u64(std::string_view text) {
	uint64_t value = 0;
	std::from_chars(text.data(), text.data() + text.size(), value);
	return value;
}

double parse_double(std::string_view text, double defaultValue = 0.0) {
	if (text.empty()) {
		return defaultValue;
	}
	return std::strtod(std::string(text).c_str(), nullptr);
}

bool pane_exists(const AppState& state, PaneId id) {
	return std::any_of(state.panes.begin(), state.panes.end(), [id](const Pane& pane) {
		return pane.id == id;
	});
}

PaneId allocate_pane_id(AppState& state) {
	while (pane_exists(state, PaneId{state.nextId})) {
		++state.nextId;
	}
	return PaneId{state.nextId++};
}

} // namespace

std::string serialize_settings(const AppState& state) {
	std::ostringstream out;
	out << "codec_vis_settings\t1\n";
	out << "path\timport\t" << escape(state.storage.lastImportDirectory.string()) << '\n';
	out << "path\texport\t" << escape(state.storage.lastExportDirectory.string()) << '\n';
	out << "layout\t" << state.layout.imageListWidth << '\t' << state.layout.inspectorWidth << '\t'
	    << (state.layout.imageListCollapsed ? 1 : 0) << '\t' << (state.layout.inspectorCollapsed ? 1 : 0) << '\n';
	out << "selection\tbackend\t" << state.selection.selectedBackend.value << '\n';
	out << "mode\t" << mode_text(state.viewMode.kind) << '\t' << state.viewMode.splitPosition << '\t'
	    << state.viewMode.blinkIntervalSeconds << '\t' << state.viewMode.differenceGain << '\n';
	for (const Pane& pane : state.panes) {
		out << "pane\t" << pane.id.value << '\t' << (pane.image ? pane.image->value : 0) << '\t'
		    << pane.transform.scale << '\t' << pane.transform.centerX << '\t' << pane.transform.centerY << '\t'
		    << (pane.linkGroup ? *pane.linkGroup : 0) << '\n';
	}
	for (PaneId pane : state.viewMode.paneOrder) {
		out << "pane_order\t" << pane.value << '\n';
	}
	for (const EncoderConfig& config : state.encoderConfigs) {
		for (const EncoderParam& param : config.params) {
			out << "param\t" << config.backend.value << '\t' << escape(param.name) << '\t'
			    << param_value_type(param.value) << '\t' << escape(param_value_text(param.value)) << '\n';
		}
	}
	return out.str();
}

AppState apply_serialized_settings(AppState state, std::string_view text) {
	std::vector<PaneId> paneOrder;
	std::unordered_map<uint64_t, PaneId> paneIdRemap;
	std::size_t begin = 0;
	while (begin < text.size()) {
		const std::size_t end = text.find('\n', begin);
		const std::string_view line = end == std::string_view::npos ? text.substr(begin) : text.substr(begin, end - begin);
		begin = end == std::string_view::npos ? text.size() : end + 1;
		const std::vector<std::string_view> fields = split_tabs(line);
		if (fields.empty()) {
			continue;
		}
		if (fields[0] == "path" && fields.size() >= 3) {
			if (fields[1] == "import") state.storage.lastImportDirectory = unescape(fields[2]);
			if (fields[1] == "export") state.storage.lastExportDirectory = unescape(fields[2]);
		} else if (fields[0] == "layout" && fields.size() >= 5) {
			state.layout.imageListWidth = static_cast<float>(parse_double(fields[1], state.layout.imageListWidth));
			state.layout.inspectorWidth = static_cast<float>(parse_double(fields[2], state.layout.inspectorWidth));
			state.layout.imageListCollapsed = fields[3] == "1";
			state.layout.inspectorCollapsed = fields[4] == "1";
		} else if (fields[0] == "selection" && fields.size() >= 3) {
			if (fields[1] == "backend") state.selection.selectedBackend = BackendId{parse_u64(fields[2])};
		} else if (fields[0] == "mode" && fields.size() >= 5) {
			state.viewMode.kind = parse_mode(fields[1]);
			state.viewMode.splitPosition = parse_double(fields[2], state.viewMode.splitPosition);
			state.viewMode.blinkIntervalSeconds = parse_double(fields[3], state.viewMode.blinkIntervalSeconds);
			state.viewMode.differenceGain = parse_double(fields[4], state.viewMode.differenceGain);
		} else if (fields[0] == "pane" && fields.size() >= 7) {
			Pane pane;
			const uint64_t storedPaneId = parse_u64(fields[1]);
			pane.id = PaneId{storedPaneId};
			if (pane_exists(state, pane.id)) {
				pane.id = allocate_pane_id(state);
				paneIdRemap[storedPaneId] = pane.id;
			} else {
				state.nextId = std::max(state.nextId, pane.id.value + 1);
			}
			const uint64_t image = parse_u64(fields[2]);
			if (image != 0) pane.image = ImageId{image};
			pane.transform.scale = parse_double(fields[3], 1.0);
			pane.transform.centerX = parse_double(fields[4], 0.0);
			pane.transform.centerY = parse_double(fields[5], 0.0);
			const uint64_t link = parse_u64(fields[6]);
			if (link != 0) pane.linkGroup = static_cast<uint32_t>(link);
			state.panes.push_back(pane);
		} else if (fields[0] == "pane_order" && fields.size() >= 2) {
			const uint64_t storedPaneId = parse_u64(fields[1]);
			const auto remapped = paneIdRemap.find(storedPaneId);
			paneOrder.push_back(remapped == paneIdRemap.end() ? PaneId{storedPaneId} : remapped->second);
		} else if (fields[0] == "param" && fields.size() >= 5) {
			const BackendId backend{parse_u64(fields[1])};
			auto config = std::find_if(state.encoderConfigs.begin(), state.encoderConfigs.end(), [&](const EncoderConfig& existing) {
				return existing.backend == backend;
			});
			if (config == state.encoderConfigs.end()) {
				EncoderConfig newConfig;
				newConfig.backend = backend;
				state.encoderConfigs.push_back(std::move(newConfig));
				config = std::prev(state.encoderConfigs.end());
			}
			EncoderParam param;
			param.name = unescape(fields[2]);
			param.value = parse_param_value(fields[3], fields[4]);
			config->params.push_back(std::move(param));
		}
	}
	if (!paneOrder.empty()) {
		state.viewMode.paneOrder = std::move(paneOrder);
	}
	return state;
}

} // namespace codec_gui::gui
