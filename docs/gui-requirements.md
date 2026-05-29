# Codec Vis GUI Requirements

## Purpose

Codec Vis is a native Linux image-compression inspection tool for understanding byte-size versus fidelity tradeoffs. It acquires one local source image through a desktop file-selection path, launch argument, or drag-and-drop, decodes it into the program's internal `RawImage` representation, lets the user configure still-image codec backends, runs encodes, and compares encoded byte size plus decoded visual quality against the source and other results.

It is not a video encoder UI. Do not expose GOP, B-frame, lookahead, scene-cut, timeline, playback, frame pacing, or other video-sequence controls unless they directly affect a single submitted image.

## Platform

- Windowing/input: Wayland, using `xdg-shell`.
- Rendering: Vulkan.
- Text shaping: HarfBuzz.
- Font rasterization: FreeType or an equivalent local rasterizer.
- No web UI, no network runtime dependency, no X11 requirement.

## First Screen

The first screen is the tool itself:

- central viewer area with one pane assigned to the source image when a source exists
- left rail with image objects and encoded results
- right inspector with encoder controls and selected item details
- top command bar for import, encode, cancel, save/export, layout mode, and zoom controls
- bottom status strip for active pane, image dimensions, zoom, cursor sample, encoded size, and active metric

No landing page, hero panel, or marketing-style layout.

## UI Layout

The layout must prioritize visual comparison. Encoder controls are important, but they must not dominate the first screen after an image is loaded.

Primary regions:

- `CommandBar`: fixed-height top bar for global commands
- `ImageList`: left rail for source, encoded results, and derived images
- `Viewer`: central area containing panes according to the active view mode
- `Inspector`: right panel for encoder configuration, selected image metadata, selected run details, and capability notes
- `StatusBar`: fixed-height bottom strip for cursor and comparison readouts

Default layout states:

- `No source`: Viewer shows a single empty pane with import actions; ImageList and Inspector remain visible but disabled where appropriate.
- `Source loaded`: Viewer has one pane assigned to the source image; ImageList shows the source object; Inspector defaults to encoder setup.
- `First result completed`: ImageList shows the result with encoded size; Viewer shows an explicit "assign result to pane" affordance and does not replace the source pane automatically.
- `Multiple results`: ImageList becomes the primary result navigation surface; panes are assigned explicitly by the user.

Region behavior:

- `CommandBar` is always visible.
- `StatusBar` is always visible.
- `Viewer` gets the largest share of window area.
- `ImageList` can collapse to icons or a narrow list.
- `Inspector` can collapse; collapsing it must not hide active encode status.
- Panes inside `Viewer` are resizable when the active mode uses spatially separate panes.

ImageList requirements:

- list every image object: source, encoded results, and derived images
- show encoded byte size directly on encoded result rows
- show codec/backend and key setting summary on encoded result rows
- show run state for in-progress encode runs
- support assigning a listed image to a pane
- support creating a derived difference image from any two compatible image objects
- support sorting results by encoded size, codec/backend, metric, and creation time

Inspector requirements:

- when an encoder/backend is selected, show image-relevant controls grouped by `EncoderParamInfo::group`
- when an image object is selected, show dimensions, format, metadata, encoded size if present, and parent images if derived
- when an encode run is selected, show backend, parameter summary, state, error text, duration, output path, and metrics
- hardware capability limitations must appear near the affected controls, not only in a log

StatusBar requirements:

- active pane id and assigned image name
- zoom and image-space cursor coordinate
- sampled pixel value for the active pane
- in linked multi-pane modes, sampled values for compared panes and delta when available
- selected encoded result size and active metric when a result is selected

Responsive constraints:

- at narrow widths, ImageList and Inspector become collapsible side panels
- controls must not overlap or truncate critical numeric values such as byte size and PSNR
- the Viewer must remain usable at 1280x720
- at 4K, the layout shows ImageList, Viewer, and Inspector simultaneously without excessive whitespace

## Image Scope

Supported workflows are still-image only:

- acquire a source image through the supported input paths
- preserve the existing test-pattern/debug path
- inspect source pixels
- configure image-relevant encoder controls
- encode one image per selected backend
- compare decoded output against the source
- save encoded files

Controls that only describe multi-frame video behavior are out of scope for the GUI.

## Input Acquisition

Interactive file selection must use the desktop portal over D-Bus:

- call `org.freedesktop.portal.FileChooser` through the session bus
- request a local readable image file
- handle portal cancellation as a normal non-error outcome
- convert the returned URI/path into the loader input path

Additional supported input paths:

- `argv`: open a local path passed at process launch, for shell use and file-manager "open with"
- Wayland drag-and-drop: accept local file drops through the Wayland data-device/data-offer path
- test pattern: keep the current generated image path available for debugging and smoke tests

