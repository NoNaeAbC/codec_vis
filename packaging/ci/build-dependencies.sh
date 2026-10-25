#!/bin/bash
set -euo pipefail
ROOT=$(pwd)
PREFIX=${CODEC_VIS_PREFIX:-/opt/codec-vis}
SOURCES=${CODEC_VIS_SOURCES:-/opt/codec-vis-sources}
JOBS=${JOBS:-2}
export PKG_CONFIG_PATH="$PREFIX/lib/pkgconfig:$PREFIX/lib/x86_64-linux-gnu/pkgconfig"
export CMAKE_PREFIX_PATH="$PREFIX"
mkdir -p "$PREFIX" "$SOURCES"
python3 - "$SOURCES" packaging/ci/dependencies.json <<'PY'
import json, pathlib, subprocess, sys
base=pathlib.Path(sys.argv[1])
for name,(url,revision) in json.load(open(sys.argv[2])).items():
    path=base/name
    if not (path/'.git').exists():
        subprocess.run(['git','init',str(path)],check=True)
        subprocess.run(['git','-C',str(path),'remote','add','origin',url],check=True)
    subprocess.run(['git','-C',str(path),'fetch','--depth=1','origin',revision],check=True)
    subprocess.run(['git','-C',str(path),'checkout','--detach','FETCH_HEAD'],check=True)
PY
cmake_install() {
  local name=$1
  shift
  cmake -S "$SOURCES/$name" -B "$SOURCES/$name/build-ci" -G Ninja \
    -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX="$PREFIX" -DCMAKE_INSTALL_LIBDIR=lib \
    -DCMAKE_POLICY_VERSION_MINIMUM=3.5 -DCMAKE_POSITION_INDEPENDENT_CODE=ON -DBUILD_SHARED_LIBS=OFF \
    -DCMAKE_C_FLAGS_RELEASE='-O3 -DNDEBUG' -DCMAKE_CXX_FLAGS_RELEASE='-O3 -DNDEBUG' "$@"
  cmake --build "$SOURCES/$name/build-ci" -j "$JOBS"
  cmake --install "$SOURCES/$name/build-ci"
}
cmake_install vvenc -DVVENC_ENABLE_WERROR=OFF -DVVENC_ENABLE_LINK_TIME_OPT=OFF
cmake_install vvdec -DVVDEC_ENABLE_WERROR=OFF -DVVDEC_ENABLE_LINK_TIME_OPT=OFF -DVVDEC_LIBRARY_ONLY=ON
cmake_install uvg266 -DUVG_BUILD_SHARED=OFF -DUVG_BIT_DEPTH=10 -DUVG_BUILD_TESTS=OFF -DUVG_BUILD_EXECUTABLE=OFF
cmake_install svt-jpeg-xs -DBUILD_APPS=OFF -DBUILD_TESTING=OFF
cmake_install svt-vp9
bash packaging/ci/build-static-libraries.sh
meson_mode=()
[[ ! -f "$SOURCES/dav2d/build-ci/meson-private/coredata.dat" ]] || meson_mode=(--reconfigure)
meson setup "${meson_mode[@]}" "$SOURCES/dav2d/build-ci" "$SOURCES/dav2d" --prefix="$PREFIX" --libdir=lib \
  --buildtype=release --default-library=static -Denable_tests=false -Denable_tools=false
meson compile -C "$SOURCES/dav2d/build-ci" -j "$JOBS"
meson install -C "$SOURCES/dav2d/build-ci"
# Keep real AVM inference enabled. The release must never silently ship the
# compatibility shims in place of TFLite. configure_avm_runtime verifies hashes.
avm_config=(cmake -S "$SOURCES/avm" -B "$SOURCES/avm/build-ci" -G Ninja
  -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX="$PREFIX" -DCMAKE_INSTALL_LIBDIR=lib \
  -DCMAKE_POLICY_VERSION_MINIMUM=3.5 -DBUILD_SHARED_LIBS=OFF -DENABLE_TESTS=OFF \
  -DENABLE_DOCS=OFF -DENABLE_TESTDATA=OFF -DENABLE_TOOLS=OFF -DENABLE_EXAMPLES=OFF)
# AVM re-extracts its bundled TFLite sources on each configure, invalidating
# otherwise reusable objects. Reconfigure when the revision or options change.
avm_signature=$({ git -C "$SOURCES/avm" rev-parse HEAD; printf '%s\n' "${avm_config[@]}"; } | sha256sum)
avm_stamp="$SOURCES/avm/build-ci/codec-vis-config.txt"
if [[ ! -f "$avm_stamp" ]] || [[ $(cat "$avm_stamp") != "$avm_signature" ]]; then
  "${avm_config[@]}"
  printf '%s\n' "$avm_signature" > "$avm_stamp"
fi
cmake --build "$SOURCES/avm/build-ci" -j "$JOBS"
cmake --install "$SOURCES/avm/build-ci"
python3 scripts/configure_avm_runtime.py "$SOURCES/avm/build-ci"
# A deliberately small FFmpeg avoids pulling in every desktop audio/video stack
# through its static pkg-config dependency graph.
cd "$SOURCES/ffmpeg"
./configure --prefix="$PREFIX" --libdir="$PREFIX/lib" --disable-everything \
  --disable-autodetect --disable-programs --disable-doc --disable-debug \
  --disable-shared --enable-static --enable-pic --enable-gpl \
  --enable-avcodec --enable-avutil --enable-decoder=h264,hevc,vvc,av1,vp8,vp9 \
  --enable-parser=h264,hevc,vvc,av1,vp8,vp9 --enable-vaapi --enable-vulkan --enable-libdrm \
  --enable-hwaccel=h264_vaapi,hevc_vaapi,av1_vaapi,vp9_vaapi,h264_vulkan,hevc_vulkan,av1_vulkan,vp9_vulkan
make -j "$JOBS"
make install
cd "$ROOT"
printf '%s\n' "$PREFIX" > build/release-prefix.txt
