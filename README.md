# codec_vis

A Linux application for comparing codec output: inspect decoded images, vary encoder settings, and compare encoded size, quality scores, and encode/decode performance. Video codecs are used to encode individual images. Encoder families include H.264, HEVC, VVC, AV1, AV2, VP8/VP9, JPEG, JPEG-LS, JPEG 2000, JPEG XL, JPEG XR, JPEG XS, PNG, and WebP.

WARNING: This entire codebase is 100% written by GPT-5.5 without any human review.

GPT-5.5 refuses to test the code it writes and did not bother hooking up many encoders, even after many requests to do so from my side. It literaly gave as an excuse once that the source image is a good enough apporximation and given its higher image quality it is likely improving the UX to not hook up the encoder. There are a lot of features that I requested but that GPT-5.5 ended up claiming that it is good enough to add bit depth and chroma subsampling configuration for only one encoder and that adding this to all the others is too much work. Due to the refusal to test the code, this code will crash often. Good luck getting this code to work on anything else but my machine. The UI sucks and you will likely not know how to use this tool correctly. I make no guarantee that this implementaion let's you compare codecs in good faith and that there are not any misconfigurations.

## Requirements

The GUI requires a native Wayland session with `color-management-v1` support for linear BT.2020, and a Vulkan driver that supports FP16 pass-through swapchains and storage-image rendering. It renders with compute shaders into a linear BT.2020 surface. The file picker uses the desktop portal.

To build, install:

- A C++23-capable compiler, a C11 compiler, Make, and `pkg-config`.
- `wayland-scanner`, the Wayland protocol XML files, and `glslc`.
- The development libraries listed in `CODEC_PKGS` and `GUI_PKGS` in the [Makefile](Makefile), plus Little CMS and jxrlib. The current build links all listed codec libraries.
- libheif 1.23 or newer, with decoders for the container codecs you intend to import.
- Python 3 for the test and dependency-configuration scripts.

## Build and run

From the repository root:

```sh
make -j4 codec_vis_gui
./codec_vis_gui image.png
```

Run without a filename to import an image through the GUI, or pass multiple files:

```sh
./codec_vis_gui reference.avif candidate.vvic
```

The executable embeds its shaders, UI fonts, and energy helper. You can rename it and run it from any working directory.

If the installed AVM library needs TensorFlow Lite archives from its source build, configure the matching link dependencies before building:

```sh
python3 scripts/configure_avm_runtime.py /path/to/avm/build
```

## Compare images

1. Import a source, choose an encoder implementation, adjust its settings, and encode.
2. Assign the source and retained results to panes A/B. Use linked navigation, wipe, blink, or difference views to inspect them.
3. Select results and compute quality metrics. The queue and plots compare size, scores, and timing; Configuration shows the recorded settings.
4. Use Decode to select a compatible decoder and measure repeated decoding attempts. Measurements keeps the individual attempts and session statistics.

Scroll over an image to zoom and drag it to pan. `0` fits the image, `1` selects actual pixels, `F` toggles focus mode, and holding Space temporarily shows A. Drag unused window space to move the window.

Zooming out uses trilinear mipmapping of the linear BT.2020 image. At 1:1 and when zooming in, the viewer fetches original level-zero texels with nearest-neighbor sampling. Mipmaps are rebuilt when a preview changes and count toward the GPU image cache budget.

Bit depth, chroma subsampling, and color conversion are part of the experiment. **Compare prepared input / decoded** in Metrics shows the application’s prepared image alongside the decoded result. The Metrics panel describes each score's reference and computation.

## Anamorphic images

Enter a sample width:height ratio in **Pixel AR** in the top bar. Valid ratios apply immediately to the selected source and subsequent imports. For example, use `4:3` for `image.nef`. Use `auto` to read embedded metadata (the default), or `1:1` to force square pixels. Existing encoded results retain their original interpretation; encode again to produce output with the changed SAR.

The viewer displays rectangular pixels without resampling the imported image. Fit, actual-pixel view, zoom, pan, and pixel inspection account for the ratio; the stored width, height, and sample planes remain intact. Source cards show the applied SAR. Ratios are reduced to relatively prime integers and must fit 16-bit signaling fields.

SAR is written and read back for x264 H.264, x265/kvazaar HEVC, VVenC/uvg266 VVC, JPEG (JFIF density), and PNG (pHYs). Both VVC paths signal H.274 clause 7 Extended_SAR with the aspect-ratio-present and constant flags set and `aspect_ratio_idc = 255`. Other encoder paths show a warning before encoding and record the loss of aspect-ratio signaling in the result configuration. This warning describes the current encoder implementation, including formats whose metadata support has not yet been implemented here.

Run `make test_anamorphic` to check bitstream signaling and re-import, including the five explicit VVC VUI fields.

## Input formats

| Input | Support |
| --- | --- |
| Still images | PNG, JPEG, JPEG-LS, JPEG 2000, JPEG XL, JPEG XR, JPEG XS, WebP, and Nikon NEF |
| HEIF containers | HEIF/HEIC/HIF, AVIF, and VVIC; imports the primary image using libheif. Grid primary images are rejected. Codec support depends on installed libheif decoders. |
| Elementary video streams | H.264, HEVC, and VVC Annex B; imports the first decoded frame |
| IVF | AV1, AV2, VP8, and VP9; imports the first decoded frame |

Import notes and Configuration describe color interpretation and conversions. Alpha is not retained.

Hardware codec support varies by device, profile, bit depth, chroma format, and driver. HEVC SCC decoding is experimental and requires a compatible Vulkan Video driver.

## Command line

```sh
make -j4 codec_vis_cli
./codec_vis_cli --help
```

The CLI is an encoding harness. Its default run executes a preset codec batch and writes fixed output filenames in the current directory. Its help lists the dedicated VA-API AV1 mode and parameters.

## Tests

```sh
make -j4 test
```

Some integration tests require compatible hardware and drivers. Individual test targets are listed in the Makefile.

## Portable bundle

```sh
make portable-bundle
```

This creates a directory and a `.tar.zst` archive under `dist/`. Packaging requires `ldd`, `patchelf`, `readelf`, `pkg-config`, `tar`, and `zstd`. The bundle includes libraries, libheif plugins, fonts, and shaders; the target system still needs a compatible Wayland compositor and graphics driver.
