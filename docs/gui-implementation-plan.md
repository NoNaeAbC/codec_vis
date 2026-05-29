# Codec Vis GUI Implementation Plan

This plan tracks the remaining implementation work against `docs/gui-requirements.md`.

## Verification Loop

Every implementation slice must finish with:

- focused unit tests for the touched model/layout/draw/interaction code
- `make test`
- `make codec_vis_gui`
- `timeout 5 ./codec_vis_gui --smoke-frames 3` under the real Wayland/Vulkan session

## Current Completed Slices

- Native Wayland `xdg-shell` window and Vulkan renderer.
- HarfBuzz/FreeType text shaping and glyph atlas path.
- Static shell layout: command bar, image list, viewer, inspector, status bar.
- Source import through launch path and desktop portal command path.
- Wayland drag-and-drop for `text/uri-list` local files.
- CPU decoded image objects, pane assignments, and mode state.
- Fit, 100%, pan, zoom, split divider, blink, side-by-side, grid, and difference view behavior.
- Encoder backend registry and capability-derived parameter lists.
- VAAPI backend identity and rate-control choices from queried capabilities.
- Encode run queue, state transitions, cancellation request state, result image creation.
- Result rows with exact bytes, human-readable size, backend name, PSNR, and sorting.
- Save/export command path through portal and explicit overwrite policy for local writes.
- Difference images as assignable derived image objects, including parent ids and gain.
- Format-mismatched difference inputs converted to a common luma representation.
- Debug log panel for recent actions, emitted commands, and capability snapshot summaries.
- Selected encode-run details in the Inspector.
- Deterministic tests for Wayland dropped URI parsing and percent-decoding.
- Deterministic tests for desktop portal URI/path decoding, including percent-decoding and cancellation-safe plain paths.
- Linked pane transforms for panes in the same link group.
- Encode execution split into one encode worker plus one decode/metric worker.
- Encode run start and finish timestamps recorded in run state.
- Explicit per-pane assignment controls on image rows.
- Preview decode and PSNR unavailability reasons are stored with encoded metadata and shown in result details.
- Source load failure keeps the previous source, dependent results, and encode runs until a new source load succeeds.
- Pane assignments survive transitions into restrictive modes and are restored when returning to multi-pane modes.
- Encoder controls use type-specific UI behavior and visuals for toggles, bounded sliders, segmented enum selection, and focused text inputs.
- Backend capability snapshots include printable parameter/range/enum detail lines and the debug panel displays those details.
- Image rows expose an explicit difference action that creates a derived image from the selected image and any other decoded image object.
- Settings persist the selected backend, keep debug UI disabled by default, and remap conflicting pane ids when imported into a nonempty session.
- Draw-list tests assert every image draw is bracketed by scissor commands.
- End-to-end command/reducer test covers encode completion, result image creation, pane assignment, and exact byte save.
- Text atlas tests assert long text glyph quads are clipped to their text command rects and elided when they overflow.
- Inspector controls visibly mark backend identity fields as identity-only and capability-derived enum controls with no choices as unavailable.
- Vulkan smoke supports an explicit pixel readback probe and has been verified with a real JPEG source path.
- `docs/gui-acceptance-checks.md` maps each documented acceptance check to deterministic tests or a manual smoke command with expected output.
- Text shaping has a deterministic secondary font path when the configured primary UI font is missing.
- Active encode status is mirrored in the always-visible status bar, with draw-list coverage when both side panels are collapsed.
- Portal response handling is isolated and unit-tested for cancellation, empty success, and returned URI actions.
- Per-glyph font substitution is used when the primary font returns missing glyphs, with glyph atlas cache keys including font identity.
- Live desktop portal smoke is documented with exact expected state changes.
- Actual draw-list coverage checks long backend labels, long technical errors, exact byte sizes, and PSNR values through the text atlas clipping path.

## Completion Audit

- Platform requirements are covered by the native Wayland window, Vulkan renderer, HarfBuzz/FreeType text path, and no web or network runtime dependency.
- First-screen and layout requirements are covered by the command bar, image list, viewer, inspector, status bar, collapsible side panels, status-bar active run mirroring, and draw-list clipping/elision tests.
- Image acquisition requirements are covered by launch-path loading, desktop portal command/response handling, Wayland drag-and-drop URI parsing, and the shared source decode/update path.
- Encoder model and capability requirements are covered by backend adapters, capability snapshots, VAAPI rate-control choices derived from queried bits, implementation identity display, unavailable controls, and still-image-only parameter filtering.
- Image object, pane assignment, and mode requirements are covered by stable ids, independent pane/image/mode state, explicit pane assignment controls, side-by-side/split/blink/difference/grid rendering, linked transforms, and mode-transition preservation tests.
- Viewport requirements are covered by fit, 100%, pan, zoom, cursor-to-image coordinate conversion, sampled pixel status text, and linked comparison deltas.
- Rendering and text requirements are covered by deterministic draw lists, scissored image draws, Vulkan texture upload/readback smoke, glyph atlas rendering, UTF-8 secondary-font shaping, and clipped/elided text tests.
- Encode-run and result requirements are covered by queued/running/completed/failed/canceled states, immutable config snapshots, cancellation state, worker-thread execution, result image creation, byte-size/ratio/duration/PSNR display, and exact-byte save tests.
- Persistence, error, diagnostics, and architecture requirements are covered by settings serialization/remap tests, `AppError` records, debug capability/log panel, pure reducer/layout/viewer functions, command side-effect separation, and build targets.
- Acceptance checks are mapped in `docs/gui-acceptance-checks.md` and currently pass through `make test`, `make codec_vis_gui`, and the Wayland/Vulkan pixel smoke.
