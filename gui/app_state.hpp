#pragma once

#include "../codec_gui_x265.hpp"
#include "decoder_types.hpp"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace codec_gui::gui {

	struct ImageId {
		uint64_t value = 0;
		friend bool operator==(ImageId, ImageId) = default;
	};
	struct PaneId {
		uint64_t value = 0;
		friend bool operator==(PaneId, PaneId) = default;
	};
	struct EncodeRunId {
		uint64_t value = 0;
		friend bool operator==(EncodeRunId, EncodeRunId) = default;
	};
	struct BackendId {
		uint64_t value = 0;
		friend bool operator==(BackendId, BackendId) = default;
	};
	struct TextureId {
		uint64_t value = 0;
		friend bool operator==(TextureId, TextureId) = default;
	};

	[[nodiscard]] constexpr bool valid(ImageId id) { return id.value != 0; }
	[[nodiscard]] constexpr bool valid(PaneId id) { return id.value != 0; }
	[[nodiscard]] constexpr bool valid(EncodeRunId id) { return id.value != 0; }
	[[nodiscard]] constexpr bool valid(BackendId id) { return id.value != 0; }
	[[nodiscard]] constexpr bool valid(TextureId id) { return id.value != 0; }

	struct Rect {
		float x = 0;
		float y = 0;
		float w = 0;
		float h = 0;
	};

	struct Point {
		float x = 0;
		float y = 0;
	};

	struct Color {
		float r = 0;
		float g = 0;
		float b = 0;
		float a = 1;
	};

	struct ColorMetadata {
		std::string primaries = "bt709";
		std::string transfer = "srgb";
		std::string matrix = "bt709";
		bool fullRange = false;
	};

	struct ParamSummary {
		std::string name;
		std::string value;
	};

	struct QualityMetricRecord {
		std::string id;
		std::string label;
		std::optional<double> value;
		std::string unit;
		bool higherIsBetter = true;
		std::string unavailableReason;
		std::string accuracyNote;
	};
	enum class MetricState { NotRequested, Queued, Running, Ready, Failed };

	enum class ImageObjectType {
		Source,
		EncodedResult,
		Derived,
		LivePreview,
	};

	struct EncodedMetadata {
		BackendId backend;
		EncodeRunId run;
		std::string codecName;
		std::string backendName;
		std::vector<std::byte> bytes;
		PixelFormat codedPixelFormat = PixelFormat::YUV420P8;
		ColorDescription codedColor;
		uint64_t byteSize = 0;
		double encodeSeconds = 0.0;
		double prepareSeconds = 0.0;
		bool preparationCacheHit = false;
		double decodeSeconds = 0.0;
		double metricSeconds = 0.0;
		std::filesystem::path outputPath;
		std::vector<ParamSummary> keyParams;
		std::vector<QualityMetricRecord> metrics;
		MetricState metricState = MetricState::NotRequested;
		std::optional<double> psnrY;
		std::optional<double> psnrRgb;
		std::string previewError;
		std::string metricError;
		std::string metricReference = "source (end-to-end)";
		std::vector<std::string> requestedMetrics;
		// Shared immutable application-prepared samples supplied to the encoder adapter.
		std::shared_ptr<const RawImage> encoderInput;
	};

	struct DerivedMetadata {
		std::string kind;
		double gain = 1.0;
        uint64_t firstRevision=0,secondRevision=0;
	};

	struct ImageObject {
		ImageId id;
		ImageObjectType type = ImageObjectType::Source;
		std::string displayName;
		int width = 0;
		int height = 0;
		PixelFormat pixelFormat = PixelFormat::YUV420P8;
		ColorMetadata color;
		std::optional<uint64_t> sourceByteSize;
		std::shared_ptr<const RawImage> decoded;
		std::optional<EncodedMetadata> encoded;
		std::optional<DerivedMetadata> derived;
		std::vector<ImageId> parents;
		std::shared_ptr<const EncodedImage> sourceCompressed;
		std::string sourceCodec;
		ImageDecoderState decoder;
		uint64_t previewRevision=0;
	};

	enum class ChannelView {
		Default,
		Y,
		U,
		V,
		Rgb,
	};

	struct ViewportTransform {
		double scale = 1.0;
		double centerX = 0.0;
		double centerY = 0.0;
	};

	struct Pane {
		PaneId id;
		std::optional<ImageId> image;
		ViewportTransform transform;
		std::optional<uint32_t> linkGroup;
		ChannelView channelView = ChannelView::Default;
	};

	enum class ViewModeKind {
		Single,
		SideBySide,
		Split,
		Blink,
		Difference,
		Grid,
	};

	struct ViewModeState {
		ViewModeKind kind = ViewModeKind::Single;
		std::vector<PaneId> paneOrder;
		double splitPosition = 0.5;
		double blinkIntervalSeconds = 0.5;
		double differenceGain = 1.0;
		std::optional<ImageId> generatedImage;
	};

	enum class BackendKind {
		Software,
		Hardware,
	};

	struct CapabilitySnapshot {
		std::string implementation;
		bool available = true;
		std::string error;
		std::vector<std::string> details;
	};

	struct BackendInfo {
		BackendId id;
		std::string name;
		std::string codec;
		BackendKind kind = BackendKind::Software;
		CapabilitySnapshot capabilities;
		std::vector<EncoderParamInfo> params;
	};

	struct EncoderConfig {
		BackendId backend;
		std::vector<EncoderParam> params;
	};

	enum class EncodeRunState {
		Queued,
		Running,
		Completed,
		Failed,
		Canceled,
		CancelRequested,
	};

	struct EncodeRun {
		EncodeRunId id;
		ImageId source;
		BackendId backend;
		std::vector<EncoderParam> params;
		bool replacePreviousResult = false;
		std::optional<EncodeRunId> replacedBy;
		EncodeRunState state = EncodeRunState::Queued;
		double startedSeconds = 0.0;
		double finishedSeconds = 0.0;
		std::string error;
		std::optional<ImageId> producedImage;
	};

	enum class ErrorSeverity {
		Info,
		Warning,
		Error,
		Fatal,
	};

	struct AppError {
		uint64_t id = 0;
		ErrorSeverity severity = ErrorSeverity::Error;
		std::string operation;
		std::string subsystem;
		std::string message;
		std::string detail;
		std::optional<ImageId> image;
		std::optional<EncodeRunId> run;
	};

	struct SelectionState {
		ImageId selectedImage;
		EncodeRunId selectedRun;
		BackendId selectedBackend;
		PaneId activePane;
	};

	struct LayoutState {
		bool imageListCollapsed = false;
		bool inspectorCollapsed = false;
		float imageListWidth = 280.0f;
		float inspectorWidth = 360.0f;
		float commandBarHeight = 40.0f;
		float statusBarHeight = 28.0f;
		float inspectorScrollOffset = 0.0f;
	};

	enum class ImageSortKey {
		CreationTime,
		EncodedSize,
		Backend,
		Metric,
	};

	struct ImageListState {
		ImageSortKey sortKey = ImageSortKey::CreationTime;
		std::string metricId; // empty selects the first admitted registry entry
		bool ascending = true;
		float scrollOffset = 0.0f;
	};

	struct StorageState {
		std::filesystem::path lastImportDirectory;
		std::filesystem::path lastExportDirectory;
	};

	struct DebugLogEntry {
		uint64_t id = 0;
		std::string message;
	};

	struct DebugState {
		bool enabled = false;
		std::vector<DebugLogEntry> recent;
	};

	struct InteractionState {
		std::string focusedWidget;
		std::string activePointerCapture;
		bool replaceFocusedNumericValue = false;
		PaneId hoveredPane;
		Point lastPointer;
		int framebufferWidth = 1280;
		int framebufferHeight = 720;
		float outputScale = 1.0f;
		bool importPending = false;
	};

	struct LivePreviewState {
        bool enabled=true,busy=false;
        ImageId source,image;
        std::weak_ptr<const RawImage> requestedSource;
        BackendId backend;
        uint64_t requested=0,displayed=0,minimumGeneration=0;
        std::vector<EncoderParam> requestedParams,shownParams;
        double seconds=0;
        std::string error;
    };
	struct AppState {
		std::vector<ImageObject> images;
		std::vector<Pane> panes;
		ViewModeState viewMode;
		std::vector<BackendInfo> backends;
		std::vector<EncoderConfig> encoderConfigs;
		std::vector<EncodeRun> encodeRuns;
		std::vector<AppError> errors;
		SelectionState selection;
		LayoutState layout;
		ImageListState imageList;
		StorageState storage;
		DebugState debug;
		InteractionState interaction;
		LivePreviewState livePreview;
		// Application workflow state: encoded results are retained unless the user
		// explicitly enables scratch replacement.
		bool scratchResults = false;
		bool decoderInspector=false;
		bool energyAccessPending=false, energyAccessEnabled=false;
		std::string energyAccessDomain, energyAccessError;
		uint64_t nextId = 1;
	};

	enum class ActionKind {
		RequestOpenSource,
		OpenSourceCanceled,
		SourcePathChosen,
		SourceLoaded,
		SourceLoadFailed,
		SelectImage,
		RemoveImage,
		SelectPane,
		AssignImageToPane,
		ClearPaneImage,
		DuplicatePane,
		ClosePane,
		SetViewMode,
		SetPaneTransform,
		FitPaneToImage,
		SetPaneLinkGroup,
		SetSplitPosition,
		SetDifferenceGain,
		SetImageListSort,
		SelectBackend,
		SetEncoderParam,
		ToggleScratchResults,
		RefreshBackendCapabilities,
		BackendCapabilitiesReady,
		BackendCapabilitiesFailed,
		CreateDifferenceImage,
		DerivedImageComputed,
		DerivedImageFailed,
		InspectEncoderInput,
		SelectEncodeRun,
		StartEncodeRun,
		SetLivePreviewEnabled,RequestLivePreview,LivePreviewCompleted,LivePreviewFailed,
		EncodeRunQueued,
		EncodeRunStarted,
		EncodeRunCompleted,
		EncodeRunFailed,
		CancelEncodeRun,
		EncodeRunCanceled,
		ShowEncoderInspector, ShowDecoderInspector, QueryDecoders, DecodersReady, SelectDecoder, SetDecodeRepetitions, StartDecode, CancelDecode, DecodeProgress, DecodeFinished,
		RequestEnergyAccess, ReleaseEnergyAccess, EnergyAccessReady,
		RequestMetrics,
		MetricsStarted,
		MetricComputed,
		MetricsCompleted,
		MetricsFailed,
		SaveEncodedResult,
		SaveCompleted,
		SaveCanceled,
		SaveFailed,
		PointerMoved,
		PointerPressed,
		PointerReleased,
		PointerScrolled,
		KeyPressed, KeyReleased, WindowFocusLost, ClipboardText,
		WindowResized,
		ErrorDismissed,
	};

	struct SourceLoadedPayload {
		std::filesystem::path path;
		std::optional<uint64_t> fileByteSize;
		std::shared_ptr<const RawImage> image;
	};

	struct EncodeCompletedPayload {
		EncodeRunId run;
		EncodedMetadata metadata;
		std::shared_ptr<const RawImage> preview;
	};

	struct BackendCapabilitiesPayload {
		BackendId backend;
		CapabilitySnapshot snapshot;
		std::vector<EncoderParamInfo> params;
	};

	struct DerivedImagePayload {
		ImageId first;
		ImageId second;
        uint64_t firstRevision=0,secondRevision=0;
		double gain = 1.0;
		std::string displayName;
		std::shared_ptr<const RawImage> image;
	};

	struct Action {
		ActionKind kind = ActionKind::RequestOpenSource;
		std::filesystem::path path;
		std::string text;
		ImageId image;
		ImageId otherImage;
		PaneId pane;
		BackendId backend;
		EncodeRunId run;
		ViewModeKind viewMode = ViewModeKind::Single;
		ImageSortKey sortKey = ImageSortKey::CreationTime;
		ViewportTransform transform;
		EncoderParam param;
		std::vector<EncoderParam> params;
		SourceLoadedPayload sourceLoaded;
		EncodeCompletedPayload encodeCompleted;
		BackendCapabilitiesPayload backendCapabilities;
		DerivedImagePayload derivedImage;
		QualityMetricRecord metric;
		std::vector<std::string> metricIds;
		DecoderNotification decoder;
		double value = 0.0;
		int width = 0;
		int height = 0;
		float outputScale = 1.0f;
		Point point;
		uint32_t button=272;
        uint32_t pointerSerial=0; // Native Wayland button-event serial; never synthesize it.
		uint64_t revision=0;
		bool shift=false,control=false,alt=false;
		float scrollX=0;
	};

	enum class CommandKind {
		ShowOpenFilePortal,
		ShowSaveFilePortal,
		LoadSourceImage,
		QueryBackendCapabilities,
		RunEncode,
		RunLivePreview,CancelLivePreview,
		RequestEncodeCancel,
		QueryImageDecoders, RunImageDecode, CancelImageDecode,
		AuthorizeEnergyAccess, CloseEnergyAccess,
		DecodeEncodedBytes,
		ComputeMetric,
		ComputeDerivedImage,
		UploadImageTexture,
		SaveBytesToFile,
		RequestRedraw,
	};

	struct Command {
		CommandKind kind = CommandKind::RequestRedraw;
		std::filesystem::path path;
		ImageId image;
		ImageId otherImage;
		EncodeRunId run;
		BackendId backend;
		std::vector<std::byte> bytes;
		std::string text;
		uint64_t revision=0;
		std::vector<EncoderParam> params;
		std::vector<std::string> metricIds;
		double value = 0.0;
	};

	struct UpdateResult {
		AppState state;
		std::vector<Command> commands;
	};

	[[nodiscard]] ImageId next_image_id(AppState& state);
	[[nodiscard]] PaneId next_pane_id(AppState& state);
	[[nodiscard]] EncodeRunId next_run_id(AppState& state);
	[[nodiscard]] uint64_t next_error_id(AppState& state);

	[[nodiscard]] ImageObject* find_image(AppState& state, ImageId id);
	[[nodiscard]] const ImageObject* find_image(const AppState& state, ImageId id);
	[[nodiscard]] Pane* find_pane(AppState& state, PaneId id);
	[[nodiscard]] const Pane* find_pane(const AppState& state, PaneId id);
	[[nodiscard]] EncodeRun* find_run(AppState& state, EncodeRunId id);

	void append_error(AppState& state, ErrorSeverity severity, std::string operation, std::string subsystem, std::string message);
	[[nodiscard]] UpdateResult update(AppState state, const Action& action);

} // namespace codec_gui::gui
