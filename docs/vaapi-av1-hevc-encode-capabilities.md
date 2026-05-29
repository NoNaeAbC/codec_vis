# VA-API AV1 and HEVC Encode Capabilities

Source hardware/driver snapshot:

- `~/vainfo.txt`
- VA-API version: 1.23, libva 2.22.0
- Driver: Intel iHD, `Intel(R) Gen Graphics - 26.1.5`
- Local headers: installed libva `va.h`, `va_enc_av1.h`, and `va_enc_hevc.h`.
- External references checked: Intel/libva API docs and Intel media-driver notes.

This document focuses on encode entrypoints and still-image / intra-coded-frame use.

## Summary

AV1 encode is narrower than HEVC on this driver:

- AV1 encode: `VAProfileAV1Profile0` only.
- AV1 encode input formats: 4:2:0 8-bit and 10-bit, plus RGB32 upload variants.
- AV1 encode max size: 8192 x 8192.
- AV1 still picture is explicit in `VAEncSequenceParameterBufferAV1::seq_fields.bits.still_picture`.

HEVC encode is broad:

- HEVC encode profiles include Main, Main10, Main422_10, Main444, Main444_10, and SCC Main/Main10/Main444/Main444_10.
- HEVC encode input formats include 4:2:0 and 4:4:4 8/10-bit for most profiles, 4:2:2 for `Main422_10`, and RGB32 upload variants.
- HEVC encode max size: 16384 x 12288.
- Intra still encoding is done by coding an I/IDR picture and setting GOP periods appropriately; there is no HEVC still-picture flag analogous to AV1.

## AV1 Encode Caps

Advertised by `vainfo`:

| Field | Value |
| --- | --- |
| Profile / entrypoint | `VAProfileAV1Profile0 / VAEntrypointEncSlice` |
| RT formats | `YUV420`, `YUV420_10`, `RGB32`, `RGB32_10`, `RGB32_10BPP`, `YUV420_10BPP` |
| Rate control | `CBR`, `VBR`, `CQP`, `ICQ` |
| Packed headers | sequence, picture, misc, raw data |
| Interlaced | none |
| Max refs | L0 = 3, L1 = 1 |
| Max dimensions | 8192 x 8192 |
| Quality levels | 7 |
| Quantization attribute | none |
| Intra refresh | none |
| Tiles | supported |

The libva AV1 encode header explicitly says the AV1 encode API supports only 8-bit/10-bit 4:2:0 input. The RGB32 formats in `vainfo` should be treated as driver upload/input conveniences, not as AV1 4:4:4 encode profiles.

### AV1 Profile / Format Implications

For still images:

- Use `VAProfileAV1Profile0`.
- Use 4:2:0 8-bit or 10-bit surfaces for the cleanest path.
- Do not expect VA-API AV1 4:4:4 encode here.
- Max square still is 8192 x 8192, subject to memory allocation and coded buffer sizing.

AV1 intra/still knobs:

- `seq_profile`: AV1 profile, range 0..2, but driver exposes only Profile0.
- `seq_level_idx`: AV1 level index, range 0..23.
- `seq_tier`: tier, range 0..1.
- `intra_period`: period between intra-only frames.
- `ip_period`: period between I/P frames.
- `seq_fields.still_picture`: still-picture encoding, no inter references.
- `seq_fields.use_128x128_superblock`: choose 128x128 vs 64x64 superblocks if supported.
- `picture_flags.frame_type`: `0 = key_frame`, `2 = intra_only frame`.
- `picture_flags.error_resilient_mode`, `disable_cdf_update`, `disable_frame_end_update_cdf`, `enable_frame_obu`.
- `refresh_frame_flags`, `primary_ref_frame`, `reference_frames[]`, `ref_frame_idx[]`; for still image these should be minimized/disabled consistently with still-picture mode.

AV1 coding tool knobs exposed by VA structs:

- Sequence-level: filter intra, intra edge filter, inter-intra compound, masked compound, warped motion, dual filter, order hint, joint compound, ref-frame MVs, superres, CDEF, restoration, bit depth, subsampling, monochrome.
- Picture-level: high precision MVs, superres use, ref-frame MVs, reduced transform set, long-term reference, recon disable, intraBC, palette mode, screen-content tools, integer MVs.
- Mode control: delta-Q, delta loop filter, transform mode, reference mode, skip mode.
- Quantization: base qindex, Y/U/V DC/AC deltas, BRC min/max base qindex, quantization matrices.
- Filters: deblock levels, sharpness, ref/mode deltas, CDEF strengths, loop restoration type/unit sizes.
- Tiles: tile rows/cols, tile groups, tile dimensions, context update tile.
- Segmentation: segment count/map/update mode, feature masks/data, optional segment map buffer.
- Global motion: warped motion parameters for reference frames.

## HEVC / H.265 Encode Caps

Common HEVC caps advertised for the main HEVC encode profiles:

| Field | Value |
| --- | --- |
| Entrypoint | `VAEntrypointEncSlice` |
| Rate control | `CBR`, `VBR`, `VCM`, `CQP`, `ICQ`, `MB`, `QVBR`, `TCBRC` |
| Packed headers | sequence, picture, slice, misc, raw data |
| Interlaced | none |
| Max refs | L0 = 3, L1 = 3 |
| Max slices | 70 |
| Slice structure | equal rows, max slice size |
| Max dimensions | 16384 x 12288 |
| Quality levels | 7 |
| Intra refresh | rolling column, rolling row |
| ROI | 16 regions, QP delta supported, priority not supported |
| Dirty rect | 16 regions |
| Tiles | supported |
| Prediction direction | previous and future |

### HEVC Encode Profiles

From `vainfo`:

| Profile | Encode? | RT formats |
| --- | --- | --- |
| `VAProfileHEVCMain` | yes | `YUV420`, `YUV444`, `YUV420_10`, `YUV444_10`, `RGB32`, `RGB32_10`, `RGB32_10BPP`, `YUV420_10BPP` |
| `VAProfileHEVCMain10` | yes | same as Main |
| `VAProfileHEVCMain422_10` | yes | `YUV420`, `YUV422`, `YUV420_10`, `YUV422_10`, `YUV420_10BPP` |
| `VAProfileHEVCMain444` | yes | same as Main |
| `VAProfileHEVCMain444_10` | yes | same as Main |
| `VAProfileHEVCSccMain` | yes | same as Main, RC is `CQP` only |
| `VAProfileHEVCSccMain10` | yes | same as Main, RC is `CQP` only |
| `VAProfileHEVCSccMain444` | yes | same as Main, RC is `CQP` only |
| `VAProfileHEVCSccMain444_10` | yes | same as Main, RC is `CQP` only |
| `VAProfileHEVCMain12`, `Main422_12`, `Main444_12` | decode only in this `vainfo` | no encode entrypoint shown |

Note: `vainfo` advertises broad RT formats for several HEVC profiles. The application still has to set HEVC SPS profile/chroma/bit-depth fields coherently; an RGB32 input surface does not mean the output HEVC profile is RGB.

### HEVC Intra / Still-Image Path

For a single still image:

- Use `VAProfileHEVCMain` for 8-bit 4:2:0, `VAProfileHEVCMain10` for 10-bit 4:2:0, `VAProfileHEVCMain444*` for 4:4:4, or SCC profiles for screen-content style testing.
- Set sequence periods so the single frame is intra: `intra_period = 1`, `intra_idr_period = 1`, `ip_period = 1` or equivalent.
- Set picture fields to IDR/I: `pic_fields.idr_pic_flag = 1`, `pic_fields.coding_type = I`, `reference_pic_flag` as needed.
- Use one I slice or multiple I slices. Driver supports up to 70 slices and equal-row/max-size slice structures.
- Set `last_picture` flags if you want end-of-sequence/end-of-stream NALs appended.

HEVC dimensions:

- Maximum encode dimensions advertised: 16384 x 12288.
- Width/height in `VAEncSequenceParameterBufferHEVC` must be multiples of minimum CU size.
- CTB/CU/TU sizes are constrained by `VAConfigAttribEncHEVCBlockSizes`; query it rather than assuming all legal HEVC block sizes are supported.

HEVC coding tool knobs exposed by VA structs:

- Profile/level/tier: `general_profile_idc`, `general_level_idc`, `general_tier_flag`.
- GOP: `intra_period`, `intra_idr_period`, `ip_period`, low-delay and hierarchical flags.
- Picture geometry: luma width/height, chroma format, bit depth.
- Block structure: min/max luma coding block, transform block size, transform hierarchy depth, PCM block sizes.
- Tools: scaling lists, strong intra smoothing, AMP, SAO, PCM, temporal MVP, sign data hiding, constrained intra prediction, transform skip, CU QP delta, weighted prediction, transquant bypass, deblocking, dependent slices.
- VUI: aspect ratio, timing, bitstream restrictions, tile fixed structure, MV length limits.
- Tiles/WPP/slices: tile column/row counts, widths/heights, entropy coding sync, loop filter across tiles/slices, slice count and slice size.
- Picture-level QP: `pic_init_qp`, chroma QP offsets, `diff_cu_qp_delta_depth`.
- Slice-level: slice type, CTU address/count, ref lists, merge candidates, slice QP/chroma QP offsets, beta/tc deblock offsets, SAO flags, temporal MVP, weighted prediction tables.
- SCC: palette mode in sequence, current-picture reference / IBC in picture.
- Screen-content hint: `screen_content_flag`.
- Weighted-prediction GPU override: `enable_gpu_weighted_prediction`.
- Q matrices: `VAQMatrixBufferHEVC` when scaling lists are enabled.

## VA-API Generic Encode Knobs

These are not all supported by every codec/profile; use `vaGetConfigAttributes()` and `vainfo` to gate them.

### Configuration Attributes