All acquisition paths must converge on the same decode step that produces `RawImage` plus source metadata.

## Encoder Model

The GUI must build controls from `EncoderParamInfo`.

Parameter controls:

- `Bool`: checkbox or toggle
- `Int`: numeric input with slider/stepper when a bounded range exists
- `Float`: numeric input with slider when a bounded range exists
- `Enum`: dropdown or segmented selector
- `String`: text input only when the value is genuinely user-authored

Encoder parameters must be grouped by `EncoderParamInfo::group`.

The renderer must not call encoders directly. Encode runs execute outside the render loop and publish immutable status/results back to UI state.

## Capability-Driven Controls

The GUI must not show hard-coded hardware encoder choices when the implementation exposes capability bits.

Examples:

- VAAPI HEVC `rate-control` choices must come from `VAConfigAttribRateControl`.
- `VA_RC_MB` must not appear as a primary rate-control mode; it only gates MBBRC controls.
- VAAPI implementation identity is shown as API plus driver/vendor string, for example `VAAPI - Intel iHD driver ...`.
- DRM render-node paths are backend/device selection details, not encoding knobs.

If a capability query fails, do not invent selectable values. Show the backend as unavailable or degraded with the query error.

## Image-Relevant Encoder Controls

Expose controls that can affect a single still image:

- quantizer and quality controls
- rate-control modes supported by the backend
- encoder speed/quality level
- profile, level, bit depth, chroma format where the backend and source support them
- transform, partitioning, filtering, SAO/deblock/CDEF/restoration-style tools
- tiling/slicing only when valid for a single image
- color/range metadata where it affects encoded output

Do not expose:

- B frames
- GOP length
- lookahead
- scene cut
- reference frame count unless it changes single-image coding behavior
- temporal layer controls
- video playback/timeline controls

## Viewer Composition Model

The viewer has three separate layers:

1. image objects
2. pane assignments
3. view modes

These layers must not be collapsed into a hard-coded source/result UI.

## Image Objects

An image object is any displayable image-like item.

Types:

- `SourceImage`: decoded original input
- `EncodedResult`: decoded output from an encode run
- `DerivedImage`: computed image such as difference, heatmap, metric map, crop, or channel view

Each image object has:

- stable id
- display name
- type
- dimensions
- pixel format
- color/range metadata
- optional encode metadata
- optional parent image ids

## Pane Assignments

A pane is a viewport slot that references an image object.

Rules:

- a pane references zero or one image object by id
- multiple panes can reference the same image object
- any image object can be assigned to any pane
- pane assignment is independent of image creation
- pane assignment is independent of the active view mode
- empty panes show image-assignment UI
- panes store viewport transform state unless the active mode overrides it

Pane state:

- pane id
- assigned image id or empty
- zoom
- image-space center
- channel/view options
- link group id or none

## View Modes

A view mode arranges and combines panes.

A mode declares:

- minimum pane count
- maximum pane count, or unlimited
- whether panes are spatially separate or composited
- whether pane viewport transforms are independent, linked, or forced shared
- whether it produces a derived visual output

Required modes:

- `Single`: displays exactly one pane
- `SideBySide`: displays 2 or more panes in a row/grid
- `Split`: composites exactly two panes with a draggable divider
- `Blink`: composites exactly two panes by alternating visibility over time
- `Difference`: consumes exactly two panes and displays a derived absolute-difference image
- `Grid`: displays N panes in a regular grid

Mode constraints:

- `Single`: exactly 1 pane
- `SideBySide`: minimum 2 panes, UI default max 4
- `Split`: exactly 2 panes
- `Blink`: exactly 2 panes
- `Difference`: exactly 2 input panes, one displayed derived output
- `Grid`: minimum 1 pane, maximum decided by layout/performance policy

Changing modes:

- changing mode must not destroy image objects
- changing mode must not discard pane assignments unless the user confirms
- if the new mode supports fewer panes than currently assigned, excess panes remain in inactive pane state and can be restored
- if the new mode requires more panes, empty panes are created

## Viewport Interaction

Viewport transform:

- maintain `scale`, `pan_x`, and `pan_y` in image-space coordinates
- `Fit` computes the largest uniform scale that shows the full image inside the pane without cropping
- `100%` maps one image pixel to one framebuffer pixel after output scale is applied
- zoom is centered on the cursor position when using pointer-wheel or pinch input
- pan preserves the image-space point under the cursor while dragging
- clamp pan so the image cannot be lost completely off-screen

Pixel inspection:

