# Codec Vis GUI Acceptance Checks

This checklist maps `docs/gui-requirements.md` acceptance items to current deterministic evidence.

## Minimum Acceptance

- starts under Wayland, opens one window, and presents multiple frames: `timeout 5 ./codec_vis_gui --smoke-frames 3`
- renders shaped text through HarfBuzz path: `make test_text_shaper test_text_atlas`; smoke output includes `HarfBuzz` and `Text quads`
- substitute text rendering: `make test_text_shaper`; verifies missing primary font substitution and per-glyph secondary-font substitution from a secondary face
- imports an image from `argv`: `timeout 5 ./codec_vis_gui --smoke-frames 3 --probe-image-pixels "$smoke_image"` after creating the JPEG source
- imports an image through the D-Bus file portal: `make test_platform_portal`; live portal path is `CommandKind::ShowOpenFilePortal`, and portal response handling is tested for cancellation and returned URIs
- displays source image in a pane: `make test_gui_model test_draw_list`; pixel smoke prints `Image pixel probe: nonblank`
- supports fit, 100%, pan, and zoom: `make test_gui_model test_ui_interaction`
- lists source image in `ImageList`: `make test_draw_list test_ui_widgets`
- queries backend parameter metadata: `make test_encoder_backends`
- queries VAAPI capability-dependent controls before showing them: `make test_encoder_backends`; VAAPI rate-control choices are built from queried capability parameters
- starts one encode run: `make test_app_commands test_encode_runner`
- shows encode run state transitions: `make test_gui_model test_encode_runner`
- shows encoded byte size in result row: `make test_draw_list test_app_commands`
- creates an `EncodedResult` image object: `make test_gui_model test_app_commands`
- assigns encoded result to a second pane: `make test_app_commands test_ui_interaction`
- compares source and result in side-by-side mode: `make test_draw_list test_gui_model`
- computes and displays PSNR when compatible: `make test_metrics test_app_commands test_draw_list`
- saving encoded bytes writes the same bytes represented by result metadata: `make test_app_commands`
- long backend labels, long technical errors, byte size, and PSNR remain clipped within their draw-list text rectangles: `make test_draw_list`
- overflowing text is elided through the text atlas path: `make test_text_atlas`

## Regression Checks

- changing view mode does not delete image objects: `make test_gui_model`
- assigning images to panes does not create image copies: `make test_gui_model test_ui_interaction`
- editing encoder parameters does not mutate already running encode config snapshots: `make test_gui_model`
- failed encode does not remove source image or completed results: `make test_encode_runner test_gui_model`
- source replacement clears dependent results only after successful decode: `make test_gui_model`
- no UI action blocks the render loop on codec execution: `make test_encode_runner`
- no unsupported VAAPI rate-control mode is shown: `make test_encoder_backends`

## Manual Smoke Commands

Create a temporary real source image and run the renderer pixel probe:

```sh
smoke_image="$(mktemp codec_vis_smoke.XXXXXX.jpg)"
trap 'rm -f "$smoke_image"' EXIT
ffmpeg -v error -y -f lavfi -i testsrc2=size=96x64:duration=1 -frames:v 1 "$smoke_image"
timeout 5 ./codec_vis_gui --smoke-frames 3 --probe-image-pixels "$smoke_image"
```

Expected output includes:

```text
Wayland: xdg-shell window 1280x720
Image pixel probe: nonblank
Frames presented: 3
Vulkan renderer:
Images: 1
Visible pane rects: 1
```

Live desktop portal smoke:

```sh
./codec_vis_gui
```

Press `Import`, then cancel the desktop file chooser. Expected state: the window remains open, no source image is replaced, and no fatal error is printed. Press `Import` again and choose a local supported JPEG. Expected state: the image list contains the selected source filename, the first pane displays the selected image, and the status bar reports the active pane and image name.
