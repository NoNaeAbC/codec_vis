# GUI framework and renderer

The application now uses `workbench.hpp` / `workbench.cpp` and
`ui_framework.hpp` for its actual frontend. The old toolbar/list/inspector
implementation remains as a compatibility test fixture; `main_gui.cpp` no
longer draws it or routes pointer events through it. `framework_lab.cpp` is a
separate synthetic stress fixture, not the product.

## Comparison workbench

- The toolbar contains import, source context, independent Sources/Tools/Results
  toggles, and focus. The in-content application-name label and permanent
  navigation tutorial are removed. Navigation help is attached to the view-mode
  control; the footer retains contextual sample readouts and the display interpretation.
- Viewed images carry run/codec/size/format context, with decisive settings and
  complete configuration available on inspection. The encoder identifies a new
  draft for the current source; copying a recorded configuration names its run.
- Quality columns put scores next to size and decisive settings. Speed columns
  expose decoder/timing/failure data; All columns and Measurements retain the
  statistical detail. The Metrics inspector shows numerical scores for selected
  runs, with metric definitions behind a disclosure and an explicit original-source
  reference. Neither switching views nor inspecting the prepared input starts metrics.
- Completed encodes retain shared immutable application-prepared input samples.
  Compare prepared input / decoded assigns those samples to A and decoded output
  to B, preserving score reference and result selection. This is the input to the
  encoder adapter; adapters can perform additional format conversion. Codec-only
  losslessness cannot be inferred from source-to-result scores. Configuration
  records source/prepared/decoded formats and the prepared sample color description.
- Plots can expand independently, expose candidate settings and exact values on
  hover, and offer an explicit View in B action after selecting a point. Selection
  remains independent from pane assignment. Active SCC prevents choosing a chroma,
  depth, or rate-control option that would make the SCC setting ineffective.

- Resizable source rail, image canvas, task tools, and bottom encode queue.
  Imports add sources without clearing existing encodes. Multiple source
  paths can also be passed to the GUI at startup.
  HEIF containers (`.heif`, `.heic`, `.hif`, `.avif`, `.vvic`) import their primary coded
  image using in-process libheif 1.23 or newer. Content detection works independently
  of the filename. Grid primary images are rejected explicitly; other image types use the installed
  libheif decoders.
  Native planar chroma/precision and NCLX color are retained; the decoder's default
  sRGB conversion and HDR-to-8-bit conversion are disabled. Container crop/rotation/
  mirroring are applied. Embedded ICC profiles use the shared import color transform.
  RGB precisions that lack a native application format expand reversibly to 16-bit
  storage, with a visible note; alpha is not retained and is also noted. The original
  container remains available for repeated decoding through the libheif decoder.
  Codec availability depends on the installed libheif decoders; portable bundles
  include its plugin directory and dependencies.
- Sources and queue rows have direct A/B assignment buttons. Add C and D for
  up to four independent panes. Row selection inspects settings and decoder data
  without reassigning images; imports preserve assignments. New encode results
  enter the active pane. A and B have equal roles. Fit, actual pixels,
  linked/unlinked navigation, split wipe, blink, difference, single-pane,
  and selected-result grid views are available. Difference compares the
  maximum absolute RGB delta in floating-point linear BT.2020, accounting for
  each image’s color interpretation. Difference is drawn directly from the two
  FP32 Vulkan textures, without a CPU image job or integer heatmap. The FP16 surface preserves
  HDR and gamut excursions; presentation is independent of quality scores. Space temporarily shows A; F
  enters focus mode; 0 fits and 1 selects actual display pixels. Inspection
  bookmarks have editable names and retain both independent viewport positions
  and the linked-navigation setting for the current session. Switching comparison
  modes preserves a manually chosen crop. Pointer navigation uses the exact
  geometry of the displayed pane, including lower grid tiles.
  The selection grid pages through A plus three candidates at a time; clicking
  a tile inspects its image without changing assignments or clearing the queue selection.
- Encode offers an implementation selector, direct numeric entry, contextual
  validation, search, pinned settings, optional help, core controls and
  collapsible specialist groups. Submission captures the effective requested
  settings. Cloning an old encode restores its draft without modifying it.
  Invalid numeric drafts block submission. SCC is a core control where offered.
