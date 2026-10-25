#!/bin/bash
set -euo pipefail
PREFIX=${CODEC_VIS_PREFIX:-/opt/codec-vis}
SOURCES=${CODEC_VIS_SOURCES:-/opt/codec-vis-sources}
export PKG_CONFIG_PATH="$PREFIX/lib/pkgconfig:$PREFIX/lib/x86_64-linux-gnu/pkgconfig"
mkdir -p build dist
python3 scripts/configure_avm_runtime.py "$SOURCES/avm/build-ci"
packages='SvtJpegxs x265 libjpeg libraw libheif libvvenc libvvdec uvg266 SvtAv1Enc libva libva-drm avm libde265 dav1d dav2d charls libpng libjxl libjxl_cms libjxl_threads libopenjp2 x264 libavcodec libavutil kvazaar libwebp vpx SvtVp9Enc rav1e gstreamer-codecparsers-1.0 libdrm vulkan wayland-client harfbuzz freetype2 fontconfig sdbus-c++ xkbcommon libjxr'
# This is a whitespace-separated fixed package list, not caller-provided code.
# shellcheck disable=SC2086
python3 scripts/static_link_flags.py $packages > build/static-link.flags
flags=$(cat build/static-link.flags)
# Keep the literal make variable reference: Make imports the matching real
# AVM/TFLite archives from build/avm-runtime.mk (never the inference shims).
flags+=' $(AVM_TFLITE_LIBS)'
# libraw advertises -fopenmp even to consumers. Its runtime is linked explicitly
# as libgomp.a, so do not let the compiler inject a shared OpenMP runtime.
# shellcheck disable=SC2086
cflags=$(pkg-config --cflags $packages)
cflags=${cflags//-fopenmp/}
make -j "${JOBS:-2}" codec_vis_gui build/test_codec_roundtrips \
  CXXFLAGS='-std=c++2c -O2 -g0 -DNDEBUG -Wall -Wextra -Wpedantic' \
  LOCAL_RUNPATH= GUI_LIBS= CODEC_LIBS="$flags" CODEC_CFLAGS="$cflags" LDFLAGS="-Wl,--exclude-libs,ALL"
install -m755 codec_vis_gui dist/codec_vis-linux-x86_64
strip --strip-unneeded dist/codec_vis-linux-x86_64
readelf -d dist/codec_vis-linux-x86_64 > dist/elf-dependencies.txt
python3 - <<'PY'
import re
from pathlib import Path
text=Path('dist/elf-dependencies.txt').read_text()
allowed={'libvulkan.so.1','libva.so.2','libva-drm.so.2','libdrm.so.2',
         'libc.so.6','libm.so.6','libdl.so.2','libpthread.so.0','librt.so.1','ld-linux-x86-64.so.2'}
needed=set(re.findall(r'\(NEEDED\).*\[([^]]+)\]',text))
if needed-allowed: raise SystemExit('Unexpected dynamic dependencies: '+', '.join(sorted(needed-allowed)))
if 'RPATH' in text or 'RUNPATH' in text: raise SystemExit('Release must not contain an RPATH/RUNPATH')
print('Dynamic dependency audit passed:',', '.join(sorted(needed)))
PY
(cd dist; sha256sum codec_vis-linux-x86_64 > SHA256SUMS)
{
  git rev-parse HEAD
  g++ --version
  dpkg-query -W
  for repo in "$SOURCES"/*; do
    if [[ -d "$repo/.git" ]]; then printf '%s ' "$(basename "$repo")"; git -C "$repo" rev-parse HEAD; fi
  done
} > dist/build-info.txt
