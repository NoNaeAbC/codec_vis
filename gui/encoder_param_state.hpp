#pragma once

#include "app_state.hpp"

#include <algorithm>
#include <string>
#include <type_traits>

namespace codec_gui::gui {

inline ParamValue resolved_param_value(const AppState& state, BackendId backend, const EncoderParamInfo& info) {
	const auto config = std::find_if(state.encoderConfigs.begin(), state.encoderConfigs.end(), [backend](const EncoderConfig& item) {
		return item.backend == backend;
	});
	if (config != state.encoderConfigs.end()) {
		const auto value = std::find_if(config->params.begin(), config->params.end(), [&](const EncoderParam& item) {
			return item.name == info.name;
		});
		if (value != config->params.end()) return value->value;
	}
	return std::visit([](const auto& value) -> ParamValue {
		using T = std::decay_t<decltype(value)>;
		if constexpr (std::is_same_v<T, std::monostate>) return std::string{};
		else return value;
	}, info.defaultValue);
}

inline std::string param_value_text(const ParamValue& value) {
	return std::visit([](const auto& item) -> std::string {
		using T = std::decay_t<decltype(item)>;
		if constexpr (std::is_same_v<T, bool>) return item ? "true" : "false";
		else if constexpr (std::is_same_v<T, int64_t>) return std::to_string(item);
		else if constexpr (std::is_same_v<T, double>) return std::to_string(item);
		else return item;
	}, value);
}

inline bool parameter_is_enabled(const AppState& state, const BackendInfo& backend, const EncoderParamInfo& info) {
	for (const ParamCondition& condition : info.enabledWhen) {
		const auto dependency = std::find_if(backend.params.begin(), backend.params.end(), [&](const EncoderParamInfo& candidate) {
			return candidate.name == condition.parameter;
		});
		if (dependency == backend.params.end()) return false;
		const std::string current = param_value_text(resolved_param_value(state, backend.id, *dependency));
		if (std::find(condition.acceptedValues.begin(), condition.acceptedValues.end(), current) == condition.acceptedValues.end()) return false;
	}
	return true;
}

inline std::string parameter_disabled_reason(const AppState& state, const BackendInfo& backend, const EncoderParamInfo& info) {
	for (const ParamCondition& condition : info.enabledWhen) {
		const auto dependency = std::find_if(backend.params.begin(), backend.params.end(), [&](const EncoderParamInfo& candidate) {
			return candidate.name == condition.parameter;
		});
		if (dependency == backend.params.end() ||
		    std::find(condition.acceptedValues.begin(), condition.acceptedValues.end(),
		              dependency == backend.params.end() ? std::string{} : param_value_text(resolved_param_value(state, backend.id, *dependency))) == condition.acceptedValues.end()) {
			return condition.explanation.empty() ? "Unavailable in the selected mode." : condition.explanation;
		}
	}
	return {};
}

} // namespace codec_gui::gui