- VA-API and JPEG XS offer live feedback while quality controls are dragged or
  edited. Bitrate and JPEG XS bpp sliders use logarithmic spacing and retain
  direct numeric entry; QP/CRF remain linear. A fixed size row reports actual
  encoded size and bits per pixel for the displayed frame. Live pane captions
  also include its encoded size. A live frame is separate from the queue:
  **Encode** retains an independent result for measurements and pane assignment.
  The inspector identifies the settings actually shown and separates encoding
  time, decoding time, and queued pipeline latency.
  Live B remains visible on every grid page. Metrics remain explicitly requested.
- VA-API CBR/VBR expose **Target image size (kB)**: decimal kB (1000 bytes),
  converted to a bit budget at 1 fps. For example, 250 kB = 250000 bytes =
  2000000 bits. Legacy kbps configurations are converted with kB = kbps / 8.
  VBR now requests 100% of this budget, removing the previous hidden 66% target.
  The logarithmic size slider initially spans one quarter to four times the
  target; Full range / Focus range changes its domain without encoding anything.
  Direct numeric entry retains the complete supported range.
  The live inspector reports the shown frame's target, actual size in the same
  decimal units, and actual/target percentage. These remain tied to the displayed
  frame while newer edits are pending. A driver can undershoot on its first
  frame, and its quantizer limits can produce a size floor. This control does
  not promise exact-size output or add application-side padding/retry encodes.
- The virtualized queue has stable run IDs, numeric size/time/metric
  sorting, filtering, multiple selection and resizable columns. Configuration
  compares recorded settings across selected runs. Source configuration shows
  import interpretation and color metadata. Requested settings and actual
  decoded formats are distinguished from unverified encoder internals. The table
  shows the latest measured decoder, codec mean, sample SD, total mean and attempt/failure
  counts, with numerical sorting. Previous decoder sessions remain available in
  Measurements and are not pooled. Filtered-out selections are counted explicitly
  and can be cleared without changing pane assignments.
- Decode independently selects verified compatible decoders. Repeated attempts,
  cancellation, codec and total time, sample SD/SE, and raw energy retain their
  existing worker semantics. The wide Measurements table retains every attempt
  across decoder sessions, sorts individual attempts and lets a selected
  attempt expose its session statistics. Sessions are never pooled. Energy
  remains an explicit session authorization with its counter coverage disclosed.
  Measurement never replaces the established decoded image or invalidates its
  quality scores, including scores on images derived from a measured source.
  Native software adapters report completed decoder API work, with application
  color conversion and pixel repacking outside the codec timer. Hardware timers
  wait for actual GPU completion before stopping, with readback counted in total
  time. Import adapters forward timing through the shared format dispatch path.
- **Compute metrics** requests every admitted adapter for selected queue rows;
  subsequent requests keep existing scores and retry failures. Numerical scores
  appear in the Metrics inspector and Quality columns; definitions are available
  through the methodology disclosure. Not-requested, queued, running and failed values remain
  distinct. Pane assignments do not change the end-to-end metric
  reference. Eligibility counts distinguish already computed scores, active
  metrics, active decodes, and runs without a decoded result. Selecting a failed
  run never falls back to computing scores for a previously displayed result.
  The plot uses available finite scores only and selects a result for inspection on click;
  it never implicitly starts computation.
- Replacement removes superseded result rows and follows replacements in every
  assigned pane. Old run provenance retains replacement links. Active metrics,
  active decode measurements, dependent images, and the transient live image
  are preserved.
- Numeric display uses fixed decimal precision chosen from the parameter step;
  text editors own placeholder alignment and reset obsolete horizontal offsets.
  Live encode/decode/pipeline times use fixed, nonwrapping rows. Short enums use
  selected-state buttons; long labels and larger lists use dropdowns. Group
  headings extend beyond the indented child controls.
- Worker completions preserve a newer user selection. Difference drawing uses
  the current A/B texture revisions, including after an independent re-decode.
  Display P3 HEVC previews retain encode color context for fields libde265
  reports as unspecified; a regression checks the actual signaled bitstream
  independently through libavcodec without that context.
- Bookmarks and panel layout are session-local. No workspace persistence,
  IME, or AT-SPI bridge is implemented in this migration.
  Image presentation remains SDR, as described below.

