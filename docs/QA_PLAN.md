# codec_vis QA plan

This plan treats codec comparison correctness as the primary product requirement. A successful encode that cannot be interpreted or compared correctly is a failure, not a partial success.

## Supported test envelope

- GUI testing must use the display's native resolution, never a lower resolution. The current display is 2880×1800; use `--window-size 2880 1800` for GUI checks here.
- Input precision: 8, 10, 12, and 14 bits. Every pipeline stage must either preserve the selected precision or reject it with the backend name, operation, requested format, and supported formats.
- Chroma: monochrome, 4:2:0, 4:2:2, and 4:4:4.
- Color descriptions use the numeric code points and equations in ITU-T H.273 (07/2024): primaries, transfer characteristics, matrix coefficients, range, and 4:2:0 sample location are independent fields.
- Enum choices are scrollable/segmented selection panes. Do not introduce dropdowns. Every non-obvious control has a hover tooltip containing the application default, codec meaning, valid range, and dependency/disabled reason.

## Release-blocking invariants

1. Never substitute the source image for a missing decoded preview.
2. Never reduce bit depth, chroma resolution, or color gamut without an explicit selected transform.
3. Never apply tone/gamut mapping implicitly. HDR-to-SDR and wide-gamut-to-BT.709 with mapping set to `None` must fail before encoding.
4. Quality metrics compare the independently decoded result with the original source (end-to-end). Each image is converted from its actual color description to linear BT.2020 D65, FP32 (FP16 semantics widened), with 1 = 203 cd/m². No gamut clipping, tone mapping, or integer intermediate is allowed. Different supported color descriptions are transformed independently; incomplete planes or unspecified interpretations fail explicitly. Difference images retain their separate signal-domain comparison path. Decoder metadata must never be replaced with encoder-input metadata.
5. Mutually exclusive rate-control modes cannot be active or submitted together.
6. Bitstream color signaling must agree with the transformed samples. Backends that cannot signal the requested description must reject it explicitly.
7. A canceled file chooser is not an error. A failed encode, decode, metric, upload, or save identifies its subsystem and operation in the visible status area.
8. All source, result, and derived objects remain reachable in the image list. The list and encoder inspector scroll without drawing or receiving input outside their clips.
9. Exact byte count, human-readable size, encode time, decode time, and metric time remain inspectable without truncated-only text.
10. Scratch mode replaces the previous result for a source/backend; keep mode preserves every result. Selected entries can be removed and active sources cannot be removed until their run is canceled.

## Automated coverage

### Trust regression checks

- `make test_image_import`: compressed fixtures remain in memory. Checks RGB/grayscale JPEG-LS at 2/8/10/12/14/16 bits with every legal interleave layout; lossless JP2 and raw J2K at 8/10/12/14/16 bits; JPEG XR 8/16-bit RGB; PNG 8/16-bit and PQ/BT.2020 cICP; WebP ICC management; existing JPEG/JXL and JPEG XS dispatch; renamed files, misleading marker bytes in JPEG APP data, and malformed/truncated inputs. `test_additional_encoders` reimports all six added backends' outputs and compares decoded sample values. Import never reconstructs an RGB source through integer YUV or substitutes source pixels for a failed decode. RGB sources without a profile and streams without complete CICP have visible interpretation notes.