- convert cursor position from pane coordinates to image pixel coordinates
- show pixel coordinate only when cursor is over image bounds
- show sampled pixel values in the image object's decoded format
- in multi-pane modes, show sampled values for each pane under the corresponding cursor position

Large images, including roughly 5568x3712 sources, must remain interactive after loading.

## Vulkan Rendering

The renderer must:

- create a Wayland Vulkan surface and swapchain
- render image objects assigned to panes as textures
- handle swapchain recreation on resize
- use scissors/clips for panels and controls
- render UI primitives and glyph atlases through Vulkan
- keep image uploads explicit and synchronized
- avoid hidden global Vulkan state

Initial implementation uploads RGB textures. Renderer interfaces keep image format metadata so direct YUV-plane rendering can be added later without changing pane or image-object state.

## Text

All UI text is shaped with HarfBuzz before rendering.

Text requirements:

- UTF-8 paths, labels, and errors
- glyph atlas for Vulkan rendering
- correct clipping and elision
- no overlapping labels in controls
- secondary font path for missing glyphs

## Encode Runs

An encode run is one invocation of one codec backend with one parameter set against one source image. The term does not imply offline batch behavior: a hardware encode may complete quickly enough to feel interactive, while AV2 or slower software settings may take much longer.

Encode runs must have visible states:

- queued
- running
- completed
- failed
- canceled

State behavior:

- `queued`: run has been requested but no worker has started it
- `running`: backend invocation is active
- `completed`: encoded bytes and any decoded preview/metrics are available
- `failed`: backend invocation ended with an error
- `canceled`: user canceled before completion, or cancellation was requested and the backend stopped

Failures must include:

- backend name
- operation
- concise technical error

The app must not silently switch from a selected hardware backend to a software backend. If replacement is offered later, it must be explicit and visible.

## Results

For each encoded result, show:

- backend/codec
- selected key parameters
- encoded byte size in bytes
- encoded byte size in KiB/MiB
- size ratio versus the decoded source buffer
- encode duration
- decoded dimensions/format when available
- PSNR for the first implementation; the metrics module API accepts additional metric records without changing result-row layout

Result size must be visible wherever results are listed, not only inside a detail panel. The result list/table must allow sorting by encoded byte size.

Encoded byte formats remain the current repo formats unless changed intentionally:

- HEVC/VVC: Annex B
- AV1/AV2: IVF

## State

Application state includes:

- image objects
- source metadata
- per-backend parameter values
- backend capability snapshots
- encode run queue and encode results
- pane assignments
- active view mode
- UI layout sizes

Encoder configuration state must be serializable.

## Code Structure

GUI code must be separated from codec implementations. Existing encoder entry points remain backend code and should not gain UI dependencies.

Proposed top-level structure:

- `app/`: application state, reducer/update logic, command routing
- `platform/wayland/`: Wayland connection, windows, input, drag-and-drop, frame callbacks
- `platform/portal/`: D-Bus/XDG desktop portal file chooser integration
- `render/`: Vulkan device, swapchain, pipelines, texture upload, glyph atlas, draw lists
- `ui/`: layout, widgets, hit testing, focus, interaction state
- `viewer/`: image objects, pane assignments, view modes, viewport transforms
- `encoders/`: adapter layer around existing `encode_*_still_image()` and `query_*_parameters()`
- `workers/`: encode-run execution, decode/metric work, result publication
- `metrics/`: PSNR and additional image metrics
- `storage/`: output naming, output writing, settings/config serialization

The existing codec files remain usable by the current CLI/batch path.

## Core Data Types

Use stable ids rather than raw pointers across UI state:

- `ImageId`
- `PaneId`
- `EncodeRunId`
- `BackendId`
- `TextureId`

Core records:

- `ImageObject`: metadata plus CPU-side decoded image data or reference to owned decoded data
- `Pane`: image assignment, viewport transform, channel/view options, link group
- `ViewModeState`: active mode plus mode-specific parameters such as split divider position or blink interval
- `BackendInfo`: name, codec family, capability snapshot, parameter metadata
- `EncoderConfig`: backend id plus concrete parameter values
- `EncodeRun`: source image id, config snapshot, state, timestamps, output bytes/path, error, produced image id
- `GpuImageResource`: Vulkan texture/sampler allocation associated with an image id

State records are plain data except for explicit shared ownership of decoded image buffers. Vulkan, Wayland, thread, file descriptor, and D-Bus handles stay outside serializable application state.

## Ownership

Ownership rules:

- UI/application state owns ids and immutable records.
- CPU image storage owns decoded pixel buffers.
- Renderer owns GPU resources and maps them to image ids.
- Worker system owns active worker threads and temporary encode/decode data.
- Platform layer owns Wayland, D-Bus, and OS handles.

