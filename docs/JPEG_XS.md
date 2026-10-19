# JPEG XS integration

JPEG XS is available as **SVT-JPEG-XS**, backend ID **15**, in the encoder inspector. It encodes the transformed input into an actual JPEG XS codestream, then parses and decodes those bytes with the SVT-JPEG-XS decoder. Encoder reconstruction/source fallbacks are not used. Saving the result uses `.jxs`.

## Supported configuration

- Precision: 8, 10, or 12 bits, with planar YUV 4:2:0, 4:2:2, or 4:4:4.
- Lossy target: 1–36 total bits per luma pixel, default 6 bpp. This includes all components, not bits per component. Very low budgets may be rejected by the library when headers/content cannot fit.
- Lossless: the installed library's MLS.12 reversible path, with raw-mode coding disabled as required by that profile. Exactness applies to the transformed YUV encoder input. Source RGB/depth/chroma conversion is still included in the application's end-to-end comparison.
- Deadzone or uniform quantization for lossy coding; bitrate and quantization controls are disabled in lossless mode.
- Low-latency or low-CPU execution profiles, slice height in multiples of four, and 1–64 threads.
- Fixed five horizontal/two vertical wavelet decompositions. This integration requires width 64–65535 and height 4–65535. Common input conversion pads subsampled formats to chroma alignment. Direct API calls with unaligned subsampled inputs fail explicitly.

Monochrome encoding is excluded because this installed encoder rejects single-component input. RGB/RCT, alpha, and 14-bit input are not advertised by this integration. This is not a claim that the JPEG XS standard excludes those formats.

The dependency is `SvtJpegxs` via pkg-config. The tested installed version is **0.10.0**, with matching encoder/decoder headers and the `lossless_enable` extension.

## Color context and file format

The raw codestream carries sampling, image dimensions, and sample precision; the decoder obtains those from the actual stream. This integration writes YUV components without an RGB color transform. Bare `.jxs` does not carry CICP primaries, transfer, matrix, or range information. The application therefore retains its known color context in `EncodedImage::codedColor` for preview and metrics. Without that context, the decoder leaves the color description unspecified rather than inventing one. Exporting only the `.jxs` bytes does not export that application context; an external consumer needs the appropriate color/container information.

This adds an encoder and decoder for generated results; it does not add a general JPEG XS container/import workflow.

## Tests

`make test_jpegxs test_storage` checks lossless samples, padded strides,
color context, invalid-input handling, and export extensions.