- `make test_metrics`: admitted adapters produce scores for HDR, wide-gamut and negative working-space values; sub-8-bit differences survive; identity and SDR reference scores are checked. CIEDE2000 matches published Sharma reference pairs.
- `make test_signal_math`: exhaustively checks every 8/10/12/14/16-bit code through endpoint-normalized dequantization, sRGB EOTF/inverse EOTF and nearest quantization, with FP32/FP64 intermediates. Tests actual RGB and neutral YUV metric ingress, full/limited range including footroom/headroom, sRGB branch boundaries and quantizer rounding ties. FP16 is separately audited: lossless at 8/10 bits, insufficient at 12/14/16 bits. Metric storage therefore remains FP32. The bin quantizer `floor(clamp(x, 0, nextafter(1, 0)) * 256)` has different decision boundaries from nearest `x * 255`; the formats here use endpoint normalization with nearest rounding.
- `make test_encode_runner`: decoded previews publish without computing metrics, and a blocked metric evaluator cannot block another encode/decode. Input quantization remains measurable against the original source.
- `make build/test_app_commands` followed by `./build/test_app_commands --metrics-only`: metric requests are lazy, duplicate requests are suppressed, scores update progressively, and notifications for removed results are ignored. This entry point does not run Save tests.
- `make test_ui_interaction test_gui_model`: Encode resolves the selected source/result lineage, Save targets only the selected result, Cancel targets the selected run and reaches the worker even while queued. Late start/completion notifications cannot undo cancellation. View-mode changes retain zoom/position and initialize an empty comparison pane with the selected result and its source.
- `make test_encoder_backends`: x265 output color metadata is read back from the bitstream, independent of deliberately incorrect out-of-band hints. AV2 must either encode with a compatible runtime or reject the installed TFLite-shim configuration explicitly before entering AVM.
- `make test_hevc_large_nal`: a 4096×2160 12-bit 4:4:4 noisy lossless x265 image produces a NAL larger than libde265's default 16 MiB limit. Checks exact lossless samples, color metadata, and byte-for-byte agreement with FFmpeg's independently decoded output. This reproduces the former `de265_push_data failed: NAL unit size exceeds security limit` failure. The preview decoder now accommodates the supplied compressed buffer size while retaining the image-size and SEI limits.
- `make test_additional_encoders`: native Kvazaar, WebP, libvpx VP8/VP9, SVT-VP9, and rav1e encode/decode checks run entirely in memory. Every offered depth/chroma combination is decoded, including 12-bit AV1; lossless modes preserve prepared samples exactly and WebP RGB lossless preserves the original 8-bit RGB source. Tests cover strided input, quality endpoints, full range, PQ/BT.2020 context, truncated streams, incomplete planes, unknown parameters, and explicit WebP HDR conversion. Kvazaar and SVT-VP9 reject non-multiple-of-eight geometry before calling the libraries. VP8/VP9 have incomplete color signaling, so absent CICP fields are explicitly retained as application context rather than inferred from the stream.
- Live native-resolution input check: launch `./codec_vis_gui --window-size 2880 1800 INPUT.jpg` on the current 2880x1800 display. Complete an encode, switch Side/Split/Blink/Diff, and verify both the visible result and unchanged zoom. No preload shim or legacy library path is needed. Use the display's actual native dimensions on other hardware.

### Current validation limits

- SSIMULACRA2 is excluded: the native BT.2020 candidate differs from the upstream SDR reference on a flat fixture (~0.49 points). PSNR-HVS is removed because the old adapter required integer YUV. Neither appears in the offered registry. Admitted perceptual metrics disclose their SDR calibration and evaluate HDR as an unclipped extension.
- AV2 is unavailable when the linked AVM pulls TFLite partition inference into a binary that supplies only compatibility shims. The tested AVM build reaches that inference even with its public ML-disable options set. Capability discovery and direct encode calls must both report this dependency problem before invoking the encoder.
- `make test_codec_integrity` verifies ALF rejection at every RDO level, VVC color signaling through native VVdeC/libavcodec decoding and reimport, all six chroma sample locations, and JPEG 2000 default/lossless fidelity at 8/10/12 bits. uvg266 ALF remains unavailable because RDO 0/1 also produce invalid coefficients. AV2 uses the matching AVM reference decoder directly because valid monochrome streams crash the installed dav2d.

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
- Metrics transform supported color descriptions independently into linear BT.2020; derived signal-domain difference images reject incompatible descriptions.
- Metric-specific failures preserve other scores and the decoded preview. Metrics are requested separately and run in a dedicated native worker. The registry is the sole offered list; adapter additions require reference parity and working-space conformance checks.

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