No layer may keep borrowed references into mutable state across an update boundary. Long-lived cross-layer references must be ids.

## Update Flow

The GUI uses an event/update/render loop:

1. platform event arrives
2. event is converted to an application action
3. update function mutates application state or emits commands
4. commands perform side effects outside the update function
5. completed commands publish actions back into the update queue
6. renderer receives a read-only snapshot and draws

Examples of commands:

- open file chooser through portal
- load/decode image
- start encode run
- cancel encode run
- write output file
- upload image texture
- compute metric

The update function must not block on disk I/O, D-Bus calls, codec execution, or GPU synchronization.

## Rendering Architecture

UI layout produces a frame description, not direct Vulkan calls.

Rendering stages:

1. compute layout rectangles for regions, panes, and widgets
2. resolve pane assignments into image draw requests
3. shape visible text with HarfBuzz
4. build draw lists for images, UI primitives, and glyphs
5. submit draw lists through Vulkan renderer

Draw lists must be deterministic data structures that can be inspected in tests.

Renderer responsibilities:

- upload textures for image objects on demand
- keep texture lifetime tied to image ids
- render panes using scissor rectangles
- render composited modes such as split, blink, and difference
- render text from glyph atlas
- handle swapchain recreation without changing application state

## UI Architecture

The UI is immediate in behavior but backed by persistent application state.

Widget responsibilities:

- draw from state
- report user intentions as actions
- never run codecs
- never access Vulkan handles directly
- never own image pixel buffers

Interaction state includes:

- focused widget
- active pointer capture
- hovered pane/widget
- selected image id
- selected encode run id
- selected backend/config
- active pane id

Hit testing must use the same layout rectangles used for rendering.

## Encode Run Architecture

Starting an encode run creates an immutable config snapshot. Later UI parameter edits must not change an already running encode.

Encode run stages:

1. copy or reference the selected source image data safely
2. call the selected backend encode function with the config snapshot
3. write or retain encoded bytes
4. decode encoded bytes for preview when a decoder exists
5. compute metrics against compatible image objects
6. create an `EncodedResult` image object
7. publish completion action

Cancellation:

- queued runs can be removed before start
- running runs receive a cancellation request
- if a backend cannot stop early, UI must show cancellation requested until the backend returns
- cancellation must not corrupt app state or partially replace a result

## Capability Query Architecture

Capability queries are backend responsibilities exposed through the encoder adapter layer.

Rules:

- query capabilities before returning capability-dependent UI controls
- cache capability snapshots with backend id and implementation string
- invalidate cache when backend implementation/device changes
- do not show controls for unsupported capability bits
- show unavailable/degraded backend state when capability query fails

VAAPI-specific capability querying belongs in the VAAPI adapter, not in generic UI code.

## Persistence

Serializable settings:

- last used import/export directory
- encoder parameter presets
- selected backend list
- pane layout and view mode
- UI region sizes

Do not serialize:

- Vulkan handles
- Wayland objects
- D-Bus handles
- active worker threads
- raw output bytes unless explicitly saving a project format later

Configuration files must be local and human-inspectable where practical.

## Error Model

Errors are data, not modal-only side effects.

Each recoverable error record includes:

- stable id
- severity
- operation
- backend or subsystem
- short message
- optional technical detail
- optional related image/run id

Errors may be shown inline near controls, in result rows, and in an error log panel. A failure in one backend must not invalidate unrelated image objects or panes.

## Concrete Initial File Layout

Initial implementation creates these files before adding feature code:

- `gui/main_gui.cpp`: program entry point, initializes platform/app/renderer, owns main loop
- `gui/app_state.hpp`: ids, records, app state, actions, commands
- `gui/app_update.cpp`: reducer/update implementation
- `gui/app_commands.cpp`: side-effect command dispatcher
- `gui/layout.hpp`
- `gui/layout.cpp`: region layout and widget rectangle computation
- `gui/viewer_model.hpp`
- `gui/viewer_model.cpp`: pane/mode logic and viewport transforms
- `gui/encoder_adapters.hpp`
- `gui/encoder_adapters.cpp`: registry for existing codec backends
- `gui/encode_runner.hpp`
- `gui/encode_runner.cpp`: worker queue and encode-run execution
- `gui/metrics.hpp`
- `gui/metrics.cpp`: PSNR and image compatibility checks
- `gui/platform_wayland.hpp`
- `gui/platform_wayland.cpp`: Wayland setup, input, frame callbacks, drag/drop
- `gui/platform_portal.hpp`
- `gui/platform_portal.cpp`: D-Bus portal file chooser
- `gui/render_vulkan.hpp`
- `gui/render_vulkan.cpp`: Vulkan instance/device/swapchain/command submission
- `gui/render_draw_list.hpp`: draw-list structs shared by UI and renderer
- `gui/text_shaper.hpp`
- `gui/text_shaper.cpp`: HarfBuzz/FreeType glyph shaping and atlas input