The 2026-10-17 controls review exercised the actual GUI at 2880×1800:
x265 CRF dragging, VA-API QP and JPEG XS bpp dragging, 12-bit/4:4:4 enum
buttons, clearing an overflowing search editor, A/B/C/D assignment, and
batch metric computation. Two retained JPEG XS results were replaced by one
new queue row; A/B/C followed the replacement while D kept the source.
A GPU observer detected changing bpp digits and no changes in the fixed group
header region during a three-second drag. The focused workbench, navigation,
framework, model, live-preview and encode-runner suites all passed.

## What the application already uses

- Unused areas support native window dragging. After UI and canvas input routing,
  an unconsumed left press requests `xdg_toplevel.move` with the original Wayland
  seat and button-event serial. The compositor owns the movement. Releases and
  stale serials cannot start a move. Blank scroll/table backgrounds and disabled
  controls leave the press available; active controls, canvas gestures, and popup
  dismissal consume it. These areas remain in the surface input region.
- One paint scene for image, shape and glyph drawing. Nested clips intersect;
  integer layers are stable and unrestricted. Painter order is preserved within
  a layer, including text interleaved with images and opaque controls.
- Compute-only Vulkan rendering tiles adjacent glyphs/shapes with the same clip.
  CPU tile lists retain painter order, and one compute invocation blends each
  pixel in order. Image batches sample the FP32 textures directly, including
  differences. No vertex or fragment shaders, graphics pipelines, render passes,
  or draw commands are used. Batching never reorders across an image or overlay.
- Persistent host-visible instance buffers and a persistent glyph atlas. Glyphs
  are rasterized once per font/size and atlas lifetime. GPU atlas allocation is
  reused while its dimensions remain constant; unchanged alpha data is not
  uploaded. Text supports measured wrapping, alignment, elision, sizes and a
  monospace role. Font scale changes invalidate the cache.
- FP32 linear BT.2020 image textures, with no 8-bit preview intermediate. The
  renderer uploads original decoded planes and uses a Vulkan compute kernel for
  signal unpacking, location-aware bilinear chroma reconstruction, EOTF/HLG OOTF,
  and the source-primaries-to-BT.2020 matrix. Matrix coefficients are shared with
  the FP64 CPU reference. Negative/HDR values are preserved and non-finite
  conversions are rejected. A device-storage-range limit falls back to the CPU
  FP32 path for oversized sources, preserving supported image sizes.
  Source staging and same-sized preview textures are reused between updates. The
  compute compositor writes an FP16 linear BT.2020 pass-through swapchain,
  without converting decoded pixels to BT.709, transfer encoding, tone mapping,
  or clamping RGB. UI palette colors are converted into BT.2020 before blending.
  Wayland color-management-v1 declares BT.2020 primaries and extended linear
  transfer only. The application sends no luminance, reference-white brightness,
  mastering luminance, or content-light-level requests and does not control the
  user's screen brightness. The compositor interprets the relative signal for
  the display. Protocol-defined defaults are not application brightness settings.
  Color-management and FP16 storage support are required; unsupported systems
  receive an explicit error rather than an incorrectly tagged fallback.
  Metrics and retained source samples are independent of presentation.
- Visible-image upload and an LRU cache. Inactive images are evicted to keep the
  cache within 512 MiB and 32 textures; images required by the current view may
  exceed those limits. The active A/B pair is retained even during blink or a
  temporary reference toggle, avoiding upload thrashing. Both preview revision and immutable source identity
  invalidate stale textures. Retained experiment records do not imply retaining
  every decoded image in GPU memory. CPU image lifetime remains the application
  model's responsibility. Large grid selections retain their experiment records
  while only the current four-image page and active A/B pair require GPU textures.
- One semantic widget tree for workbench drawing, focus, input and hit testing.
  Legacy toolbar definitions remain shared by their compatibility tests.
- Wayland input includes pointer buttons, modifiers, horizontal wheel movement,
  named navigation keys, key releases and focus loss. The platform also supports
  native text clipboard ownership and asynchronous paste. Clipboard transfers
  use pipes and memory; the GUI does not block on a producer/consumer.

## Framework contracts

A `ui::Context` owns transient interaction state; application data stays in the
application model. Supply a text-measurement callback using the same fonts and
physical scale as the atlas builder.

1. Deliver normalized `ui::Input` to `Context::handle` against the last committed
   frame. `input_from_action` converts native Wayland actions.
