#include "decoder_ui.hpp"
#include "../codec_gui_x265.hpp"
#include "app_state.hpp"
#include "image_list_model.hpp"
#include "layout.hpp"
#include "storage.hpp"
#include "viewer_model.hpp"

#include <algorithm>
#include <cmath>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <optional>
#include <sstream>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

namespace codec_gui::gui {
namespace {

template <typename Id>
Id make_id(uint64_t value) {
	return Id{value};
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
        case ActionKind::SetLivePreviewEnabled:return "SetLivePreviewEnabled";
        case ActionKind::RequestLivePreview:return "RequestLivePreview";
        case ActionKind::LivePreviewCompleted:return "LivePreviewCompleted";
        case ActionKind::LivePreviewFailed:return "LivePreviewFailed";
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
		case ActionKind::InspectEncoderInput: return "InspectEncoderInput";
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
        case ActionKind::ShowEncoderInspector: return "ShowEncoderInspector";
        case ActionKind::ShowDecoderInspector: return "ShowDecoderInspector";
        case ActionKind::QueryDecoders: return "QueryDecoders";
        case ActionKind::DecodersReady: return "DecodersReady";
        case ActionKind::SelectDecoder: return "SelectDecoder";
        case ActionKind::SetDecodeRepetitions: return "SetDecodeRepetitions";
        case ActionKind::StartDecode: return "StartDecode";
        case ActionKind::CancelDecode: return "CancelDecode";
        case ActionKind::DecodeProgress: return "DecodeProgress";
        case ActionKind::DecodeFinished: return "DecodeFinished";
		case ActionKind::RequestEnergyAccess: return "RequestEnergyAccess";
        case ActionKind::ReleaseEnergyAccess: return "ReleaseEnergyAccess";
        case ActionKind::EnergyAccessReady: return "EnergyAccessReady";
		case ActionKind::RequestMetrics: return "RequestMetrics";
		case ActionKind::MetricsStarted: return "MetricsStarted";
		case ActionKind::MetricComputed: return "MetricComputed";
		case ActionKind::MetricsCompleted: return "MetricsCompleted";
		case ActionKind::MetricsFailed: return "MetricsFailed";
		case ActionKind::SaveEncodedResult: return "SaveEncodedResult";
		case ActionKind::SaveCompleted: return "SaveCompleted";
		case ActionKind::SaveCanceled: return "SaveCanceled";
		case ActionKind::SaveFailed: return "SaveFailed";
		case ActionKind::PointerMoved: return "PointerMoved";
		case ActionKind::PointerPressed: return "PointerPressed";
		case ActionKind::PointerReleased: return "PointerReleased";
		case ActionKind::PointerScrolled: return "PointerScrolled";
		case ActionKind::KeyPressed: return "KeyPressed";
        case ActionKind::KeyReleased: return "KeyReleased";
        case ActionKind::WindowFocusLost: return "WindowFocusLost";
        case ActionKind::ClipboardText: return "ClipboardText";
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
        case CommandKind::RunLivePreview:return "RunLivePreview";
        case CommandKind::CancelLivePreview:return "CancelLivePreview";
		case CommandKind::RequestEncodeCancel: return "RequestEncodeCancel";
        case CommandKind::QueryImageDecoders: return "QueryImageDecoders";
        case CommandKind::RunImageDecode: return "RunImageDecode";
        case CommandKind::CancelImageDecode: return "CancelImageDecode";
		case CommandKind::AuthorizeEnergyAccess: return "AuthorizeEnergyAccess";
        case CommandKind::CloseEnergyAccess: return "CloseEnergyAccess";
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
	if (!id.starts_with(prefix)) {
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
	const auto backendIt = std::ranges::find_if(state.backends, [&](const BackendInfo& info) {
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
	const auto it = std::ranges::find_if(state.backends, [id](const BackendInfo& backend) {
		return backend.id == id;
	});
	return it == state.backends.end() ? nullptr : &*it;
}

EncoderConfig& ensure_encoder_config(AppState& state, BackendId backend) {
	auto config = std::ranges::find_if(state.encoderConfigs, [backend](const EncoderConfig& cfg) {
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

void set_encoder_param(AppState &state, BackendId backend, EncoderParam param) {
	if (param.name == "bitrate-kbps") {
		const auto *info = find_backend(state, backend);
		if (info && std::ranges::any_of(info->params, [](const auto &p) { return p.name == "target-size-kb"; })) {
			param.name  = "target-size-kb";
			param.value = double(std::get<int64_t>(param.value)) / 8.;
		}
	}
	EncoderConfig &config = ensure_encoder_config(state, backend);
	auto existing = std::ranges::find_if(config.params,
										 [&](const EncoderParam &current) { return current.name == param.name; });
	if (existing == config.params.end()) {
		config.params.push_back(std::move(param));
	} else {
		*existing = std::move(param);
	}
}

ParamValue current_param_value(const AppState& state, BackendId backend, const EncoderParamInfo& info) {
	const auto config = std::ranges::find_if(state.encoderConfigs, [backend](const EncoderConfig& cfg) {
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
		if (state.interaction.replaceFocusedNumericValue) text.clear();
		if (!text.empty()) {
			text.pop_back();
		}
	} else if (action.text == "escape" || action.text == "enter" || action.text == "fit" || action.text == "100%") {
		state.interaction.replaceFocusedNumericValue = false;
		return false;
	} else {
		if (state.interaction.replaceFocusedNumericValue) text = action.text;
		else text += action.text;
	}
	state.interaction.replaceFocusedNumericValue = false;
	EncoderParam param;
	param.name = name;
	if (info->kind == ParamKind::Int) {
		try {
			const int64_t parsed = std::stoll(text.empty() || text == "-" ? "0" : text);
			const IntRange range = info->intRange.value_or(IntRange{.min=INT64_MIN, .max=INT64_MAX, .step=1});
			param.value = std::clamp(parsed, range.min, range.max);
		} catch (const std::exception&) { return true; }
	} else if (info->kind == ParamKind::Float) {
		try {
			const double parsed = std::stod(text.empty() || text == "-" ? "0" : text);
			const FloatRange range = info->floatRange.value_or(FloatRange{.min=-1.0e300, .max=1.0e300, .step=0.01});
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
		const auto end = state.viewMode.paneOrder.begin() + static_cast<std::ptrdiff_t>(std::min<std::size_t>(state.viewMode.paneOrder.size(), 2));
		if (std::find(begin, end, id) != end) {
			return layout.viewer;
		}
	}
	const std::vector<PaneRect> rects = compute_pane_rects(state.viewMode, state.panes, layout.viewer);
	const auto it = std::ranges::find_if(rects, [id](const PaneRect& rect) {
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
		const auto end = state.viewMode.paneOrder.begin() + static_cast<std::ptrdiff_t>(std::min<std::size_t>(state.viewMode.paneOrder.size(), 2));
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


} // namespace

ImageId next_image_id(AppState& state) { return make_id<ImageId>(state.nextId++); }
PaneId next_pane_id(AppState& state) { return make_id<PaneId>(state.nextId++); }
EncodeRunId next_run_id(AppState& state) { return make_id<EncodeRunId>(state.nextId++); }
uint64_t next_error_id(AppState& state) { return state.nextId++; }

ImageObject* find_image(AppState& state, ImageId id) {
	const auto it = std::ranges::find_if(state.images, [id](const ImageObject& image) {
		return image.id == id;
	});
	return it == state.images.end() ? nullptr : &*it;
}

const ImageObject* find_image(const AppState& state, ImageId id) {
	const auto it = std::ranges::find_if(state.images, [id](const ImageObject& image) {
		return image.id == id;
	});
	return it == state.images.end() ? nullptr : &*it;
}

Pane* find_pane(AppState& state, PaneId id) {
	const auto it = std::ranges::find_if(state.panes, [id](const Pane& pane) {
		return pane.id == id;
	});
	return it == state.panes.end() ? nullptr : &*it;
}

const Pane* find_pane(const AppState& state, PaneId id) {
	const auto it = std::ranges::find_if(state.panes, [id](const Pane& pane) {
		return pane.id == id;
	});
	return it == state.panes.end() ? nullptr : &*it;
}

EncodeRun* find_run(AppState& state, EncodeRunId id) {
	const auto it = std::ranges::find_if(state.encodeRuns, [id](const EncodeRun& run) {
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

static ColorMetadata color_metadata(const ColorDescription& color) {
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
    std::vector<ImageId> decoderWork;
    for(const auto& image:state.images)if(image.decoder.running || image.decoder.querying)decoderWork.push_back(image.id);
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
			result.state.interaction.importPending = false;
			if (!action.sourceLoaded.image) {
				append_error(result.state, ErrorSeverity::Error, "load source", "image", "source loaded without image data");
				break;
			}
			// Imports add to the workbench; completed experiments and active jobs retain their source.
			result.state.viewMode.generatedImage.reset();
			ImageObject source;
			source.id = next_image_id(result.state);
			source.type = ImageObjectType::Source;
			source.displayName = action.sourceLoaded.path.empty() ? "Source" : action.sourceLoaded.path.filename().string();
			source.width = action.sourceLoaded.image->width;
			source.height = action.sourceLoaded.image->height;
			source.pixelFormat = action.sourceLoaded.image->format;
			source.color = color_metadata(action.sourceLoaded.image->color);
			source.sourceByteSize = action.sourceLoaded.fileByteSize;
			source.decoded = action.sourceLoaded.image;
			source.sourceCompressed=source.decoded->compressedSource;source.sourceCodec=source.decoded->compressedCodec;

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
            result.state.selection.selectedRun = {};
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
            if(const auto* selected=find_image(result.state,action.image))result.state.selection.selectedRun=selected->encoded?selected->encoded->run:EncodeRunId{};
            if(result.state.decoderInspector)result.state.layout.inspectorScrollOffset=0;
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
            if(const auto* selected=find_image(result.state,action.image))result.state.selection.selectedRun=selected->encoded?selected->encoded->run:EncodeRunId{};
            if(result.state.decoderInspector)result.state.layout.inspectorScrollOffset=0;
			} else {
				append_error(result.state, ErrorSeverity::Warning, "select image", "viewer", "invalid image id");
			}
			break;
		case ActionKind::RemoveImage: {
			const auto active = std::ranges::find_if(result.state.encodeRuns, [&](const EncodeRun& run) {
				return run.source == action.image && (run.state == EncodeRunState::Queued || run.state == EncodeRunState::Running || run.state == EncodeRunState::CancelRequested);
			});
			if (active != result.state.encodeRuns.end()) {
				append_error(result.state, ErrorSeverity::Warning, "remove image", "image list", "cannot remove an image while its encode is active; cancel the run first");
				append_command(result, CommandKind::RequestRedraw);
				break;
			}
			std::vector<ImageId> removed{action.image};
			for (const auto& image : result.state.images)
				if (image.derived && image.derived->kind == "encoder-input" && std::ranges::find(image.parents, action.image) != image.parents.end()) removed.push_back(image.id);
			auto isRemoved = [&](ImageId id) { return std::ranges::find(removed, id) != removed.end(); };
			std::erase_if(result.state.images, [&](const ImageObject& image) { return isRemoved(image.id); });
			for (ImageObject& image : result.state.images) {
				std::erase_if(image.parents, isRemoved);
			}
			for (Pane& pane : result.state.panes) if (pane.image && isRemoved(*pane.image)) pane.image.reset();
			for (EncodeRun& run : result.state.encodeRuns) if (run.producedImage == action.image) run.producedImage.reset();
			if (isRemoved(result.state.selection.selectedImage)) result.state.selection.selectedImage = {};
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
			std::erase_if(result.state.panes, [action](const Pane& pane) {
					return pane.id == action.pane;
				});
			std::erase(result.state.viewMode.paneOrder, action.pane);
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
				if (const Pane* active = find_pane(result.state, result.state.selection.activePane)) pane.transform = active->transform;
				result.state.panes.push_back(pane);
				return pane.id;
			});
			// Opening a comparison of the selected result starts with its source
			// and decoded output. Existing explicit pane assignments are retained.
			if (action.viewMode != ViewModeKind::Single && result.state.viewMode.paneOrder.size() >= 2) {
				Pane* first = find_pane(result.state, result.state.viewMode.paneOrder[0]);
				Pane* second = find_pane(result.state, result.state.viewMode.paneOrder[1]);
				const ImageObject* selected = find_image(result.state, result.state.selection.selectedImage);
				if (first && second && !second->image && selected && selected->decoded &&
				    selected->type == ImageObjectType::EncodedResult && selected->parents.size() == 1 &&
				    (first->image == selected->id || first->image == selected->parents.front())) {
					first->image = selected->parents.front();
					second->image = selected->id;
					second->transform = first->transform;
				}
			}
			queue_difference_compute(result);
			append_command(result, CommandKind::RequestRedraw);
			break;
		case ActionKind::SetPaneTransform:
			if (Pane const* pane = find_pane(result.state, action.pane)) {
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
						Rect const rect = current_pane_rect(result.state, action.pane).value_or(
							Rect{.x=0, .y=0, .w=static_cast<float>(result.state.interaction.framebufferWidth), .h=static_cast<float>(result.state.interaction.framebufferHeight)}
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
			if(result.state.selection.selectedBackend!=action.backend){
                auto& live=result.state.livePreview;live.minimumGeneration=++live.requested;live.busy=false;live.requestedParams.clear();
                append_command(result,CommandKind::CancelLivePreview);
            }
			result.state.selection.selectedBackend = action.backend;
			break;
		case ActionKind::SetEncoderParam: {
			const std::string widgetId = param_widget_id(action.backend, action.param.name);
			result.state.interaction.focusedWidget = widgetId;
			if (const BackendInfo* backend = find_backend(result.state, action.backend)) {
				const auto info = std::ranges::find_if(backend->params, [&](const EncoderParamInfo& param) { return param.name == action.param.name; });
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
			auto backend = std::ranges::find_if(result.state.backends, [action](const BackendInfo& info) {
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
			command.value = action.text=="explicit-gain"||action.text=="linear-difference"?action.value:result.state.viewMode.differenceGain;
            command.text=action.text;
			result.commands.push_back(command);
			break;
		}
		case ActionKind::InspectEncoderInput: {
			const auto* resultImage = find_image(result.state, action.image);
			if (!resultImage || resultImage->type != ImageObjectType::EncodedResult || !resultImage->encoded || !resultImage->encoded->encoderInput) break;
			const bool exists = std::ranges::any_of(result.state.images, [&](const ImageObject& image) {
				return image.derived && image.derived->kind == "encoder-input" &&
					std::ranges::find(image.parents, action.image) != image.parents.end();
			});
			if (!exists) {
				ImageObject input;
				input.id = next_image_id(result.state);
				input.type = ImageObjectType::Derived;
				input.displayName = "Prepared input · run #" + std::to_string(resultImage->encoded->run.value);
				input.decoded = resultImage->encoded->encoderInput;
				input.width = input.decoded->width; input.height = input.decoded->height;
				input.pixelFormat = input.decoded->format; input.color = color_metadata(input.decoded->color);
				input.parents = resultImage->parents; input.parents.push_back(action.image);
				input.derived = DerivedMetadata{.kind="encoder-input"};
				result.state.images.push_back(std::move(input));
			}
			append_command(result, CommandKind::RequestRedraw);
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
			image->derived = DerivedMetadata{.kind="absolute-difference", .gain=action.derivedImage.gain,.firstRevision=action.derivedImage.firstRevision,.secondRevision=action.derivedImage.secondRevision};
			image->parents = {action.derivedImage.first, action.derivedImage.second};
			result.state.viewMode.generatedImage = image->id;

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
		case ActionKind::SetLivePreviewEnabled: {
            auto& live=result.state.livePreview;live.enabled=action.value!=0;
            if(live.enabled)live.requestedParams.clear();
            if(!live.enabled){live.minimumGeneration=++live.requested;live.busy=false;
                append_command(result,CommandKind::CancelLivePreview);}
            append_command(result,CommandKind::RequestRedraw);break;
        }
        case ActionKind::RequestLivePreview: {
            auto& live=result.state.livePreview;
            const auto* source=find_image(result.state,action.image);
            const auto* backend=find_backend(result.state,action.backend);
            if(!live.enabled || !source || !source->decoded || !backend || !backend->capabilities.available)break;
            if(live.source!=action.image || live.backend!=action.backend || live.requestedSource.lock()!=source->decoded)live.minimumGeneration=live.requested+1;
            bool sameSource=live.source==action.image;
            live.source=action.image;live.requestedSource=source->decoded;live.backend=action.backend;live.requestedParams=action.params;live.error.clear();live.busy=true;
            Command command;command.kind=CommandKind::RunLivePreview;command.image=action.image;command.backend=action.backend;
            command.params=action.params;command.revision=++live.requested;result.commands.push_back(std::move(command));
            result.state.selection.selectedRun={};
            result.state.selection.selectedImage=sameSource && find_image(result.state,live.image)?live.image:live.source;
            break;
        }
        case ActionKind::LivePreviewCompleted: {
            auto& live=result.state.livePreview;
            if(!live.enabled || live.source!=action.image || live.backend!=action.backend ||
                result.state.selection.selectedBackend!=action.backend || action.revision<live.minimumGeneration ||
                action.revision<=live.displayed || action.revision>live.requested)break;
            if(!action.encodeCompleted.preview || !find_image(result.state,live.source))break;
            ImageObject* image=find_image(result.state,live.image);
            if(!image){ImageObject fresh;fresh.id=next_image_id(result.state);live.image=fresh.id;
                result.state.images.push_back(std::move(fresh));image=&result.state.images.back();}
            image->type=ImageObjectType::LivePreview;image->parents={live.source};image->decoded=action.encodeCompleted.preview;
            image->width=image->decoded->width;image->height=image->decoded->height;image->pixelFormat=image->decoded->format;
            image->color=color_metadata(image->decoded->color);image->encoded=action.encodeCompleted.metadata;
            image->displayName="Live preview · "+image->encoded->backendName;
            image->decoder={};++image->previewRevision;
            live.displayed=action.revision;live.shownParams=action.params;live.seconds=action.value;live.error.clear();
            live.busy=live.displayed!=live.requested;
            if(!valid(result.state.selection.selectedRun) &&
                (result.state.selection.selectedImage==live.source || result.state.selection.selectedImage==live.image))
                result.state.selection.selectedImage=live.image;
            append_command(result,CommandKind::RequestRedraw);break;
        }
        case ActionKind::LivePreviewFailed: {
            auto& live=result.state.livePreview;
            if(action.revision==live.requested && action.image==live.source && action.backend==live.backend){live.busy=false;live.error=action.text;}
            append_command(result,CommandKind::RequestRedraw);break;
        }
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
				if (run->state == EncodeRunState::Canceled || run->state == EncodeRunState::CancelRequested) break;
				run->state = EncodeRunState::Running;
				run->startedSeconds = action.value;
			}
			break;
        case ActionKind::RequestEnergyAccess:
            if(!result.state.energyAccessPending && !result.state.energyAccessEnabled) {
                result.state.energyAccessPending=true;result.state.energyAccessError.clear();
                Command command;command.kind=CommandKind::AuthorizeEnergyAccess;result.commands.push_back(command);
            }break;
        case ActionKind::ReleaseEnergyAccess: {
            result.state.energyAccessPending=false;result.state.energyAccessEnabled=false;
            result.state.energyAccessDomain.clear();result.state.energyAccessError.clear();
            Command command;command.kind=CommandKind::CloseEnergyAccess;result.commands.push_back(command);break;
        }
        case ActionKind::EnergyAccessReady:
            if(result.state.energyAccessPending) {
                result.state.energyAccessPending=false;result.state.energyAccessEnabled=action.value==1;
                result.state.energyAccessDomain=action.value==1?action.text:"";
                result.state.energyAccessError=action.value==1?"":action.text;
            }break;
        case ActionKind::ShowEncoderInspector:
            result.state.decoderInspector=false;result.state.layout.inspectorScrollOffset=0;break;
        case ActionKind::ShowDecoderInspector:
            result.state.decoderInspector=true;result.state.layout.inspectorScrollOffset=0;break;
        case ActionKind::QueryDecoders:
            if(auto* image=find_image(result.state,action.image);image && !image->decoder.running && !image->decoder.querying)image->decoder.queried=false;
            break;
        case ActionKind::DecodersReady:
            if(auto* image=find_image(result.state,action.image);image && image->decoder.generation==action.decoder.generation) {
                auto& d=image->decoder;d.querying=false;d.queried=true;d.available=action.decoder.available;d.error=action.decoder.error;
                if(std::none_of(d.available.begin(),d.available.end(),[&](const auto& info){return info.id==d.selected;}))d.selected=d.available.empty()?"":d.available.front().id;
            } break;
        case ActionKind::SelectDecoder:
            if(auto* image=find_image(result.state,action.image);image && !image->decoder.running) {
                if(std::any_of(image->decoder.available.begin(),image->decoder.available.end(),[&](const auto& info){return info.id==action.text;}))image->decoder.selected=action.text;
            }break;
        case ActionKind::SetDecodeRepetitions:
            if(auto* image=find_image(result.state,action.image))image->decoder.repetitions=std::clamp(int(action.value),1,1000);
            break;
        case ActionKind::StartDecode:
            if(auto* image=find_image(result.state,action.image);image && !image->decoder.running && !image->decoder.selected.empty() && !image->decoder.querying) {
                // Serialize benchmarking with metrics to avoid compute contention.
                if(decode_metrics_busy(result.state,*image))break;
                auto& d=image->decoder;d.running=true;d.error.clear();++d.generation;
                DecodeMeasurement measurement;measurement.decoderId=d.selected;measurement.requested=action.value==1?1:d.repetitions;d.history.push_back(measurement);
                Command command;command.kind=CommandKind::RunImageDecode;command.image=image->id;command.text=d.selected;command.value=measurement.requested;command.revision=d.generation;result.commands.push_back(command);
            }break;
        case ActionKind::CancelDecode: {
            Command command;command.kind=CommandKind::CancelImageDecode;command.image=action.image;result.commands.push_back(command);break;
        }
        case ActionKind::DecodeProgress:
        case ActionKind::DecodeFinished:
            if(auto* image=find_image(result.state,action.image);image && image->decoder.generation==action.decoder.generation && image->decoder.running) {
                auto& d=image->decoder;if(!d.history.empty())d.history.back()=action.decoder.measurement;
                if(action.kind==ActionKind::DecodeFinished) {
                    d.running=false;d.error=action.decoder.error;
                    if(d.error.empty() && !action.decoder.measurement.samples.empty() && !action.decoder.measurement.samples.back().error.empty())d.error="Decode failed. See the recorded attempt below.";
                    // Decode measurement records attempts only. The established
                    // image and its quality scores remain the comparison target.
                }
            }break;
		case ActionKind::RequestMetrics: {
			ImageObject* image=find_image(result.state,action.image);
			if(!image || !image->decoded || !image->encoded || image->parents.empty() || image->decoder.running) break;
            if(const auto* reference=find_image(result.state,image->parents.front());reference && reference->decoder.running)break;
			auto& metadata=*image->encoded;
			if(metadata.metricState==MetricState::Queued || metadata.metricState==MetricState::Running) break;
			metadata.metricState=MetricState::Queued;
			metadata.requestedMetrics.clear();
            for(const auto& id:action.metricIds) {
                auto previous=std::ranges::find_if(metadata.metrics,[&](const auto& m){return m.id==id;});
                if(previous!=metadata.metrics.end() && previous->value)continue;
                metadata.requestedMetrics.push_back(id);
            }
            if(!action.metricIds.empty() && metadata.requestedMetrics.empty()){metadata.metricState=MetricState::Ready;break;}
            metadata.metricError.clear();
			Command command;command.kind=CommandKind::ComputeMetric;command.image=image->id;command.otherImage=image->parents.front();command.metricIds=metadata.requestedMetrics;
			result.commands.push_back(command);append_command(result,CommandKind::RequestRedraw);break;
		}
		case ActionKind::MetricsStarted:
			if(auto* image=find_image(result.state,action.image);image && image->encoded && image->encoded->metricState==MetricState::Queued)
				image->encoded->metricState=MetricState::Running;
			append_command(result,CommandKind::RequestRedraw);break;
		case ActionKind::MetricComputed:
			if(auto* image=find_image(result.state,action.image);image && image->encoded &&
			   (image->encoded->metricState==MetricState::Queued || image->encoded->metricState==MetricState::Running)) {
				auto& metadata=*image->encoded;
				std::erase_if(metadata.metrics,[&](const auto& m){return m.id==action.metric.id;});
				metadata.metrics.push_back(action.metric);
				if(action.metric.id=="psnr-y") metadata.psnrY=action.metric.value;
				if(action.metric.id=="psnr-all") metadata.psnrRgb=action.metric.value;
			}
			append_command(result,CommandKind::RequestRedraw);break;
		case ActionKind::MetricsCompleted:
			if(auto* image=find_image(result.state,action.image);image && image->encoded && image->encoded->metricState==MetricState::Running) {
				image->encoded->metricState=MetricState::Ready;
				image->encoded->metricSeconds=action.value;
				if(std::ranges::any_of(image->encoded->metrics,[](const auto& m){return !m.value;})) {
					image->encoded->metricState=MetricState::Failed;
					image->encoded->metricError="one or more metrics failed; inspect individual results";
				}
			}
			append_command(result,CommandKind::RequestRedraw);break;
		case ActionKind::MetricsFailed:
			if(auto* image=find_image(result.state,action.image);image && image->encoded) {
				image->encoded->metricState=MetricState::Failed;image->encoded->metricError=action.text;
			}
			append_command(result,CommandKind::RequestRedraw);break;
		case ActionKind::EncodeRunCompleted: {
			EncodeRun* run = find_run(result.state, action.encodeCompleted.run);
			if (run == nullptr) {
				append_error(result.state, ErrorSeverity::Error, "complete encode", "encoder", "invalid encode completion payload");
				break;
			}
			if (run->state == EncodeRunState::CancelRequested || run->state == EncodeRunState::Canceled) {
				run->state = EncodeRunState::Canceled;
				run->finishedSeconds = action.value;
				append_command(result, CommandKind::RequestRedraw);
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
				for (const ImageObject &image: result.state.images) {
					if (image.type == ImageObjectType::EncodedResult && image.encoded &&
						image.encoded->backend == run->backend &&
						std::ranges::find(image.parents, run->source) != image.parents.end() &&
						!image.decoder.running && image.encoded->metricState != MetricState::Queued &&
						image.encoded->metricState != MetricState::Running &&
						std::ranges::none_of(result.state.images, [&](const ImageObject &dependent) {
							return (!dependent.derived || dependent.derived->kind != "encoder-input") &&
								std::ranges::find(dependent.parents, image.id) != dependent.parents.end();
						})) {
						removed.push_back(image.id);
					}
				}
				auto isRemoved = [&](ImageId id) { return std::ranges::find(removed, id) != removed.end(); };
				for (const auto& image : result.state.images)
					if (image.derived && image.derived->kind == "encoder-input" && std::ranges::any_of(image.parents, isRemoved)) removed.push_back(image.id);
				if (isRemoved(result.state.selection.selectedImage)) {
					result.state.selection.selectedImage = resultImage.id;
					result.state.selection.selectedRun   = run->id;
				}
				std::erase_if(result.state.images, [&](const ImageObject &image) { return isRemoved(image.id); });
				for (Pane &pane: result.state.panes)
					if (pane.image && isRemoved(*pane.image)) pane.image = resultImage.id;
				for (EncodeRun &oldRun: result.state.encodeRuns)
					if (oldRun.producedImage && isRemoved(*oldRun.producedImage)) {
						oldRun.producedImage.reset();
						oldRun.replacedBy = run->id;
					}
			}
			run->state = EncodeRunState::Completed;
			if (action.value > 0.0) {
				run->finishedSeconds = action.value;
			} else if (action.encodeCompleted.metadata.encodeSeconds > 0.0) {
				run->finishedSeconds = run->startedSeconds + action.encodeCompleted.metadata.encodeSeconds;
			}
			run->producedImage = resultImage.id;
			result.state.images.push_back(resultImage);
			if(result.state.selection.selectedRun==run->id)result.state.selection.selectedImage = resultImage.id;
			if (resultImage.decoded && result.state.viewMode.kind == ViewModeKind::Single) {
				if (Pane* pane = find_pane(result.state, result.state.selection.activePane)) {
					pane->image = resultImage.id;
				}
			}
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
				if (run->state == EncodeRunState::Queued || run->state == EncodeRunState::Running) {
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
            if(action.button!=272)break;
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
				const float contentHeight = 4.0f + ImageListRowStep * static_cast<float>(result.state.images.size());
				const float viewportHeight = std::max(0.0f, layout.imageList.h - ImageListHeaderHeight);
				const float maximum = std::max(0.0f, contentHeight - viewportHeight);
				result.state.imageList.scrollOffset = std::clamp(
					result.state.imageList.scrollOffset + static_cast<float>(action.value), 0.0f, maximum
				);
				append_command(result, CommandKind::RequestRedraw);
				break;
			}
			if (contains(layout.inspector, action.point)) {
				std::size_t parameterCount = 0;
				std::size_t parameterGroupCount = 0;
				bool selectedBackendAvailable = true;
				for (const BackendInfo& backend : result.state.backends) {
					if (backend.id == result.state.selection.selectedBackend) {
						std::string previousGroup;
						for (const EncoderParamInfo& param : backend.params) {
							if (!param.relevantForStillImage) continue;
							++parameterCount;
							if (param.group != previousGroup) {
								previousGroup = param.group;
								++parameterGroupCount;
							}
						}
						selectedBackendAvailable = backend.capabilities.available;
						break;
					}
				}
				const float backendHeight = 54.0f + 24.0f * static_cast<float>(result.state.backends.size()) +
				                            (selectedBackendAvailable ? 0.0f : 38.0f);
				const float parameterHeight = 24.0f * static_cast<float>(parameterCount + parameterGroupCount);
				const float runHeight = result.state.encodeRuns.empty() ? 0.0f : 220.0f + 40.0f * static_cast<float>(std::min<std::size_t>(4, result.state.encodeRuns.size()));
				const float imageHeight = valid(result.state.selection.selectedImage) ? 260.0f : 0.0f;
				const float estimatedHeight = result.state.decoderInspector ? decoder_content_height(find_image(result.state,result.state.selection.selectedImage)) : backendHeight + parameterHeight + runHeight + imageHeight + 24.0f;
				const float viewportHeight = std::max(0.0f, layout.inspector.h - 40.0f);
				const float maximum = std::max(0.0f, estimatedHeight - viewportHeight);
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
        case ActionKind::WindowFocusLost:
            result.state.interaction.activePointerCapture.clear();
            break;
        case ActionKind::KeyReleased:
        case ActionKind::ClipboardText:
            break;
		case ActionKind::KeyPressed:
            if(action.control || action.alt) {
                if(action.control && (action.text=="a" || action.text=="A"))result.state.interaction.replaceFocusedNumericValue=true;
                break;
            }
            if(action.text=="tab" || action.text=="left" || action.text=="right" || action.text=="up" || action.text=="down" || action.text=="home" || action.text=="end" || action.text=="pageup" || action.text=="pagedown" || action.text=="delete")break;
			if (action.text == "debug") {
				result.state.debug.enabled = !result.state.debug.enabled;
				append_command(result, CommandKind::RequestRedraw);
				break;
			}
			if (action.text == "escape") {
				result.state.interaction.focusedWidget.clear();
				result.state.interaction.activePointerCapture.clear();
				if (!result.state.errors.empty()) result.state.errors.erase(result.state.errors.begin());
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
						if (action.text == "fit" || action.text == "f") {
							const Rect rect = current_pane_rect(result.state, pane->id).value_or(Rect{});
							set_pane_transform(result.state, pane->id, fit_transform(image->width, image->height, rect, result.state.interaction.outputScale));
							append_command(result, CommandKind::RequestRedraw);
						} else if (action.text == "100%" || action.text == "0") {
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
			std::erase_if(result.state.errors, [&](const AppError& error) {
					return error.id == static_cast<uint64_t>(action.value);
				});
			break;
		default:
			break;
	}

	log_update(result.state, action.kind, result.commands);
    if(result.state.decoderInspector && valid(result.state.selection.selectedImage)) {
        if(auto* image=find_image(result.state,result.state.selection.selectedImage);image && !image->decoder.queried && !image->decoder.querying && !image->decoder.running) {
            image->decoder.querying=true;++image->decoder.generation;
            Command query;query.kind=CommandKind::QueryImageDecoders;query.image=image->id;query.revision=image->decoder.generation;result.commands.push_back(query);
        }
    }
    for(const auto id:decoderWork)if(!find_image(result.state,id)) {
        Command cancel;cancel.kind=CommandKind::CancelImageDecode;cancel.image=id;result.commands.push_back(cancel);
    }
	return result;
}

} // namespace codec_gui::gui