Keep this GUI code under a `gui/` directory until there is enough structure to split into subdirectories.

## App State Schema

The first implementation uses a single `AppState` record with explicit containers:

```cpp
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
    InteractionState interaction;
    uint64_t nextId = 1;
};
```

Id allocation:

- ids are monotonically increasing `uint64_t`
- id value `0` is invalid
- ids are never reused during one process lifetime
- serialization may preserve ids, but import into an existing session must remap on conflict

Selection state:

```cpp
struct SelectionState {
    ImageId selectedImage = {};
    EncodeRunId selectedRun = {};
    BackendId selectedBackend = {};
    PaneId activePane = {};
};
```

Layout state:

```cpp
struct LayoutState {
    bool imageListCollapsed = false;
    bool inspectorCollapsed = false;
    float imageListWidth = 280.0f;
    float inspectorWidth = 360.0f;
    float commandBarHeight = 40.0f;
    float statusBarHeight = 28.0f;
};
```

## Image Object Schema

```cpp
enum class ImageObjectType {
    Source,
    EncodedResult,
    Derived,
};

struct ImageObject {
    ImageId id;
    ImageObjectType type;
    std::string displayName;
    int width = 0;
    int height = 0;
    PixelFormat pixelFormat;
    ColorMetadata color;
    std::shared_ptr<const RawImage> decoded;
    std::optional<EncodedMetadata> encoded;
    std::vector<ImageId> parents;
};
```

Rules:

- `decoded` is required for displayable image objects
- `EncodedResult::encoded` is required and includes byte size
- `DerivedImage::parents` must contain the source image ids used to compute it
- source import creates exactly one `SourceImage` for the current source
- replacing the source clears encode runs and encoded/derived image objects after the new source decodes successfully

Encoded metadata:

```cpp
struct EncodedMetadata {
    BackendId backend;
    EncodeRunId run;
    std::string codecName;
    std::string backendName;
    std::vector<std::byte> bytes;
    uint64_t byteSize = 0;
    double encodeSeconds = 0.0;
    std::filesystem::path outputPath;
    std::vector<ParamSummary> keyParams;
    std::optional<double> psnrY;
    std::optional<double> psnrRgb;
};
```

The UI must read encoded size from `EncodedMetadata::byteSize`, not from file system stat calls, because output bytes may exist before saving to a final user-selected path.

## Pane And View Mode Schema

```cpp
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
};
```

Pane order is mode input. A mode reads panes from `paneOrder`; it must not infer panes from image role.

Mode algorithms:

- `Single`: use `paneOrder[0]`; if missing, create one empty pane
- `SideBySide`: lay out all panes in `paneOrder` as columns until 4 panes, then switch to grid-like rows
- `Split`: use first two panes; draw first pane clipped left of divider and second pane clipped right of divider
- `Blink`: use first two panes; choose visible pane by `floor(time / interval) % 2`
- `Difference`: use first two panes; if no matching derived image exists, emit command to compute one; display derived image in one generated display pane
- `Grid`: choose columns as `ceil(sqrt(n))`, rows as `ceil(n / columns)`

Mode transition algorithm:

1. keep all existing panes
2. set `viewMode.kind`
3. if `paneOrder` has too few panes for the new mode, append existing unlisted panes or create empty panes
4. if `paneOrder` has too many panes for the new mode, keep excess panes in state but ignore them for rendering
5. never delete image objects
6. never change pane image assignment unless the action explicitly says so

## Actions

The update function consumes only actions. Initial action names:

```cpp
enum class ActionKind {
    RequestOpenSource,
    OpenSourceCanceled,
    SourcePathChosen,
    SourceLoaded,
    SourceLoadFailed,

    SelectImage,
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

    SelectBackend,
    SetEncoderParam,
    RefreshBackendCapabilities,
    BackendCapabilitiesReady,
    BackendCapabilitiesFailed,

    StartEncodeRun,
    EncodeRunQueued,
    EncodeRunStarted,
    EncodeRunCompleted,
    EncodeRunFailed,
    CancelEncodeRun,
    EncodeRunCanceled,

    SaveEncodedResult,
    SaveCompleted,
    SaveFailed,

    PointerMoved,
    PointerPressed,
    PointerReleased,
    PointerScrolled,
    KeyPressed,
    WindowResized,

    ErrorDismissed,
};
```

Action payloads must be structs, not untyped maps. Actions that originate from worker/platform threads are posted into the same update queue as UI actions.

## Commands

The update function emits commands for side effects:

```cpp
enum class CommandKind {
    ShowOpenFilePortal,
    LoadSourceImage,
    QueryBackendCapabilities,
    RunEncode,
    RequestEncodeCancel,
    DecodeEncodedBytes,
    ComputeMetric,
    ComputeDerivedImage,
    UploadImageTexture,
    SaveBytesToFile,
    RequestRedraw,
};
```

Command execution rules:

- commands run outside `update(AppState&, Action)`
- each command must eventually publish an action or be explicitly fire-and-forget
- command failures become action payloads, then `AppError` records
- command handlers may read immutable snapshots but must not mutate `AppState` directly

## Layout Algorithm

Given framebuffer size `W x H` and output scale `S`:

1. convert all layout constants to physical pixels by multiplying by `S`
2. assign `CommandBar` rectangle: `(0, 0, W, commandBarHeight)`
3. assign `StatusBar` rectangle: `(0, H - statusBarHeight, W, statusBarHeight)`
4. remaining content rectangle is between command and status bars
5. assign `ImageList` from left side unless collapsed
6. assign `Inspector` from right side unless collapsed
7. assign `Viewer` to the remaining central rectangle

Minimum sizes:

- `ImageList` expanded minimum: 220 logical px
- `ImageList` collapsed width: 48 logical px
- `Inspector` expanded minimum: 300 logical px
- `Inspector` collapsed width: 48 logical px
- `Viewer` minimum: 480 logical px wide and 320 logical px high

If the window is too narrow:

1. collapse `Inspector`
2. collapse `ImageList`
3. keep `Viewer` and `StatusBar` visible
4. if still too narrow, allow horizontal clipping inside side panels, not inside `Viewer`

Pane layout uses the `Viewer` rectangle only. Panes never overlap `CommandBar`, `ImageList`, `Inspector`, or `StatusBar`.

## Widget Set

Initial widgets:

- `Button`
- `IconButton`
- `Toggle`
- `Checkbox`
- `SliderInt`
- `SliderFloat`
- `NumericInput`
- `Dropdown`
- `TextInput`
- `TableList`
- `Splitter`
- `PaneView`
- `ProgressRow`
- `InlineError`

Every widget must expose:

- stable widget id for focus/hit testing
- layout rectangle
- enabled/disabled state
- hover/active/focus state
- draw-list generation
- input handling that emits actions

Text in controls must be clipped or elided. Numeric values such as byte size, QP, PSNR, and dimensions must not be hidden by decorative labels.

## Encoder Adapter Interface

Every backend adapter implements:

```cpp
struct EncoderBackend {
    BackendId id;
    std::string name;
    std::string codec;
    BackendKind kind;
    bool hardware = false;

    CapabilityResult queryCapabilities();
    std::vector<EncoderParamInfo> queryParameters(const CapabilitySnapshot&);
    EncodedImage encode(const RawImage&, std::span<const EncoderParam>);
    DecodeResult decodePreview(const EncodedImage&);
    std::vector<ParamSummary> summarizeParams(std::span<const EncoderParam>);
};
```

Rules:

- `queryParameters()` receives capabilities so it can omit unsupported controls
- backend adapters can wrap existing functions directly
- backend adapters convert backend exceptions into `EncodeRunFailed` action payloads
- backend adapters must not include UI layout or widget code
- hardware adapters must expose implementation identity and unavailable state

Initial backend ids:

- `x265_hevc`
- `vvenc_vvc`
- `svt_av1`
- `vaapi_hevc`
- `vaapi_av1`
- `uvg266_vvc`
- `av2`

## Encode Runner Protocol

The encode runner owns a FIFO queue and a fixed worker count.

Initial policy:

- one encode worker thread
- one decode/metric worker thread
- hardware backends still use the encode worker, even if they complete quickly
- encode runs are serialized initially to avoid CPU/GPU oversubscription surprises

Run lifecycle:

1. UI emits `StartEncodeRun`
2. update creates `EncodeRun{Queued}` and emits `RunEncode`
3. runner posts `EncodeRunStarted`
4. backend runs encode
5. runner records encoded byte vector and duration
6. runner posts decode/metric command if preview is supported
7. final action is `EncodeRunCompleted` or `EncodeRunFailed`

Threading rules:

- worker threads must not touch `AppState`
- worker threads must not touch Vulkan or Wayland objects
- worker threads can call codec backend functions
- worker thread results are moved into actions
- large byte vectors are moved, not copied

## Texture Upload Protocol

CPU decoded image availability and GPU texture availability are separate.

Flow:

1. `ImageObject` is created with CPU decoded pixels
2. update emits `UploadImageTexture`
3. render system uploads through staging buffer
4. render system records `TextureId` for `ImageId`
5. frame rendering uses placeholder until texture is ready

