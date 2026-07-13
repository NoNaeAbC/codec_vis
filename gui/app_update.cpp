#include "app_state.hpp"
#include "layout.hpp"
#include "storage.hpp"
#include "viewer_model.hpp"

#include <algorithm>
#include <cmath>
#include <charconv>
#include <iterator>
#include <sstream>
#include <span>
#include <string_view>
#include <system_error>
#include <type_traits>

namespace codec_gui::gui {
namespace {

template <typename Id>
Id make_id(uint64_t value) {
	return Id{value};
}

bool source_dependent(const ImageObject& image) {
	return image.type != ImageObjectType::Source;
}

void ensure_first_pane(AppState& state) {
	if (!state.panes.empty()) {
		return;
	}
	Pane pane;
	pane.id = next_pane_id(state);
	state.panes.push_back(pane);
	state.viewMode.paneOrder.push_back(pane.id);
	state.selection.activePane = pane.id;
}

void append_command(UpdateResult& result, CommandKind kind) {
	Command command;
	command.kind = kind;
	result.commands.push_back(std::move(command));
}

const char* action_name(ActionKind kind) {
	switch (kind) {
		case ActionKind::RequestOpenSource: return "RequestOpenSource";
		case ActionKind::OpenSourceCanceled: return "OpenSourceCanceled";
		case ActionKind::SourcePathChosen: return "SourcePathChosen";
		case ActionKind::SourceLoaded: return "SourceLoaded";
		case ActionKind::SourceLoadFailed: return "SourceLoadFailed";
		case ActionKind::SelectImage: return "SelectImage";
		case ActionKind::RemoveImage: return "RemoveImage";
		case ActionKind::SelectPane: return "SelectPane";
		case ActionKind::AssignImageToPane: return "AssignImageToPane";
		case ActionKind::ClearPaneImage: return "ClearPaneImage";
		case ActionKind::DuplicatePane: return "DuplicatePane";
		case ActionKind::ClosePane: return "ClosePane";
		case ActionKind::SetViewMode: return "SetViewMode";
		case ActionKind::SetPaneTransform: return "SetPaneTransform";
		case ActionKind::FitPaneToImage: return "FitPaneToImage";
		case ActionKind::SetPaneLinkGroup: return "SetPaneLinkGroup";
		case ActionKind::SetSplitPosition: return "SetSplitPosition";
		case ActionKind::SetDifferenceGain: return "SetDifferenceGain";
		case ActionKind::SetImageListSort: return "SetImageListSort";
		case ActionKind::SelectBackend: return "SelectBackend";
		case ActionKind::SetEncoderParam: return "SetEncoderParam";
		case ActionKind::ToggleScratchResults: return "ToggleScratchResults";
		case ActionKind::RefreshBackendCapabilities: return "RefreshBackendCapabilities";
		case ActionKind::BackendCapabilitiesReady: return "BackendCapabilitiesReady";
		case ActionKind::BackendCapabilitiesFailed: return "BackendCapabilitiesFailed";
		case ActionKind::CreateDifferenceImage: return "CreateDifferenceImage";
		case ActionKind::DerivedImageComputed: return "DerivedImageComputed";
		case ActionKind::DerivedImageFailed: return "DerivedImageFailed";
		case ActionKind::SelectEncodeRun: return "SelectEncodeRun";
		case ActionKind::StartEncodeRun: return "StartEncodeRun";
		case ActionKind::EncodeRunQueued: return "EncodeRunQueued";
		case ActionKind::EncodeRunStarted: return "EncodeRunStarted";
		case ActionKind::EncodeRunCompleted: return "EncodeRunCompleted";
		case ActionKind::EncodeRunFailed: return "EncodeRunFailed";
		case ActionKind::CancelEncodeRun: return "CancelEncodeRun";
		case ActionKind::EncodeRunCanceled: return "EncodeRunCanceled";
		case ActionKind::SaveEncodedResult: return "SaveEncodedResult";
		case ActionKind::SaveCompleted: return "SaveCompleted";
		case ActionKind::SaveCanceled: return "SaveCanceled";
		case ActionKind::SaveFailed: return "SaveFailed";
		case ActionKind::PointerMoved: return "PointerMoved";
		case ActionKind::PointerPressed: return "PointerPressed";
		case ActionKind::PointerReleased: return "PointerReleased";
		case ActionKind::PointerScrolled: return "PointerScrolled";
		case ActionKind::KeyPressed: return "KeyPressed";
		case ActionKind::WindowResized: return "WindowResized";
		case ActionKind::ErrorDismissed: return "ErrorDismissed";
	}
	return "UnknownAction";
}

const char* command_name(CommandKind kind) {
	switch (kind) {
		case CommandKind::ShowOpenFilePortal: return "ShowOpenFilePortal";
		case CommandKind::ShowSaveFilePortal: return "ShowSaveFilePortal";
		case CommandKind::LoadSourceImage: return "LoadSourceImage";
		case CommandKind::QueryBackendCapabilities: return "QueryBackendCapabilities";
		case CommandKind::RunEncode: return "RunEncode";
		case CommandKind::RequestEncodeCancel: return "RequestEncodeCancel";
		case CommandKind::DecodeEncodedBytes: return "DecodeEncodedBytes";
		case CommandKind::ComputeMetric: return "ComputeMetric";
		case CommandKind::ComputeDerivedImage: return "ComputeDerivedImage";
		case CommandKind::UploadImageTexture: return "UploadImageTexture";
		case CommandKind::SaveBytesToFile: return "SaveBytesToFile";
		case CommandKind::RequestRedraw: return "RequestRedraw";
	}
	return "UnknownCommand";
}

void append_debug_log(AppState& state, std::string message) {
	DebugLogEntry entry;
	entry.id = state.nextId++;
	entry.message = std::move(message);
	state.debug.recent.push_back(std::move(entry));
	constexpr std::size_t MaxEntries = 128;
	if (state.debug.recent.size() > MaxEntries) {
		state.debug.recent.erase(state.debug.recent.begin(), state.debug.recent.end() - static_cast<std::ptrdiff_t>(MaxEntries));
	}
}

void log_update(AppState& state, ActionKind action, std::span<const Command> commands) {
	if (!state.debug.enabled) {
		return;
	}
	if (action == ActionKind::PointerMoved) {
		return;
	}
	std::ostringstream line;
	line << "action " << action_name(action);
	if (!commands.empty()) {
		line << " ->";
		for (const Command& command : commands) {
			line << ' ' << command_name(command.kind);
		}
	}
	append_debug_log(state, line.str());
}

std::string param_widget_id(BackendId backend, const std::string& name) {
	return "param:" + std::to_string(backend.value) + ":" + name;
}

bool parse_param_widget_id(const std::string& id, BackendId& backend, std::string& name) {
	const std::string prefix = "param:";
	if (id.rfind(prefix, 0) != 0) {
		return false;
	}
	const std::size_t nameBegin = id.find(':', prefix.size());
	if (nameBegin == std::string::npos) {
		return false;
	}
	uint64_t backendValue = 0;
	const std::string_view backendText{id.data() + prefix.size(), nameBegin - prefix.size()};
	const auto parsed = std::from_chars(backendText.data(), backendText.data() + backendText.size(), backendValue);
	if (parsed.ec != std::errc{}) {
		return false;
	}
	backend = BackendId{backendValue};
	name = id.substr(nameBegin + 1);
	return true;
}

const EncoderParamInfo* find_param_info(const AppState& state, BackendId backend, const std::string& name) {
	const auto backendIt = std::find_if(state.backends.begin(), state.backends.end(), [&](const BackendInfo& info) {
		return info.id == backend;
	});
	if (backendIt == state.backends.end()) {
		return nullptr;
	}
	const auto paramIt = std::find_if(backendIt->params.begin(), backendIt->params.end(), [&](const EncoderParamInfo& param) {
		return param.name == name;
	});
	return paramIt == backendIt->params.end() ? nullptr : &*paramIt;
}

const BackendInfo* find_backend(const AppState& state, BackendId id) {
	const auto it = std::find_if(state.backends.begin(), state.backends.end(), [id](const BackendInfo& backend) {
		return backend.id == id;
	});
	return it == state.backends.end() ? nullptr : &*it;
}

EncoderConfig& ensure_encoder_config(AppState& state, BackendId backend) {
	auto config = std::find_if(state.encoderConfigs.begin(), state.encoderConfigs.end(), [backend](const EncoderConfig& cfg) {
		return cfg.backend == backend;
	});
	if (config == state.encoderConfigs.end()) {
		EncoderConfig newConfig;
		newConfig.backend = backend;
		state.encoderConfigs.push_back(std::move(newConfig));
		return state.encoderConfigs.back();
	}
	return *config;
}

void set_encoder_param(AppState& state, BackendId backend, EncoderParam param) {
	EncoderConfig& config = ensure_encoder_config(state, backend);
	auto existing = std::find_if(config.params.begin(), config.params.end(), [&](const EncoderParam& current) {
		return current.name == param.name;
	});
	if (existing == config.params.end()) {
		config.params.push_back(std::move(param));
	} else {
		*existing = std::move(param);
	}
}

ParamValue current_param_value(const AppState& state, BackendId backend, const EncoderParamInfo& info) {
	const auto config = std::find_if(state.encoderConfigs.begin(), state.encoderConfigs.end(), [backend](const EncoderConfig& cfg) {
		return cfg.backend == backend;
	});
	if (config != state.encoderConfigs.end()) {
		const auto param = std::find_if(config->params.begin(), config->params.end(), [&](const EncoderParam& existing) {
			return existing.name == info.name;
		});
		if (param != config->params.end()) {
			return param->value;
		}
	}
	return std::visit([](const auto& value) -> ParamValue {
		using T = std::decay_t<decltype(value)>;
		if constexpr (std::is_same_v<T, std::monostate>) {
			return std::string{};
		} else {
			return value;
		}
	}, info.defaultValue);
}

bool apply_text_input(AppState& state, const Action& action) {
	BackendId backend;
	std::string name;
	if (!parse_param_widget_id(state.interaction.focusedWidget, backend, name)) {
		return false;
	}
	const EncoderParamInfo* info = find_param_info(state, backend, name);
	if (info == nullptr || (info->kind != ParamKind::String && !info->directNumericInput)) {
		return false;
	}
	ParamValue value = current_param_value(state, backend, *info);
	std::string text;
	if (const std::string* existing = std::get_if<std::string>(&value)) {
		text = *existing;
	} else if (const int64_t* existing = std::get_if<int64_t>(&value)) {
		text = std::to_string(*existing);
	} else if (const double* existing = std::get_if<double>(&value)) {
		text = std::to_string(*existing);
	}
	if (action.text == "backspace") {
		if (info->directNumericInput && state.interaction.replaceFocusedNumericValue) text.clear();
		if (!text.empty()) {
			text.pop_back();
		}
	} else if (action.text == "escape" || action.text == "enter" || action.text == "fit" || action.text == "100%") {
		state.interaction.replaceFocusedNumericValue = false;
		return false;
	} else {
		if (info->directNumericInput && state.interaction.replaceFocusedNumericValue) text = action.text;
		else text += action.text;
	}
	state.interaction.replaceFocusedNumericValue = false;
	EncoderParam param;
	param.name = name;
	if (info->kind == ParamKind::Int) {
		try {
			const int64_t parsed = std::stoll(text.empty() || text == "-" ? "0" : text);
			const IntRange range = info->intRange.value_or(IntRange{INT64_MIN, INT64_MAX, 1});
			param.value = std::clamp(parsed, range.min, range.max);
		} catch (const std::exception&) { return true; }
	} else if (info->kind == ParamKind::Float) {
		try {
			const double parsed = std::stod(text.empty() || text == "-" ? "0" : text);
			const FloatRange range = info->floatRange.value_or(FloatRange{-1.0e300, 1.0e300, 0.01});
			param.value = std::clamp(parsed, range.min, range.max);
		} catch (const std::exception&) { return true; }
	} else {
		param.value = std::move(text);
	}
	set_encoder_param(state, backend, std::move(param));
	return true;
}

void queue_difference_compute(UpdateResult& result) {
	if (result.state.viewMode.kind != ViewModeKind::Difference || result.state.viewMode.paneOrder.size() < 2) {
		return;
	}
	const Pane* firstPane = find_pane(result.state, result.state.viewMode.paneOrder[0]);
	const Pane* secondPane = find_pane(result.state, result.state.viewMode.paneOrder[1]);
	if (firstPane == nullptr || secondPane == nullptr || !firstPane->image || !secondPane->image || *firstPane->image == *secondPane->image) {
		result.state.viewMode.generatedImage.reset();
		return;
	}
	const ImageObject* first = find_image(result.state, *firstPane->image);
	const ImageObject* second = find_image(result.state, *secondPane->image);
	if (first == nullptr || second == nullptr || !first->decoded || !second->decoded) {
		result.state.viewMode.generatedImage.reset();
		return;
	}
	Command command;
	command.kind = CommandKind::ComputeDerivedImage;
	command.image = first->id;
	command.otherImage = second->id;
	command.value = result.state.viewMode.differenceGain;
	result.commands.push_back(std::move(command));
}

std::optional<Rect> current_pane_rect(const AppState& state, PaneId id) {
	const LayoutResult layout = compute_layout(
		state.layout,
		state.interaction.framebufferWidth,
		state.interaction.framebufferHeight,
		state.interaction.outputScale
	);
	if (state.viewMode.kind == ViewModeKind::Blink) {
		const auto begin = state.viewMode.paneOrder.begin();
		const auto end = state.viewMode.paneOrder.begin() + std::min<std::size_t>(state.viewMode.paneOrder.size(), 2);
		if (std::find(begin, end, id) != end) {
			return layout.viewer;
		}
	}
	const std::vector<PaneRect> rects = compute_pane_rects(state.viewMode, state.panes, layout.viewer);
	const auto it = std::find_if(rects.begin(), rects.end(), [id](const PaneRect& rect) {
		return rect.pane == id;
	});
	return it == rects.end() ? std::nullopt : std::optional<Rect>{it->rect};
}

std::optional<PaneId> pane_at_point(const AppState& state, Point point) {
	const LayoutResult layout = compute_layout(
		state.layout,
		state.interaction.framebufferWidth,
		state.interaction.framebufferHeight,
		state.interaction.outputScale
	);
	const std::vector<PaneRect> rects = compute_pane_rects(state.viewMode, state.panes, layout.viewer);
	for (const PaneRect& rect : rects) {
		if (point.x >= rect.rect.x && point.y >= rect.rect.y &&
		    point.x < rect.rect.x + rect.rect.w && point.y < rect.rect.y + rect.rect.h) {
			return rect.pane;
		}
	}
	return std::nullopt;
}

bool split_divider_at_point(const AppState& state, Point point) {
	if (state.viewMode.kind != ViewModeKind::Split) {
		return false;
	}
	const LayoutResult layout = compute_layout(
		state.layout,
		state.interaction.framebufferWidth,
		state.interaction.framebufferHeight,
		state.interaction.outputScale
	);
	if (point.y < layout.viewer.y || point.y >= layout.viewer.y + layout.viewer.h ||
	    point.x < layout.viewer.x || point.x >= layout.viewer.x + layout.viewer.w) {
		return false;
	}
	const float splitX = layout.viewer.x + layout.viewer.w * static_cast<float>(std::clamp(state.viewMode.splitPosition, 0.0, 1.0));
	return std::fabs(point.x - splitX) <= 6.0f;
}

double split_position_from_point(const AppState& state, Point point) {
	const LayoutResult layout = compute_layout(
		state.layout,
		state.interaction.framebufferWidth,
		state.interaction.framebufferHeight,
		state.interaction.outputScale
	);
	if (layout.viewer.w <= 0.0f) {
		return state.viewMode.splitPosition;
	}
	return std::clamp(static_cast<double>((point.x - layout.viewer.x) / layout.viewer.w), 0.02, 0.98);
}

void set_pane_transform(AppState& state, PaneId id, ViewportTransform transform) {
	Pane* pane = find_pane(state, id);
	if (pane == nullptr) {
		return;
	}
	pane->transform = transform;
	if (state.viewMode.kind == ViewModeKind::Blink) {
		const auto begin = state.viewMode.paneOrder.begin();
		const auto end = state.viewMode.paneOrder.begin() + std::min<std::size_t>(state.viewMode.paneOrder.size(), 2);
		if (std::find(begin, end, id) != end) {
			for (auto it = begin; it != end; ++it) {
				if (*it == id) {
					continue;
				}
				if (Pane* other = find_pane(state, *it)) {
					other->transform = transform;
				}
			}
			return;
		}
	}
	if (state.viewMode.kind == ViewModeKind::Split || state.viewMode.kind == ViewModeKind::SideBySide) {
		const LayoutResult layout = compute_layout(
			state.layout,
			state.interaction.framebufferWidth,
			state.interaction.framebufferHeight,
			state.interaction.outputScale
		);
		bool visible = false;
		const std::vector<PaneRect> rects = compute_pane_rects(state.viewMode, state.panes, layout.viewer);
		for (const PaneRect& paneRect : rects) {
			if (paneRect.pane == id) {
				visible = true;
				break;
			}
		}
		if (visible) {
			for (const PaneRect& paneRect : rects) {
				if (paneRect.pane == id) {
					continue;
				}
				if (Pane* other = find_pane(state, paneRect.pane)) {
					other->transform = transform;
				}
			}
			return;
		}
	}
	if (!pane->linkGroup) {
		return;
	}
	for (Pane& other : state.panes) {
		if (other.id != pane->id && other.linkGroup == pane->linkGroup) {
			other.transform = transform;
		}
	}
}

void fit_visible_panes(AppState& state) {
	const LayoutResult layout = compute_layout(
		state.layout,
		state.interaction.framebufferWidth,
		state.interaction.framebufferHeight,
		state.interaction.outputScale
	);
	if (state.viewMode.kind == ViewModeKind::Blink) {
		const std::size_t count = std::min<std::size_t>(state.viewMode.paneOrder.size(), 2);
		for (std::size_t i = 0; i < count; ++i) {
			Pane* pane = find_pane(state, state.viewMode.paneOrder[i]);
			const ImageObject* image = pane != nullptr && pane->image ? find_image(state, *pane->image) : nullptr;
			if (pane != nullptr && image != nullptr) {
				set_pane_transform(state, pane->id, fit_transform(image->width, image->height, layout.viewer, state.interaction.outputScale));
			}
		}
		return;
	}
	const std::vector<PaneRect> rects = compute_pane_rects(state.viewMode, state.panes, layout.viewer);
	for (const PaneRect& paneRect : rects) {
		Pane* pane = find_pane(state, paneRect.pane);
		const ImageObject* image = pane != nullptr && pane->image ? find_image(state, *pane->image) : nullptr;
		if (pane != nullptr && image != nullptr) {
			const Rect fitRect = state.viewMode.kind == ViewModeKind::Split ? layout.viewer : paneRect.rect;
			pane->transform = fit_transform(image->width, image->height, fitRect, state.interaction.outputScale);
		}
	}
}

} // namespace

ImageId next_image_id(AppState& state) { return make_id<ImageId>(state.nextId++); }
PaneId next_pane_id(AppState& state) { return make_id<PaneId>(state.nextId++); }
EncodeRunId next_run_id(AppState& state) { return make_id<EncodeRunId>(state.nextId++); }
uint64_t next_error_id(AppState& state) { return state.nextId++; }

ImageObject* find_image(AppState& state, ImageId id) {
	const auto it = std::find_if(state.images.begin(), state.images.end(), [id](const ImageObject& image) {
		return image.id == id;
	});
	return it == state.images.end() ? nullptr : &*it;
}

const ImageObject* find_image(const AppState& state, ImageId id) {
	const auto it = std::find_if(state.images.begin(), state.images.end(), [id](const ImageObject& image) {
		return image.id == id;
	});
	return it == state.images.end() ? nullptr : &*it;
}

Pane* find_pane(AppState& state, PaneId id) {
	const auto it = std::find_if(state.panes.begin(), state.panes.end(), [id](const Pane& pane) {
		return pane.id == id;
	});
	return it == state.panes.end() ? nullptr : &*it;
}

const Pane* find_pane(const AppState& state, PaneId id) {
	const auto it = std::find_if(state.panes.begin(), state.panes.end(), [id](const Pane& pane) {
		return pane.id == id;
	});
	return it == state.panes.end() ? nullptr : &*it;
}

EncodeRun* find_run(AppState& state, EncodeRunId id) {
	const auto it = std::find_if(state.encodeRuns.begin(), state.encodeRuns.end(), [id](const EncodeRun& run) {
		return run.id == id;
	});
	return it == state.encodeRuns.end() ? nullptr : &*it;
}

void append_error(AppState& state, ErrorSeverity severity, std::string operation, std::string subsystem, std::string message) {
	AppError error;
	error.id = next_error_id(state);
	error.severity = severity;
	error.operation = std::move(operation);
	error.subsystem = std::move(subsystem);
	error.message = std::move(message);
	state.errors.push_back(std::move(error));
}

ColorMetadata color_metadata(const ColorDescription& color) {
	ColorMetadata out;
	switch (color.primaries) {
		case ColorPrimaries::BT709: out.primaries = "bt709"; break;
		case ColorPrimaries::BT2020: out.primaries = "bt2020"; break;
		case ColorPrimaries::DisplayP3: out.primaries = "display-p3"; break;
		default: out.primaries = "unspecified (H.273 " + std::to_string(static_cast<int>(color.primaries)) + ")"; break;
	}
	switch (color.transfer) {
		case TransferCharacteristics::SRGB: out.transfer = "srgb"; break;
		case TransferCharacteristics::BT709: out.transfer = "bt709"; break;
		case TransferCharacteristics::Linear: out.transfer = "linear"; break;
		case TransferCharacteristics::PQ: out.transfer = "pq"; break;
		case TransferCharacteristics::HLG: out.transfer = "hlg"; break;
		case TransferCharacteristics::BT2020_10: out.transfer = "bt2020-10"; break;
		case TransferCharacteristics::BT2020_12: out.transfer = "bt2020-12"; break;
		default: out.transfer = "unspecified (H.273 " + std::to_string(static_cast<int>(color.transfer)) + ")"; break;
	}
	switch (color.matrix) {
		case MatrixCoefficients::Identity: out.matrix = "identity"; break;
		case MatrixCoefficients::BT709: out.matrix = "bt709"; break;
		case MatrixCoefficients::BT2020NonConstant: out.matrix = "bt2020nc"; break;
		default: out.matrix = "unspecified (H.273 " + std::to_string(static_cast<int>(color.matrix)) + ")"; break;
	}
	out.fullRange = color.range == ColorRange::Full;
	return out;
}

UpdateResult update(AppState state, const Action& action) {
	UpdateResult result;
	result.state = std::move(state);

	switch (action.kind) {
		case ActionKind::RequestOpenSource:
			result.state.interaction.importPending = true;
			append_command(result, CommandKind::ShowOpenFilePortal);
			break;
		case ActionKind::OpenSourceCanceled:
			result.state.interaction.importPending = false;
			break;
		case ActionKind::SourcePathChosen: {
			if (action.path.has_parent_path()) {
				result.state.storage.lastImportDirectory = action.path.parent_path();
			}
			Command command;
			command.kind = CommandKind::LoadSourceImage;
			command.path = action.path;
			result.commands.push_back(std::move(command));
			break;
		}
		case ActionKind::SourceLoaded: {
			if (!action.sourceLoaded.image) {
				append_error(result.state, ErrorSeverity::Error, "load source", "image", "source loaded without image data");
				break;
			}
			result.state.images.erase(
				std::remove_if(result.state.images.begin(), result.state.images.end(), source_dependent),
				result.state.images.end()
			);
			result.state.encodeRuns.clear();
			result.state.viewMode.generatedImage.reset();
			ImageObject source;
			source.id = next_image_id(result.state);
			source.type = ImageObjectType::Source;
			source.displayName = action.sourceLoaded.path.empty() ? "Source" : action.sourceLoaded.path.filename().string();
			source.width = action.sourceLoaded.image->width;
			source.height = action.sourceLoaded.image->height;
			source.pixelFormat = action.sourceLoaded.image->format;
			source.color = color_metadata(action.sourceLoaded.image->color);
			source.decoded = action.sourceLoaded.image;
			result.state.images.erase(
				std::remove_if(result.state.images.begin(), result.state.images.end(), [](const ImageObject& image) {
					return image.type == ImageObjectType::Source;
				}),
				result.state.images.end()
			);
			result.state.images.push_back(source);
			result.state.panes.clear();
			ensure_first_pane(result.state);
			result.state.viewMode.kind = ViewModeKind::Single;
			result.state.viewMode.generatedImage.reset();
			result.state.viewMode.paneOrder = {result.state.panes.front().id};
			result.state.panes.front().image = source.id;
			if (const std::optional<Rect> rect = current_pane_rect(result.state, result.state.panes.front().id)) {
				result.state.panes.front().transform = fit_transform(source.width, source.height, *rect, result.state.interaction.outputScale);
			}
			result.state.selection.selectedImage = source.id;
			result.state.selection.activePane = result.state.panes.front().id;
			Command upload;
			upload.kind = CommandKind::UploadImageTexture;
			upload.image = source.id;
			result.commands.push_back(upload);
			append_command(result, CommandKind::RequestRedraw);
			break;
		}
		case ActionKind::SourceLoadFailed:
			result.state.interaction.importPending = false;
			append_error(result.state, ErrorSeverity::Error, "load source", "image", action.text);
			break;
		case ActionKind::AssignImageToPane: {
			Pane* pane = find_pane(result.state, action.pane);
			const ImageObject* image = find_image(result.state, action.image);
			if (pane == nullptr || image == nullptr) {
				append_error(result.state, ErrorSeverity::Warning, "assign image", "viewer", "invalid pane or image id");
				break;
			}
			pane->image = action.image;
			if (const std::optional<Rect> rect = current_pane_rect(result.state, action.pane)) {
				const LayoutResult layout = compute_layout(
					result.state.layout,
					result.state.interaction.framebufferWidth,
					result.state.interaction.framebufferHeight,
					result.state.interaction.outputScale
				);
				const Rect fitRect =
					result.state.viewMode.kind == ViewModeKind::Split || result.state.viewMode.kind == ViewModeKind::Blink
						? layout.viewer
						: *rect;
				set_pane_transform(
					result.state,
					action.pane,
					fit_transform(image->width, image->height, fitRect, result.state.interaction.outputScale)
				);
			}
			result.state.selection.activePane = action.pane;
			result.state.selection.selectedImage = action.image;
			Command upload;
			upload.kind = CommandKind::UploadImageTexture;
			upload.image = action.image;
			result.commands.push_back(upload);
			queue_difference_compute(result);
			append_command(result, CommandKind::RequestRedraw);
			break;
		}
		case ActionKind::ClearPaneImage:
			if (Pane* pane = find_pane(result.state, action.pane)) {
				pane->image.reset();
			} else {
				append_error(result.state, ErrorSeverity::Warning, "clear pane", "viewer", "invalid pane id");
			}
			append_command(result, CommandKind::RequestRedraw);
			break;
		case ActionKind::SelectImage:
			if (find_image(result.state, action.image) != nullptr) {
				result.state.selection.selectedImage = action.image;
			} else {
				append_error(result.state, ErrorSeverity::Warning, "select image", "viewer", "invalid image id");
			}
			break;
		case ActionKind::RemoveImage: {
			const auto active = std::find_if(result.state.encodeRuns.begin(), result.state.encodeRuns.end(), [&](const EncodeRun& run) {
				return run.source == action.image && (run.state == EncodeRunState::Queued || run.state == EncodeRunState::Running || run.state == EncodeRunState::CancelRequested);
			});
			if (active != result.state.encodeRuns.end()) {
				append_error(result.state, ErrorSeverity::Warning, "remove image", "image list", "cannot remove an image while its encode is active; cancel the run first");
				append_command(result, CommandKind::RequestRedraw);
				break;
			}
			result.state.images.erase(std::remove_if(result.state.images.begin(), result.state.images.end(), [&](const ImageObject& image) { return image.id == action.image; }), result.state.images.end());
			for (ImageObject& image : result.state.images) {
				image.parents.erase(std::remove(image.parents.begin(), image.parents.end(), action.image), image.parents.end());
			}
			for (Pane& pane : result.state.panes) if (pane.image == action.image) pane.image.reset();
			for (EncodeRun& run : result.state.encodeRuns) if (run.producedImage == action.image) run.producedImage.reset();
			if (result.state.selection.selectedImage == action.image) result.state.selection.selectedImage = {};
			append_command(result, CommandKind::RequestRedraw);
			break;
		}
		case ActionKind::SelectPane:
			if (find_pane(result.state, action.pane) != nullptr) {
				result.state.selection.activePane = action.pane;
			} else {
				append_error(result.state, ErrorSeverity::Warning, "select pane", "viewer", "invalid pane id");
			}
			break;
		case ActionKind::DuplicatePane: {
			const Pane* source = find_pane(result.state, action.pane);
			if (source == nullptr) {
				append_error(result.state, ErrorSeverity::Warning, "duplicate pane", "viewer", "invalid pane id");
				break;
			}
			Pane copy = *source;
			copy.id = next_pane_id(result.state);
			result.state.panes.push_back(copy);
			result.state.viewMode.paneOrder.push_back(copy.id);
			result.state.selection.activePane = copy.id;
			append_command(result, CommandKind::RequestRedraw);
			break;
		}
		case ActionKind::ClosePane:
			result.state.panes.erase(
				std::remove_if(result.state.panes.begin(), result.state.panes.end(), [action](const Pane& pane) {
					return pane.id == action.pane;
				}),
				result.state.panes.end()
			);
			result.state.viewMode.paneOrder.erase(
				std::remove(result.state.viewMode.paneOrder.begin(), result.state.viewMode.paneOrder.end(), action.pane),
				result.state.viewMode.paneOrder.end()
			);
			if (result.state.panes.empty()) {
				ensure_first_pane(result.state);
			}
			append_command(result, CommandKind::RequestRedraw);
			break;
		case ActionKind::SetViewMode:
			result.state.viewMode.kind = action.viewMode;
			result.state.viewMode.generatedImage.reset();
			apply_mode_transition(result.state.panes, result.state.viewMode, [&] {
				Pane pane;
				pane.id = next_pane_id(result.state);
				result.state.panes.push_back(pane);
				return pane.id;
			});
			fit_visible_panes(result.state);
			queue_difference_compute(result);
			append_command(result, CommandKind::RequestRedraw);
			break;
		case ActionKind::SetPaneTransform:
			if (Pane* pane = find_pane(result.state, action.pane)) {
				(void)pane;
				set_pane_transform(result.state, action.pane, action.transform);
			} else {
				append_error(result.state, ErrorSeverity::Warning, "set pane transform", "viewer", "invalid pane id");
			}
			append_command(result, CommandKind::RequestRedraw);
			break;
		case ActionKind::FitPaneToImage:
			if (Pane* pane = find_pane(result.state, action.pane)) {
				if (pane->image) {
					if (const ImageObject* image = find_image(result.state, *pane->image)) {
						Rect rect = current_pane_rect(result.state, action.pane).value_or(
							Rect{0, 0, static_cast<float>(result.state.interaction.framebufferWidth), static_cast<float>(result.state.interaction.framebufferHeight)}
						);
						set_pane_transform(result.state, action.pane, fit_transform(image->width, image->height, rect, result.state.interaction.outputScale));
						append_command(result, CommandKind::RequestRedraw);
					}
				}
			} else {
				append_error(result.state, ErrorSeverity::Warning, "fit pane", "viewer", "invalid pane id");
			}
			break;
		case ActionKind::SetPaneLinkGroup:
			if (Pane* pane = find_pane(result.state, action.pane)) {
				pane->linkGroup = action.value <= 0.0 ? std::nullopt : std::optional<uint32_t>{static_cast<uint32_t>(action.value)};
			} else {
				append_error(result.state, ErrorSeverity::Warning, "link pane", "viewer", "invalid pane id");
			}
			break;
		case ActionKind::SetSplitPosition:
			result.state.viewMode.splitPosition = std::clamp(action.value, 0.0, 1.0);
			append_command(result, CommandKind::RequestRedraw);
			break;
		case ActionKind::SetDifferenceGain:
			result.state.viewMode.differenceGain = std::max(0.0, action.value);
			result.state.viewMode.generatedImage.reset();
			queue_difference_compute(result);
			append_command(result, CommandKind::RequestRedraw);
			break;
		case ActionKind::SetImageListSort:
			if (result.state.imageList.sortKey == action.sortKey) {
				result.state.imageList.ascending = !result.state.imageList.ascending;
			} else {
				result.state.imageList.sortKey = action.sortKey;
				result.state.imageList.ascending = true;
			}
			append_command(result, CommandKind::RequestRedraw);
			break;
		case ActionKind::SelectBackend:
			result.state.selection.selectedBackend = action.backend;
			break;
		case ActionKind::SetEncoderParam: {
			const std::string widgetId = param_widget_id(action.backend, action.param.name);
			result.state.interaction.focusedWidget = widgetId;
			if (const BackendInfo* backend = find_backend(result.state, action.backend)) {
				const auto info = std::find_if(backend->params.begin(), backend->params.end(), [&](const EncoderParamInfo& param) { return param.name == action.param.name; });
				if (info != backend->params.end() &&
				    !info->directNumericInput &&
				    ((info->kind == ParamKind::Int && info->intRange) || (info->kind == ParamKind::Float && info->floatRange))) {
					result.state.interaction.activePointerCapture = widgetId;
				}
				if (info != backend->params.end() && info->directNumericInput) {
					result.state.interaction.replaceFocusedNumericValue = true;
				}
			}
			set_encoder_param(result.state, action.backend, action.param);
			append_command(result, CommandKind::RequestRedraw);
			break;
		}
		case ActionKind::ToggleScratchResults:
			result.state.scratchResults = !result.state.scratchResults;
			append_command(result, CommandKind::RequestRedraw);
			break;
		case ActionKind::RefreshBackendCapabilities: {
			Command command;
			command.kind = CommandKind::QueryBackendCapabilities;
			command.backend = action.backend;
			result.commands.push_back(std::move(command));
			break;
		}
		case ActionKind::BackendCapabilitiesReady: {
			auto backend = std::find_if(result.state.backends.begin(), result.state.backends.end(), [action](const BackendInfo& info) {
				return info.id == action.backendCapabilities.backend;
			});
			if (backend == result.state.backends.end()) {
				BackendInfo info;
				info.id = action.backendCapabilities.backend;
				info.capabilities = action.backendCapabilities.snapshot;
				info.params = action.backendCapabilities.params;
				result.state.backends.push_back(std::move(info));
			} else {
				backend->capabilities = action.backendCapabilities.snapshot;
				backend->params = action.backendCapabilities.params;
			}
			break;
		}
		case ActionKind::BackendCapabilitiesFailed:
			append_error(result.state, ErrorSeverity::Error, "query capabilities", "encoder", action.text);
			break;
		case ActionKind::CreateDifferenceImage: {
			const ImageObject* first = find_image(result.state, action.image);
			const ImageObject* second = find_image(result.state, action.otherImage);
			if (first == nullptr || second == nullptr) {
				append_error(result.state, ErrorSeverity::Warning, "create difference", "viewer", "invalid image ids");
				break;
			}
			if (first->decoded == nullptr || second->decoded == nullptr) {
				append_error(result.state, ErrorSeverity::Warning, "create difference", "viewer", "difference inputs are not decoded");
				break;
			}
			Command command;
			command.kind = CommandKind::ComputeDerivedImage;
			command.image = first->id;
			command.otherImage = second->id;
			command.value = result.state.viewMode.differenceGain;
			result.commands.push_back(command);
			break;
		}
		case ActionKind::DerivedImageComputed: {
			if (!action.derivedImage.image) {
				append_error(result.state, ErrorSeverity::Error, "compute difference", "viewer", "derived image payload had no image");
				break;
			}
			ImageObject* image = nullptr;
			if (result.state.viewMode.generatedImage) {
				image = find_image(result.state, *result.state.viewMode.generatedImage);
				if (image != nullptr && image->type != ImageObjectType::Derived) {
					image = nullptr;
				}
			}
			if (image == nullptr) {
				ImageObject created;
				created.id = next_image_id(result.state);
				created.type = ImageObjectType::Derived;
				result.state.images.push_back(std::move(created));
				image = &result.state.images.back();
			}
			image->type = ImageObjectType::Derived;
			image->displayName = action.derivedImage.displayName.empty() ? "Difference" : action.derivedImage.displayName;
			image->width = action.derivedImage.image->width;
			image->height = action.derivedImage.image->height;
			image->pixelFormat = action.derivedImage.image->format;
			image->color = color_metadata(action.derivedImage.image->color);
			image->decoded = action.derivedImage.image;
			image->encoded.reset();
			image->derived = DerivedMetadata{"absolute-difference", action.derivedImage.gain};
			image->parents = {action.derivedImage.first, action.derivedImage.second};
			result.state.viewMode.generatedImage = image->id;
			result.state.selection.selectedImage = action.derivedImage.second;
			Command upload;
			upload.kind = CommandKind::UploadImageTexture;
			upload.image = image->id;
			result.commands.push_back(upload);
			append_command(result, CommandKind::RequestRedraw);
			break;
		}
		case ActionKind::DerivedImageFailed:
			result.state.viewMode.generatedImage.reset();
			append_error(result.state, ErrorSeverity::Error, "compute difference", "viewer", action.text);
			break;
		case ActionKind::SelectEncodeRun:
			if (find_run(result.state, action.run) != nullptr) {
				result.state.selection.selectedRun = action.run;
			} else {
				append_error(result.state, ErrorSeverity::Warning, "select encode run", "encoder", "invalid run id");
			}
			break;
		case ActionKind::StartEncodeRun: {
			if (!valid(action.image) || !valid(action.backend)) {
				append_error(result.state, ErrorSeverity::Warning, "start encode", "encoder", "missing source image or backend");
				break;
			}
			EncodeRun run;
			run.id = next_run_id(result.state);
			run.source = action.image;
			run.backend = action.backend;
			run.params = action.params;
			run.replacePreviousResult = result.state.scratchResults;
			result.state.encodeRuns.push_back(run);
			result.state.selection.selectedRun = run.id;
			Command command;
			command.kind = CommandKind::RunEncode;
			command.run = run.id;
			command.image = action.image;
			command.backend = action.backend;
			command.params = action.params;
			result.commands.push_back(command);
			break;
		}
		case ActionKind::EncodeRunStarted:
			if (EncodeRun* run = find_run(result.state, action.run)) {
				run->state = EncodeRunState::Running;
				run->startedSeconds = action.value;
			}
			break;
		case ActionKind::EncodeRunCompleted: {
			EncodeRun* run = find_run(result.state, action.encodeCompleted.run);
			if (run == nullptr) {
				append_error(result.state, ErrorSeverity::Error, "complete encode", "encoder", "invalid encode completion payload");
				break;
			}
			const ImageObject* source = find_image(result.state, run->source);
			ImageObject resultImage;
			resultImage.id = next_image_id(result.state);
			resultImage.type = ImageObjectType::EncodedResult;
			resultImage.displayName = action.encodeCompleted.metadata.backendName.empty()
				? "Encoded result"
				: action.encodeCompleted.metadata.backendName;
			resultImage.displayName += "  run #" + std::to_string(run->id.value);
			if (action.encodeCompleted.preview) {
				resultImage.width = action.encodeCompleted.preview->width;
				resultImage.height = action.encodeCompleted.preview->height;
				resultImage.pixelFormat = action.encodeCompleted.preview->format;
				resultImage.color = color_metadata(action.encodeCompleted.preview->color);
			} else if (source != nullptr) {
				resultImage.width = source->width;
				resultImage.height = source->height;
				resultImage.pixelFormat = action.encodeCompleted.metadata.codedPixelFormat;
				resultImage.color = color_metadata(action.encodeCompleted.metadata.codedColor);
			}
			resultImage.decoded = action.encodeCompleted.preview;
			resultImage.encoded = action.encodeCompleted.metadata;
			resultImage.encoded->byteSize = resultImage.encoded->bytes.size();
			resultImage.parents.push_back(run->source);
			if (run->replacePreviousResult) {
				std::vector<ImageId> removed;
				for (const ImageObject& image : result.state.images) {
					if (image.encoded && image.encoded->backend == run->backend &&
					    std::find(image.parents.begin(), image.parents.end(), run->source) != image.parents.end()) {
						removed.push_back(image.id);
					}
				}
				auto isRemoved = [&](ImageId id) { return std::find(removed.begin(), removed.end(), id) != removed.end(); };
				result.state.images.erase(std::remove_if(result.state.images.begin(), result.state.images.end(), [&](const ImageObject& image) { return isRemoved(image.id); }), result.state.images.end());
				for (Pane& pane : result.state.panes) if (pane.image && isRemoved(*pane.image)) pane.image.reset();
				for (EncodeRun& oldRun : result.state.encodeRuns) if (oldRun.producedImage && isRemoved(*oldRun.producedImage)) oldRun.producedImage.reset();
			}
			run->state = EncodeRunState::Completed;
			if (action.value > 0.0) {
				run->finishedSeconds = action.value;
			} else if (action.encodeCompleted.metadata.encodeSeconds > 0.0) {
				run->finishedSeconds = run->startedSeconds + action.encodeCompleted.metadata.encodeSeconds;
			}
			run->producedImage = resultImage.id;
			result.state.images.push_back(resultImage);
			result.state.selection.selectedImage = resultImage.id;
			if (resultImage.decoded) {
				Command upload;
				upload.kind = CommandKind::UploadImageTexture;
				upload.image = resultImage.id;
				result.commands.push_back(upload);
			}
			append_command(result, CommandKind::RequestRedraw);
			break;
		}
		case ActionKind::EncodeRunFailed:
			if (EncodeRun* run = find_run(result.state, action.run)) {
				run->state = EncodeRunState::Failed;
				if (action.value > 0.0) {
					run->finishedSeconds = action.value;
				}
				run->error = action.text;
				std::string backendName = "encoder";
				if (const BackendInfo* backend = find_backend(result.state, run->backend)) {
					backendName = backend->name.empty() ? backend->capabilities.implementation : backend->name;
				}
				append_error(result.state, ErrorSeverity::Error, "encode", backendName.empty() ? "encoder" : backendName, action.text);
			} else {
				append_error(result.state, ErrorSeverity::Error, "encode", "encoder", action.text);
			}
			break;
		case ActionKind::CancelEncodeRun:
			if (EncodeRun* run = find_run(result.state, action.run)) {
				if (run->state == EncodeRunState::Queued) {
					run->state = EncodeRunState::Canceled;
				} else if (run->state == EncodeRunState::Running) {
					run->state = EncodeRunState::CancelRequested;
					Command command;
					command.kind = CommandKind::RequestEncodeCancel;
					command.run = action.run;
					result.commands.push_back(command);
				}
			}
			break;
		case ActionKind::EncodeRunCanceled:
			if (EncodeRun* run = find_run(result.state, action.run)) {
				run->state = EncodeRunState::Canceled;
				if (action.value > 0.0) {
					run->finishedSeconds = action.value;
				}
			}
			break;
		case ActionKind::SaveEncodedResult: {
			const ImageObject* image = find_image(result.state, action.image);
			if (image == nullptr || !image->encoded) {
				append_error(result.state, ErrorSeverity::Warning, "save encoded result", "storage", "selected image has no encoded bytes");
				break;
			}
			if (action.path.empty()) {
				Command portal;
				portal.kind = CommandKind::ShowSaveFilePortal;
				portal.image = action.image;
				portal.path = default_export_path(result.state, *image);
				result.commands.push_back(std::move(portal));
				break;
			}
			Command command;
			command.kind = CommandKind::SaveBytesToFile;
			command.image = action.image;
			command.path = action.path;
			command.bytes = image->encoded->bytes;
			command.value = action.value;
			result.commands.push_back(std::move(command));
			break;
		}
		case ActionKind::SaveCompleted:
			if (ImageObject* image = find_image(result.state, action.image)) {
				if (image->encoded) {
					image->encoded->outputPath = action.path;
				}
			}
			if (action.path.has_parent_path()) {
				result.state.storage.lastExportDirectory = action.path.parent_path();
			}
			break;
		case ActionKind::SaveCanceled:
			break;
		case ActionKind::SaveFailed:
			append_error(result.state, ErrorSeverity::Error, "save encoded result", "storage", action.text);
			break;
		case ActionKind::PointerMoved: {
			const Point previous = result.state.interaction.lastPointer;
			result.state.interaction.lastPointer = action.point;
			result.state.interaction.hoveredPane = pane_at_point(result.state, action.point).value_or(PaneId{});
			if (result.state.interaction.activePointerCapture == "split-divider") {
				result.state.viewMode.splitPosition = split_position_from_point(result.state, action.point);
				append_command(result, CommandKind::RequestRedraw);
				break;
			}
			if (result.state.interaction.activePointerCapture == "pane-pan" && valid(result.state.selection.activePane)) {
				Pane* pane = find_pane(result.state, result.state.selection.activePane);
				if (pane != nullptr && pane->image) {
					if (const ImageObject* image = find_image(result.state, *pane->image)) {
						const Rect rect = current_pane_rect(result.state, pane->id).value_or(Rect{});
						set_pane_transform(result.state, pane->id, pan_transform(pane->transform, *image, rect, action.point.x - previous.x, action.point.y - previous.y));
						append_command(result, CommandKind::RequestRedraw);
					}
				}
			}
			break;
		}
		case ActionKind::PointerPressed:
			if (split_divider_at_point(result.state, action.point)) {
				result.state.interaction.activePointerCapture = "split-divider";
				result.state.interaction.lastPointer = action.point;
				break;
			}
			if (std::optional<PaneId> pane = pane_at_point(result.state, action.point)) {
				result.state.selection.activePane = *pane;
				result.state.interaction.hoveredPane = *pane;
				result.state.interaction.activePointerCapture = "pane-pan";
				result.state.interaction.lastPointer = action.point;
			}
			break;
		case ActionKind::PointerReleased:
			result.state.interaction.activePointerCapture.clear();
			result.state.interaction.lastPointer = action.point;
			break;
		case ActionKind::PointerScrolled: {
			const LayoutResult layout = compute_layout(
				result.state.layout,
				result.state.interaction.framebufferWidth,
				result.state.interaction.framebufferHeight,
				result.state.interaction.outputScale
			);
			auto contains = [](Rect rect, Point point) {
				return point.x >= rect.x && point.y >= rect.y &&
				       point.x < rect.x + rect.w && point.y < rect.y + rect.h;
			};
			if (contains(layout.imageList, action.point)) {
				const float contentHeight = 42.0f + 56.0f * static_cast<float>(result.state.images.size());
				const float maximum = std::max(0.0f, contentHeight - layout.imageList.h);
				result.state.imageList.scrollOffset = std::clamp(
					result.state.imageList.scrollOffset + static_cast<float>(action.value), 0.0f, maximum
				);
				append_command(result, CommandKind::RequestRedraw);
				break;
			}
			if (contains(layout.inspector, action.point)) {
				std::size_t parameterCount = 0;
				for (const BackendInfo& backend : result.state.backends) {
					if (backend.id == result.state.selection.selectedBackend) {
						parameterCount = static_cast<std::size_t>(std::count_if(
							backend.params.begin(), backend.params.end(),
							[](const EncoderParamInfo& param) { return param.relevantForStillImage; }
						));
						break;
					}
				}
				const float estimatedHeight = 120.0f + 24.0f * static_cast<float>(result.state.backends.size()) +
				                              48.0f * static_cast<float>(parameterCount) + 420.0f;
				const float maximum = std::max(0.0f, estimatedHeight - layout.inspector.h);
				result.state.layout.inspectorScrollOffset = std::clamp(
					result.state.layout.inspectorScrollOffset + static_cast<float>(action.value), 0.0f, maximum
				);
				append_command(result, CommandKind::RequestRedraw);
				break;
			}
			if (std::optional<PaneId> paneId = pane_at_point(result.state, action.point)) {
				Pane* pane = find_pane(result.state, *paneId);
				if (pane != nullptr && pane->image) {
					if (const ImageObject* image = find_image(result.state, *pane->image)) {
						const Rect rect = current_pane_rect(result.state, pane->id).value_or(Rect{});
						const double factor = std::pow(1.0015, -action.value);
						set_pane_transform(result.state, pane->id, zoom_transform(pane->transform, *image, rect, action.point, factor));
						result.state.selection.activePane = pane->id;
						append_command(result, CommandKind::RequestRedraw);
					}
				}
			}
			break;
		}
		case ActionKind::KeyPressed:
			if (action.text == "debug") {
				result.state.debug.enabled = !result.state.debug.enabled;
				append_command(result, CommandKind::RequestRedraw);
				break;
			}
			if (apply_text_input(result.state, action)) {
				append_command(result, CommandKind::RequestRedraw);
				break;
			}
			if (valid(result.state.selection.activePane)) {
				Pane* pane = find_pane(result.state, result.state.selection.activePane);
				if (pane != nullptr && pane->image) {
					if (const ImageObject* image = find_image(result.state, *pane->image)) {
						if (action.text == "fit") {
							const Rect rect = current_pane_rect(result.state, pane->id).value_or(Rect{});
							set_pane_transform(result.state, pane->id, fit_transform(image->width, image->height, rect, result.state.interaction.outputScale));
							append_command(result, CommandKind::RequestRedraw);
						} else if (action.text == "100%") {
							set_pane_transform(result.state, pane->id, one_to_one_transform(image->width, image->height));
							append_command(result, CommandKind::RequestRedraw);
						}
					}
				}
			}
			break;
		case ActionKind::WindowResized:
			result.state.interaction.framebufferWidth = action.width;
			result.state.interaction.framebufferHeight = action.height;
			result.state.interaction.outputScale = action.outputScale;
			append_command(result, CommandKind::RequestRedraw);
			break;
		case ActionKind::ErrorDismissed:
			result.state.errors.erase(
				std::remove_if(result.state.errors.begin(), result.state.errors.end(), [&](const AppError& error) {
					return error.id == static_cast<uint64_t>(action.value);
				}),
				result.state.errors.end()
			);
			break;
		default:
			break;
	}

	log_update(result.state, action.kind, result.commands);
	return result;
}

} // namespace codec_gui::gui
