#!/bin/bash
# Debian development packages intentionally omit many static archives. Build
# those libraries explicitly rather than falling back to shared codec libraries.
set -euo pipefail
PREFIX=${CODEC_VIS_PREFIX:-/opt/codec-vis}
SOURCES=${CODEC_VIS_SOURCES:-/opt/codec-vis-sources}
JOBS=${JOBS:-2}
export PKG_CONFIG_PATH="$PREFIX/lib/pkgconfig:$PREFIX/lib/x86_64-linux-gnu/pkgconfig"
export CMAKE_PREFIX_PATH="$PREFIX"
mkdir -p "$SOURCES/debian" "$PREFIX/lib"
source_package() {
  local name=$1 package=$2
  local download="$SOURCES/debian/download-$name"
  if [[ -f "$SOURCES/debian/$name/debian/changelog" ]]; then return; fi
  mkdir -p "$download"
  (cd "$download"; apt-get source --download-only "$package")
  local dsc=("$download"/*.dsc)
  [[ ${#dsc[@]} == 1 ]]
  dpkg-source -x "${dsc[0]}" "$SOURCES/debian/$name"
  dpkg-parsechangelog -l "$SOURCES/debian/$name/debian/changelog" -SVersion >> "$SOURCES/debian/source-versions.txt"
}
cmake_static() {
  local name=$1; shift
  cmake -S "$SOURCES/debian/$name" -B "$SOURCES/debian/$name/build-static" -G Ninja \
    -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX="$PREFIX" -DCMAKE_INSTALL_LIBDIR=lib \
    -DCMAKE_POLICY_VERSION_MINIMUM=3.5 -DBUILD_SHARED_LIBS=OFF -DBUILD_TESTING=OFF "$@"
  cmake --build "$SOURCES/debian/$name/build-static" -j "$JOBS"
  cmake --install "$SOURCES/debian/$name/build-static"
}
meson_static() {
  local name=$1; shift
  local mode=()
  [[ ! -f "$SOURCES/debian/$name/build-static/meson-private/coredata.dat" ]] || mode=(--reconfigure)
  meson setup "${mode[@]}" "$SOURCES/debian/$name/build-static" "$SOURCES/debian/$name" \
    --prefix="$PREFIX" --libdir=lib --buildtype=release --default-library=static -Dauto_features=disabled "$@"
  meson compile -C "$SOURCES/debian/$name/build-static" -j "$JOBS"
  meson install -C "$SOURCES/debian/$name/build-static"
}
autotools_static() {
  local name=$1; shift
  (cd "$SOURCES/debian/$name"; autoreconf -fi; ./configure --prefix="$PREFIX" --disable-shared --enable-static "$@"; make -j "$JOBS"; make install)
}
source_package wayland libwayland-dev
meson_static wayland -Ddocumentation=false -Dtests=false -Ddtd_validation=false
source_package harfbuzz libharfbuzz-dev
meson_static harfbuzz -Dtests=disabled -Ddocs=disabled -Dutilities=disabled -Dicu=disabled \
  -Dglib=disabled -Dgobject=disabled -Dcairo=disabled -Dfreetype=enabled -Dintrospection=disabled
source_package xkbcommon libxkbcommon-dev
meson_static xkbcommon -Denable-x11=false -Denable-wayland=false -Denable-docs=false -Denable-tools=false -Denable-xkbregistry=false
source_package sdbus libsdbus-c++-dev
cmake_static sdbus -DSDBUSCPP_BUILD_CODEGEN=OFF -DSDBUSCPP_BUILD_TESTS=OFF
source_package de265 libde265-dev
autotools_static de265 --disable-dec265 --disable-sherlock265
source_package dav1d libdav1d-dev
meson_static dav1d -Denable_tests=false -Denable_tools=false
source_package charls libcharls-dev
cmake_static charls -DCHARLS_BUILD_TESTS=OFF -DCHARLS_BUILD_SAMPLES=OFF
source_package highway libhwy-dev
cmake_static highway -DHWY_ENABLE_TESTS=OFF -DHWY_ENABLE_EXAMPLES=OFF -DHWY_ENABLE_CONTRIB=ON
source_package jxl libjxl-dev
cmake_static jxl -DJPEGXL_ENABLE_TOOLS=OFF -DJPEGXL_ENABLE_DOXYGEN=OFF \
  -DJPEGXL_ENABLE_MANPAGES=OFF -DJPEGXL_ENABLE_JPEGLI=OFF -DJPEGXL_FORCE_SYSTEM_BROTLI=ON \
  -DJPEGXL_FORCE_SYSTEM_HWY=ON -DJPEGXL_ENABLE_BENCHMARK=OFF
source_package x264 libx264-dev
(cd "$SOURCES/debian/x264"; ./configure --prefix="$PREFIX" --enable-static --disable-shared --disable-cli; make -j "$JOBS"; make install)
source_package svt-av1 libsvtav1enc-dev
cmake_static svt-av1 -DBUILD_APPS=OFF -DBUILD_TESTING=OFF
source_package kvazaar libkvazaar-dev
autotools_static kvazaar --without-cryptopp
source_package jxr libjxr-dev
(cd "$SOURCES/debian/jxr"; make -j "$JOBS")
find "$SOURCES/debian/jxr" -name 'libjpegxr.a' -exec install -m644 '{}' "$PREFIX/lib/" \;
find "$SOURCES/debian/jxr" -name 'libjxrglue.a' -exec install -m644 '{}' "$PREFIX/lib/" \;
source_package gstreamer libgstreamer1.0-dev
meson_static gstreamer -Dauto_features=disabled -Dtests=disabled -Dexamples=disabled -Dbenchmarks=disabled -Dintrospection=disabled
source_package gst-parsers libgstreamer-plugins-bad1.0-dev
meson_static gst-parsers -Dauto_features=disabled -Dcodecparsers=enabled -Dtests=disabled -Dexamples=disabled -Dintrospection=disabled
source_package heif libheif-dev
cmake_static heif -DWITH_EXAMPLES=OFF -DENABLE_PLUGIN_LOADING=OFF \
  -DWITH_LIBDE265=ON -DWITH_LIBDE265_PLUGIN=OFF -DWITH_DAV1D=ON -DWITH_DAV1D_PLUGIN=OFF \
  -DWITH_VVDEC=ON -DWITH_VVDEC_PLUGIN=OFF -DWITH_AOM_ENCODER=OFF -DWITH_AOM_DECODER=OFF \
  -DWITH_X265=OFF -DWITH_RAV1E=OFF -DWITH_SvtEnc=OFF -DWITH_GDK_PIXBUF=OFF -DWITH_LIBSHARPYUV=OFF
# cargo-c emits the complete Rust runtime into the archive and installs its .pc.
if [[ ! -d "$SOURCES/rav1e/.git" ]]; then git clone --depth 1 --branch v0.8.1 https://github.com/xiph/rav1e.git "$SOURCES/rav1e"; fi
(cd "$SOURCES/rav1e"; cargo cinstall --release --library-type staticlib --prefix "$PREFIX" --libdir lib)