Texture invalidation:

- deleting an image object releases the texture
- replacing source releases textures for source, encoded results, and derived images that are removed
- swapchain recreation must not destroy image textures unless the Vulkan device is recreated

## Difference Image Protocol

Difference is modeled as a `DerivedImage`, not a special pane-only effect.

Creation rules:

- inputs must have same dimensions
- inputs must be in compatible display color space
- if formats differ, convert into a common float or 16-bit working representation
- output display name includes both parent image names
- output metadata stores parent ids and gain

Difference pixel formula for initial implementation:

```text
out = clamp(abs(a - b) * gain)
```

The `Difference` view mode requests a derived image automatically when the required derived image does not already exist. The produced `DerivedImage` remains assignable to any pane.

## Metrics Protocol

Initial metric is PSNR.

PSNR rules:

- compare decoded images after aligning dimensions and pixel format
- if dimensions differ, metric is unavailable with a visible reason
- report at least one luma-like metric for YUV images
- report RGB/global metric only when conversion is defined
- store metric on the encoded result metadata, not only in UI

Metric failures must not fail the encode run if encoded bytes were produced.

## Import Protocol

Opening a source image:

1. user requests import
2. portal command opens file chooser
3. portal returns URI/path or cancellation
4. loader command decodes path into `RawImage`
5. update removes old source-dependent images and runs
6. update creates `SourceImage`
7. update ensures at least one pane exists and assigns source to first pane
8. update requests texture upload and redraw

Drag-and-drop and `argv` skip the portal step and enter at path decoding.

Import failure:

- no image objects are replaced
- error record references import operation
- UI remains usable

## Save And Export Protocol

Encoded bytes may exist before user export.

Save rules:

- result rows show byte size immediately after encode completion
- default filename is generated from source stem, backend id, and key settings
- saving prompts through portal or explicit local path depending on platform support
- existing files require confirmation unless overwrite policy is explicit
- saving failure does not delete the encoded result from app state

## Logging And Diagnostics

Keep diagnostics local and inspectable:

- stderr receives startup and fatal platform errors
- recoverable runtime errors become `AppError`
- debug log panel shows recent actions and command completions when debug UI is enabled
- VAAPI capability snapshots are printable in a debug view

No diagnostic path may require network access.

## Pure Function Interfaces

Implement these pure functions before platform or Vulkan work:

```cpp
UpdateResult update(AppState state, const Action& action);

LayoutResult compute_layout(
    const LayoutState& layout,
    int framebufferWidth,
    int framebufferHeight,
    float outputScale
);

std::vector<PaneRect> compute_pane_rects(
    const ViewModeState& mode,
    std::span<const Pane> panes,
    Rect viewerRect
);

ViewportTransform fit_transform(
    int imageWidth,
    int imageHeight,
    Rect paneRect,
    float outputScale
);

std::optional<ImagePixelCoord> pane_to_image_coord(
    const Pane& pane,
    const ImageObject& image,
    Rect paneRect,
    Point pointer
);

std::vector<DrawCommand> build_draw_list(
    const AppStateSnapshot& state,
    const LayoutResult& layout,
    const ResourceSnapshot& resources,
    double timeSeconds
);
```

Rules:

- pure functions take all inputs as parameters
- pure functions do not read globals
- pure functions do not call Wayland, Vulkan, D-Bus, encoders, or file I/O
- pure function tests use fixed input records and compare deterministic output records

## Reducer Behavior

Reducer output is:

```cpp
struct UpdateResult {
    AppState state;
    std::vector<Command> commands;
};
```

Required action behavior:

- `RequestOpenSource`: emits `ShowOpenFilePortal`
- `OpenSourceCanceled`: leaves state unchanged except clearing pending import UI state
- `SourcePathChosen`: emits `LoadSourceImage(path)`
- `SourceLoaded`: removes old source-dependent images/runs, creates source image, creates/assigns first pane, selects source, emits `UploadImageTexture`
- `SourceLoadFailed`: keeps previous images/runs, appends `AppError`
- `AssignImageToPane`: validates ids, sets pane image id, selects pane and image, emits `UploadImageTexture` if texture missing
- `SetViewMode`: applies mode transition algorithm, emits `RequestRedraw`
- `FitPaneToImage`: computes fit transform if pane has image, updates pane transform
- `SetEncoderParam`: updates editable encoder config only; does not mutate existing encode runs
- `StartEncodeRun`: snapshots selected source image id and encoder config, appends queued run, emits `RunEncode`
- `EncodeRunStarted`: sets matching run state to running and records start timestamp
- `EncodeRunCompleted`: sets run state to completed, creates `EncodedResult`, records byte size, emits texture upload for preview image if present
- `EncodeRunFailed`: sets run state to failed and appends `AppError`
- `CancelEncodeRun`: if queued, marks canceled; if running, emits `RequestEncodeCancel`
- `SaveEncodedResult`: validates encoded bytes exist and emits `SaveBytesToFile`
- `WindowResized`: updates platform/window size and emits `RequestRedraw`