2. Call `begin_frame(bounds)` and declare controls using stable semantic IDs.
   Each declaration provides drawing geometry and the corresponding hit/focus
   geometry together. Duplicate IDs and unbalanced clips are errors.
3. Read returned activation/edit/selection results to update the application
   model. Then call `end_frame()`.
4. Build the text atlas, sync visible images, and render the resulting commands.

Widgets do not retain references to application model fields between frames.
Pointer capture survives dragging outside bounds; release outside a button
cancels activation. Disabled controls block clicks without activating. Tab and
Shift+Tab skip disabled or fully clipped widgets. Focus/capture are cleared when
controls disappear. Focus loss releases capture.

Text inputs support UTF-8 codepoint movement/deletion, selection by keyboard and
pointer, Home/End, select-all, clipboard copy/cut/paste, and horizontal scrolling.
Clipboard requests include the originating widget ID; a late paste is ignored if
focus has moved. Numeric editors retain incomplete/invalid drafts, validate on
Enter or blur, expose an inline error, and change the numerical model only on a
valid commit. Escape restores the original value. Arrow stepping is bounded.

Combo boxes support disabled choices, keyboard navigation, and clipped scrolling
menus. Their popup uses a higher layer outside its parent's scroll clip, stays
within the window, and consumes the entire outside-dismissal click sequence.

The table model supplies row count, stable row ID, ID-to-index lookup, and cell
text callbacks. Only visible rows plus overscan are requested. Sorting is a
request to the model; the framework never invents sorting for numerical strings.
Selection is stored by row ID, including Ctrl/Shift selection. Headers sort,
columns resize/hide, and both axes scroll with a fixed header. The same generic
scroll regions work inside each other, with unconsumed wheel movement bubbling
to the parent.

Dock layout supplies source/inspector/results/canvas regions and resize handles,
plus a focus mode. Track layout supports bounded fixed/flexible rows and columns.
Splitters support pointer capture and keyboard adjustment. The scatter primitive
supports labelled linear/log axes, selectable stable point IDs and clipping;
all finite in-range observations are submitted. It performs no aggregation,
outlier removal or statistical inference.

## Verification

Build and run `make test_workbench test_ui_framework test_live_preview` for
navigation, controls, clipping, validation, and worker scheduling checks.
`build/gui_framework_lab --verify` checks actual Vulkan output, FP32 texture
precision, unclipped HDR, clipping and painter order, and FP16 presentation.

The framework does not provide Wayland text-input/IME composition, full
bidirectional paragraph layout, or an AT-SPI accessibility bridge.

## Live feedback

Encoder input preparation retains one immutable source and its conversion key
per encoder worker. QP, bitrate and speed edits reuse prepared planes; source
identity, output format, color/tone-map settings or backend changes invalidate
them. VA-API device/config/surfaces are retained on the encoding worker and input
is uploaded on every encode. CBR, VBR and ICQ use a fresh context and coded buffer
for each still image: IDR pictures do not reset driver rate-control history, and
the tested Intel driver does not reliably apply changed ICQ quality to an existing
context. CQP quantizer edits reuse the context; other parameter changes recreate
it to avoid latched sequence settings. Failed sessions are discarded. The initial
VA-API preview prefers actual hardware decoding on the encoder's device with a
software fallback. Independent decoder measurements retain their own explicit
decoder selection and setup semantics. JPEG XS previews use four decoding threads.

Live encoding and decoding overlap on workers separate from retained experiments,
so a slow retained codec call cannot queue-block the interactive path. Each stage retains at most one pending live
item, replacing it when a newer edit arrives; already active results can be shown
while dragging continues. Generation/source/backend guards prevent old frames
from returning after a pause, source change or backend change. Live completions
do not create experiment rows. Explicit experiments and decoder measurement
attempts keep their existing retention semantics.

`build/test_live_preview` tests conversion reuse/invalidation, 200 edits arriving
during one held codec call, progressive display, bounded pending work, final-value
delivery, source replacement, stale completion, pause guards and independence
from a held retained encode. Workbench tests
also exercise slider-driven requests and live B in paged grid/difference views.
The native Vulkan verification compares 200 format/transfer combinations against
the FP64 reference, including every supported primary set, full/limited ranges,
chroma locations, odd strides and unclipped HDR. It checks invalid PQ rejection
and a difference amplified from adjacent 16-bit sample codes.
