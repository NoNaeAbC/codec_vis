# codec_vis QA plan

This plan treats codec comparison correctness as the primary product requirement. A successful encode that cannot be interpreted or compared correctly is a failure, not a partial success.

## Supported test envelope

- Minimum window: 1920x1080 logical pixels. Test 1920x1080, 2560x1440, and 3840x2160 at 1x and 2x output scale.
- Input precision: 8, 10, 12, and 14 bits. Every pipeline stage must either preserve the selected precision or reject it with the backend name, operation, requested format, and supported formats.
- Chroma: monochrome, 4:2:0, 4:2:2, and 4:4:4.
- Color descriptions use the numeric code points and equations in ITU-T H.273 (07/2024): primaries, transfer characteristics, matrix coefficients, range, and 4:2:0 sample location are independent fields.
- Enum choices are scrollable/segmented selection panes. Do not introduce dropdowns. Every non-obvious control has a hover tooltip containing the application default, codec meaning, valid range, and dependency/disabled reason.

## Release-blocking invariants

1. Never substitute the source image for a missing decoded preview.
2. Never reduce bit depth, chroma resolution, or color gamut without an explicit selected transform.
3. Never apply tone/gamut mapping implicitly. HDR-to-SDR and wide-gamut-to-BT.709 with mapping set to `None` must fail before encoding.
4. Metrics compare the decoded result with the exact transformed encoder input, at a common precision/chroma layout, and never with an unrelated source signal.
5. Mutually exclusive rate-control modes cannot be active or submitted together.
6. Bitstream color signaling must agree with the transformed samples. Backends that cannot signal the requested description must reject it explicitly.
7. A canceled file chooser is not an error. A failed encode, decode, metric, upload, or save identifies its subsystem and operation in the visible status area.
8. All source, result, and derived objects remain reachable in the image list. The list and encoder inspector scroll without drawing or receiving input outside their clips.
9. Exact byte count, human-readable size, encode time, decode time, and metric time remain inspectable without truncated-only text.
10. Scratch mode replaces the previous result for a source/backend; keep mode preserves every result. Selected entries can be removed and active sources cannot be removed until their run is canceled.

## Automated coverage

### Image representation and conversion

- Plane count, dimensions, strides, storage size, neutral chroma, and maximum values for all 16 pixel formats (four depths by four chroma layouts).
- Exhaustive depth conversion pairs: endpoints, midpoint error, monotonicity, and round-trip error bounds.
- 4:4:4→4:2:2/4:2:0 averaging and 4:2:0/4:2:2→4:4:4 reconstruction on odd and even dimensions.
- H.273 limited/full-range vectors at 8/10/12/14 bits, including black, nominal white, chroma midpoint, and excursion clamping.
- BT.709, Display P3, and BT.2020 primary transforms; sRGB, BT.709, linear, PQ, and HLG transfer round trips.
- Assert rejection for unspecified source metadata when a color transform is requested.
- Assert rejection for HDR/gamut narrowing with mapping `None`; test clip and Reinhard deterministically.

### Backend contract tests

For every registered backend and every supported format:

- Capability query returns usable controls, explicit defaults, and help text.
- A 64x64 deterministic image encodes, produces non-empty bytes, decodes, and reports the decoded format.
- Decode output is not byte-identical to the source object by aliasing.
- Invalid precision/chroma/profile/rate-control combinations fail with backend and parameter context.
- Parse the produced bitstream/container independently and verify codec, dimensions, bit depth, chroma, primaries, transfer, matrix, range, and still/intra constraints.
- Run lossless modes with bit-exact decoded comparison.
- Run lossy modes at low/mid/high quality and assert monotonic size trends with tolerant quality trends.
- Exercise 8/10-bit for every capable backend; 12-bit for capable builds; 14-bit must either pass or produce the explicit unsupported error.

### Metrics and derived images

- Identical images produce infinite PSNR and a zero-valued neutral difference image.
- Chroma-only errors affect the difference image and chroma/all-plane metrics.
- Format-normalized comparisons cover 4:2:0 source versus 4:4:4 still decoder output and 8-bit versus 10/12/14-bit output.
- Different color descriptions are rejected unless an explicit comparison transform is supplied.
- VMAF allocation/feature failures retain PSNR results and show a metric-specific reason.

### State, workflow, and errors

- Queue, run, cancel-requested, canceled, failed, and completed transitions.
- Cancellation before encode and before decode; document backends whose in-process API cannot interrupt an active call.
- Scratch replacement and keep-history behavior, deletion, dangling parent cleanup, pane cleanup, and active-source deletion rejection.
- Open/save chooser cancel, overwrite refusal, permission failure, disk-full/short-write simulation, and invalid input.
- Error status contains severity, subsystem, operation, message, and dismissal behavior.

### UI geometry and interaction

- Snapshot/layout tests at every supported resolution/scale.
- Populate 100 images, all backends, and 100 parameters; scroll to first/middle/last items and verify draw/input clipping.
- Verify every option, result detail, queue entry, and error is reachable at 1920x1080.
- Long UTF-8 filenames/backend names and exact 64-bit byte counts remain available via tooltip.
- Disabled dependent controls ignore clicks and explain the active condition.
- Keyboard/pointer focus, slider capture, split dragging, zoom, linked panes, and delete behavior.

## Manual visual protocol

- Use a chart containing saturated BT.2020/P3 colors, skin tones, neutral ramps, near-black steps, highlights above SDR white, fine chroma detail, grain, text, and transparency where supported.
- Compare source/transformed reference/decoded result at 100%, 200%, split, blink, and amplified difference.
- Inspect banding at every depth, chroma siting shifts, clipping, hue changes, haloing, blocking, ringing, and grain loss.
- Validate at least one SDR BT.709, Display P3, BT.2020 PQ, and BT.2020 HLG source.
- Record encoder version, build bit depth, hardware/driver, selected parameters, byte size, and all three timings with every defect.

## Release matrix and evidence

Maintain one row per backend × format × color description with: capability result, encode/decode result, independent bitstream probe, metric result, known limitation, and test artifact hash. A release is blocked by any invariant violation, crash, silent conversion, mislabeled screenshot, unreachable control, or backend presented as supporting a combination that its contract test rejects.