Invalid ids:

- invalid ids do not crash
- invalid ids append warning-level `AppError`
- invalid ids do not partially mutate state

## Draw Command Schema

Draw lists contain only renderer-neutral commands:

```cpp
enum class DrawCommandKind {
    Rect,
    Border,
    Text,
    Image,
    ScissorBegin,
    ScissorEnd,
};

struct DrawCommand {
    DrawCommandKind kind;
    Rect rect;
    Color color;
    std::string text;
    ImageId image = {};
    TextureId texture = {};
    Rect uvRect;
    float opacity = 1.0f;
};
```

Rules:

- UI code emits `ImageId`; renderer resolves to `TextureId`
- missing texture draws a placeholder rectangle with loading/error text
- all pane image draws are enclosed in scissor commands
- text commands store shaped glyph references after text shaping stage, not raw strings in the final renderer submission

## Build Integration

Add a GUI target without breaking existing CLI builds.

Required targets:

- existing CLI/batch binary remains buildable
- new GUI binary: `codec_vis_gui`
- pure unit test binary for app/layout/viewer logic

Dependencies for GUI target:

- Wayland client
- wayland-protocols generated `xdg-shell` bindings
- Vulkan loader and headers
- HarfBuzz
- FreeType
- D-Bus client library for portal integration
- existing codec dependencies

Build rules:

- GUI files compile without including platform headers from app/model headers
- `gui/app_state.hpp`, `gui/viewer_model.hpp`, and `gui/layout.hpp` remain platform-independent
- codec backend adapters include existing codec headers, but generic UI files do not
- renderer files are the only files that include Vulkan headers
- Wayland files are the only files that include Wayland headers
- portal files are the only files that include D-Bus headers

## Implementation Order

Implementation proceeds in this order:

1. define ids, records, actions, commands in `gui/app_state.hpp`
2. implement pure reducer with unit tests for pane/mode actions
3. implement layout rectangles without Vulkan
4. implement Wayland window and Vulkan clear
5. implement text shaping and basic draw lists
6. render static shell: CommandBar, ImageList, Viewer, Inspector, StatusBar
7. implement import via `argv`
8. implement texture upload and single-pane image display
9. implement portal file chooser
10. implement pane assignment and view modes
11. implement encoder adapter registry
12. implement one encode backend end to end
13. add result rows with byte size
14. add preview decode and PSNR
15. add difference derived images
16. add remaining backends

Do not implement encoder controls before the app can display source image panes; otherwise the UI will drift back into being an encoder form instead of an image comparison tool.

## Acceptance Checks

Minimum acceptance for first usable GUI:

- starts under Wayland and opens one window
- renders shaped text through HarfBuzz path
- imports an image from `argv`
- imports an image through the D-Bus file portal
- displays source image in a pane
- supports fit, 100%, pan, and zoom
- lists source image in `ImageList`
- queries backend parameter metadata
- queries VAAPI capability-dependent controls before showing them
- starts one encode run
- shows encode run state transitions
- shows encoded byte size in result row
- creates an `EncodedResult` image object
- assigns encoded result to a second pane
- compares source and result in side-by-side mode
- computes and displays PSNR when compatible
- saving encoded bytes writes the same bytes represented by result metadata

Regression checks:

- changing view mode does not delete image objects
- assigning images to panes does not create image copies
- editing encoder parameters does not mutate already running encode config snapshots
- failed encode does not remove source image or completed results
- source replacement clears dependent results only after successful decode
- no UI action blocks the render loop on codec execution
- no unsupported VAAPI rate-control mode is shown

## Non-Goals

- video timeline
- video playback
- frame sequence editing
- network import/export
- plugin system
- cross-platform GUI abstraction
- theme editor

## Milestones

1. Wayland/Vulkan/HarfBuzz bootstrap:
   - open a window
   - clear and resize swapchain
   - render shaped text

2. Static image viewer:
   - load an existing supported image
   - upload and draw the texture
   - pan, zoom, fit, and sample pixels

3. Encoder inspector:
   - query `EncoderParamInfo`
   - query backend capabilities where required
   - render image-relevant controls only

4. Encode runs:
   - run one backend on a worker thread
   - show status and errors
   - write output bytes

5. Comparison:
   - load/decode output preview
   - side-by-side and split views
   - display size and PSNR
