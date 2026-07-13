# VAAPI HEVC Encoding Knobs

Reference sources: installed libva headers for `va.h` and `va_enc_hevc.h`.

The user-facing encoder controls should be VAAPI/HEVC controls, not transport details like a DRM render-node path. The implementation identity is exposed separately as `implementation`, using `vaQueryVendorString()` when available, for example `VAAPI - Intel iHD driver for Intel(R) Gen Graphics - 26.1.5`.

## Rate Control

libva exposes rate-control capability through `VAConfigAttribRateControl` and rate-control parameters through `VAEncMiscParameterRateControl`, `VAEncMiscParameterFrameRate`, and `VAEncMiscParameterBufferQualityLevel`. This still-image UI exposes `qpi` only because it submits one IDR/I picture.

The UI must not hard-code `rate-control`. It is built from the implementation's `VAConfigAttribRateControl` bitmask queried with `vaGetConfigAttributes()` for local HEVC encode profiles. `VA_RC_MB` is not a primary rate-control mode; it controls whether `mbbrc=on` can be offered.

Additional libva rate-control fields exposed here: `window-size`, `initial-qp`, `basic-unit-size`, `disable-frame-skip`, `disable-bit-stuffing`, `target-frame-size`, `quality-factor`, and `icq-quality-factor`.

## Quantization

libva exposes trellis quantization through `VAEncMiscParameterQuantization`.

## Still-Image Scope

GOP/video controls such as key interval, P/B-frame counts, reference frames, and B-pyramid are intentionally not user-facing here. This program encodes images, so the VAAPI HEVC path submits one IDR picture with fixed single-picture sequence timing.

## Partitioning

libva exposes HEVC picture flags for tiles, WPP, and loop filtering across tile/slice boundaries.

The current packed-slice still path supports one slice. Tile counts and WPP/filter flags are wired into `VAEncPictureParameterBufferHEVC`.

## HEVC Coding Tools

libva sequence/picture/slice buffers expose the syntax/tool flags now surfaced by the metadata: profile/level/tier, strong intra smoothing, AMP, SAO, temporal MVP, transform skip, dependent slices, sign data hiding, constrained intra, CU QP delta, QP/chroma offsets, deblocking offsets, merge candidates, parallel merge level, and CTU bit-size limit.

## Not Encoding Knobs

The DRM render node is an internal device selection detail and is not listed as a HEVC encoding knob. Every usable render node is discovered independently and exposed as a distinct backend whose name includes the driver vendor and render-node identity. A failed node does not prevent other nodes from being queried or used.

The controls for each backend come from that device's own encode profiles and `VAConfigAttrib*` values. Rate-control modes, bit depths, chroma layouts, quality range, packed-header support, tiling, and maximum dimensions are therefore never copied from another installed driver. The encode path revalidates the selected profile/format/rate-control combination immediately before creating the VA config so a driver change produces a specific error rather than undefined behaviour.