- `VAConfigAttribRTFormat`: allowed input surface formats.
- `VAConfigAttribRateControl`: RC modes.
- `VAConfigAttribEncPackedHeaders`: which headers the application must/can pack.
- `VAConfigAttribEncInterlaced`: interlaced support; here AV1/HEVC encode are progressive-only.
- `VAConfigAttribEncMaxRefFrames`: L0/L1 reference capacity.
- `VAConfigAttribEncMaxSlices`: HEVC max slices.
- `VAConfigAttribEncSliceStructure`: equal rows, max slice size, arbitrary rows, etc.
- `VAConfigAttribMaxPictureWidth` / `MaxPictureHeight`.
- `VAConfigAttribEncQualityRange`: quality/speed level count; here 7 for both AV1 and HEVC.
- `VAConfigAttribEncQuantization`: trellis/special quantization support where present.
- `VAConfigAttribEncIntraRefresh`: rolling/adaptive/cyclic intra refresh support.
- `VAConfigAttribEncROI`: number and type of ROI controls.
- `VAConfigAttribEncDirtyRect`: dirty-rectangle regions.
- `VAConfigAttribEncTileSupport`: tile support.
- `VAConfigAttribPredictionDirection`: previous/future prediction for inter coding.
- `VAConfigAttribEncRateControlExt`: temporal-layer bitrate control where advertised.
- `VAConfigAttribMaxFrameSize`: max frame size and multi-pass size control where advertised.
- Codec-specific attributes: `VAConfigAttribEncAV1`, `EncAV1Ext1`, `EncAV1Ext2`, `EncHEVCFeatures`, `EncHEVCBlockSizes`.

### Rate Control Modes

Modes seen in this `vainfo`:

- `VA_RC_CQP`: constant QP. Best fit for still-image quality comparison.
- `VA_RC_CBR`: constant bitrate.
- `VA_RC_VBR`: variable bitrate.
- `VA_RC_VCM`: video conferencing mode; HEVC only here.
- `VA_RC_ICQ`: intelligent constant quality.
- `VA_RC_MB`: macroblock/CTU-level rate control support bit.
- `VA_RC_QVBR`: quality-defined VBR; HEVC only here.
- `VA_RC_TCBRC`: transport-controlled BRC; HEVC only here.

For HEVC SCC profiles, `vainfo` reports `CQP` only.

### Rate Control Parameter Buffer

`VAEncMiscParameterRateControl` exposes:

- `bits_per_second`
- `target_percentage`
- `window_size`
- `initial_qp`
- `min_qp`
- `max_qp`
- `basic_unit_size`
- `rc_flags.reset`
- `rc_flags.disable_frame_skip`
- `rc_flags.disable_bit_stuffing`
- `rc_flags.mb_rate_control`
- `rc_flags.temporal_id`
- `rc_flags.cfs_I_frames`
- `rc_flags.enable_parallel_brc`
- `rc_flags.enable_dynamic_scaling`
- `rc_flags.frame_tolerance_mode`
- `ICQ_quality_factor`
- `quality_factor`
- `target_frame_size`

Other generic misc buffers:

- `VAEncMiscParameterFrameRate`: RC framerate.
- `VAEncMiscParameterHRD`: initial buffer fullness and CPB size.
- `VAEncMiscParameterBufferQualityLevel`: quality/speed level, 1 highest quality, larger values faster/lower quality; 0 means default.
- `VAEncMiscParameterBufferMaxFrameSize`: maximum frame size in bits.
- `VAEncMiscParameterBufferMultiPassFrameSize`: multi-pass max frame size, where supported.
- `VAEncMiscParameterMaxSliceSize`: maximum slice size in bits.
- `VAEncMiscParameterRIR`: rolling intra refresh row/column, insertion location/size, inserted-intra QP delta.
- `VAEncMiscParameterAIR`: adaptive intra refresh.
- `VAEncMiscParameterQuantization`: trellis disable/enable per I/P/B when supported.
- `VAEncMiscParameterSkipFrame`: skip/drop-frame accounting for RC.
- `VAEncMiscParameterBufferROI`: per-frame ROI QP delta/priority.
- `VAEncMiscParameterBufferDirtyRect`: dirty rectangles.
- `VAEncMiscParameterEncQuality`: HME/skip/FTQ/panic/repartition quality controls.
- `VAEncMiscParameterCustomRoundingControl`: custom intra/inter rounding offsets when supported.

## Practical Still-Image Recommendations

For this project’s still-image encoder probes:

- Prefer CQP for both AV1 and HEVC to avoid bitrate controller behavior on a one-frame encode.
- Use AV1 Profile0 only; choose 8-bit or 10-bit 4:2:0 input.
- Use AV1 `still_picture = 1`, `frame_type = key_frame`, no references, one tile group unless testing tiles.
- Use HEVC Main/Main10/Main444 as appropriate; encode one IDR I picture.
- Keep packed headers enabled or provide them explicitly as required by the driver.
- Query codec-specific feature/block-size attributes before exposing low-level toggles in a GUI.
- Treat RGB RT formats as upload/input formats; document the actual bitstream profile separately.
