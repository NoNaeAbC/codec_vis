#!/bin/bash
# Build the locked sources matching the tested Arch packages. Build
# those libraries explicitly rather than falling back to shared codec libraries.
set -euo pipefail
PREFIX=${CODEC_VIS_PREFIX:-/opt/codec-vis}
SOURCES=${CODEC_VIS_SOURCES:-/opt/codec-vis-sources}
JOBS=${JOBS:-2}
export PKG_CONFIG_PATH="$PREFIX/lib/pkgconfig:$PREFIX/lib/x86_64-linux-gnu/pkgconfig"
export CMAKE_PREFIX_PATH="$PREFIX"
mkdir -p "$SOURCES/arch" "$PREFIX/lib"
source_package() {
  local name=$1 package=${2:-$1}
  local actual
  actual=$(python3 packaging/ci/fetch-static-source.py "$package")
  if [[ "$name" != "$package" ]]; then ln -sfn "$actual" "$SOURCES/arch/$name"; fi
}
cmake_static() {
  local name=$1; shift
  cmake -S "$SOURCES/arch/$name" -B "$SOURCES/arch/$name/build-static" -G Ninja \
    -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX="$PREFIX" -DCMAKE_INSTALL_LIBDIR=lib \
    -DCMAKE_POLICY_VERSION_MINIMUM=3.5 -DBUILD_SHARED_LIBS=OFF -DBUILD_TESTING=OFF "$@"
  cmake --build "$SOURCES/arch/$name/build-static" -j "$JOBS"
  cmake --install "$SOURCES/arch/$name/build-static"
}
meson_static() {
  local name=$1; shift
  local mode=()
  [[ ! -f "$SOURCES/arch/$name/build-static/meson-private/coredata.dat" ]] || mode=(--reconfigure)
  meson setup "${mode[@]}" "$SOURCES/arch/$name/build-static" "$SOURCES/arch/$name" \
    --prefix="$PREFIX" --libdir=lib --buildtype=release --default-library=static -Dauto_features=disabled "$@"
  meson compile -C "$SOURCES/arch/$name/build-static" -j "$JOBS"
  meson install -C "$SOURCES/arch/$name/build-static"
}
autotools_static() {
  local name=$1; shift
  (cd "$SOURCES/arch/$name"; autoreconf -fi; ./configure --prefix="$PREFIX" --disable-shared --enable-static "$@"; make -j "$JOBS"; make install)
}
# Foundation libraries are built from the same versions as the tested system.
source_package zlib
(cd "$SOURCES/arch/zlib"; ./configure --static --prefix="$PREFIX"; make -j "$JOBS"; make install)
source_package bzip2
make -C "$SOURCES/arch/bzip2" -j "$JOBS" libbz2.a
install -m644 "$SOURCES/arch/bzip2/libbz2.a" "$PREFIX/lib/"
source_package xz
autotools_static xz --disable-doc
source_package zstd
cmake -S "$SOURCES/arch/zstd/build/cmake" -B "$SOURCES/arch/zstd/build-static" -G Ninja \
  -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX="$PREFIX" -DCMAKE_INSTALL_LIBDIR=lib \
  -DZSTD_BUILD_PROGRAMS=OFF -DZSTD_BUILD_SHARED=OFF -DZSTD_BUILD_TESTS=OFF
cmake --build "$SOURCES/arch/zstd/build-static" -j "$JOBS"
cmake --install "$SOURCES/arch/zstd/build-static"
source_package brotli
cmake_static brotli -DBROTLI_DISABLE_TESTS=ON
source_package expat
cmake -S "$SOURCES/arch/expat/expat" -B "$SOURCES/arch/expat/build-static" -G Ninja \
  -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX="$PREFIX" -DCMAKE_INSTALL_LIBDIR=lib \
  -DEXPAT_SHARED_LIBS=OFF -DEXPAT_BUILD_TOOLS=OFF -DEXPAT_BUILD_TESTS=OFF -DEXPAT_BUILD_DOCS=OFF
cmake --build "$SOURCES/arch/expat/build-static" -j "$JOBS"
cmake --install "$SOURCES/arch/expat/build-static"
source_package libffi
autotools_static libffi --disable-docs
source_package png libpng
cmake_static png -DPNG_SHARED=OFF -DPNG_TESTS=OFF -DPNG_TOOLS=OFF
source_package jpeg libjpeg-turbo
cmake_static jpeg -DENABLE_SHARED=OFF -DENABLE_STATIC=ON -DWITH_TURBOJPEG=OFF
source_package lcms2
autotools_static lcms2 --without-jpeg --without-tiff
source_package freetype freetype2
cmake_static freetype
source_package fontconfig
meson_static fontconfig -Ddoc=disabled -Dtests=disabled -Dtools=disabled -Dxml-backend=expat
source_package openjpeg openjpeg2
cmake_static openjpeg -DBUILD_CODEC=OFF
source_package webp libwebp
cmake_static webp -DWEBP_BUILD_ANIM_UTILS=OFF -DWEBP_BUILD_CWEBP=OFF -DWEBP_BUILD_DWEBP=OFF \
  -DWEBP_BUILD_GIF2WEBP=OFF -DWEBP_BUILD_IMG2WEBP=OFF -DWEBP_BUILD_VWEBP=OFF \
  -DWEBP_BUILD_WEBPINFO=OFF -DWEBP_BUILD_WEBPMUX=OFF -DWEBP_BUILD_EXTRAS=OFF
source_package vpx libvpx
(cd "$SOURCES/arch/vpx"; ./configure --prefix="$PREFIX" --enable-static --disable-shared \
  --disable-examples --disable-tools --disable-docs --disable-unit-tests --enable-vp9-highbitdepth; make -j "$JOBS"; make install)
source_package raw libraw
autotools_static raw --disable-examples
source_package libunwind
autotools_static libunwind --disable-tests --disable-documentation
source_package libcap
make -C "$SOURCES/arch/libcap/libcap" -j "$JOBS" lib=lib
make -C "$SOURCES/arch/libcap/libcap" prefix="$PREFIX" lib=lib install
source_package systemd
meson_mode=()
[[ ! -f "$SOURCES/arch/systemd/build-static/meson-private/coredata.dat" ]] || meson_mode=(--reconfigure)
meson setup "${meson_mode[@]}" "$SOURCES/arch/systemd/build-static" "$SOURCES/arch/systemd" \
  --prefix="$PREFIX" --libdir=lib --buildtype=release -Dauto_features=disabled \
  -Dstatic-libsystemd=true -Dtests=false -Dman=false -Dhtml=false
meson compile -C "$SOURCES/arch/systemd/build-static" -j "$JOBS" libsystemd.a
install -m644 "$SOURCES/arch/systemd/build-static/libsystemd.a" "$PREFIX/lib/"
# systemd's installed public .pc omits the private dependencies of its archive.
# Supply the static library's enabled capability dependency explicitly.
printf 'prefix=%s\nlibdir=${prefix}/lib\nincludedir=/usr/include\nName: libsystemd\nDescription: systemd client library\nVersion: 262.1\nLibs: -L${libdir} -lsystemd\nLibs.private: -lcap -lrt\nCflags: -I${includedir}\n' "$PREFIX" > "$PREFIX/lib/pkgconfig/libsystemd.pc"
source_package x265
for depth in 10 12; do
  extra=(); [[ "$depth" != 12 ]] || extra=(-DMAIN12=ON)
  cmake -S "$SOURCES/arch/x265/source" -B "$SOURCES/arch/x265/build-$depth" -G Ninja \
    -DCMAKE_BUILD_TYPE=Release -DCMAKE_POLICY_VERSION_MINIMUM=3.5 -DENABLE_SHARED=OFF \
    -DENABLE_CLI=OFF -DEXPORT_C_API=OFF -DHIGH_BIT_DEPTH=ON "${extra[@]}"
  cmake --build "$SOURCES/arch/x265/build-$depth" -j "$JOBS"
done
cmake -S "$SOURCES/arch/x265/source" -B "$SOURCES/arch/x265/build-static" -G Ninja \
  -DCMAKE_BUILD_TYPE=Release -DCMAKE_POLICY_VERSION_MINIMUM=3.5 -DENABLE_SHARED=OFF \
  -DENABLE_CLI=OFF -DCMAKE_INSTALL_PREFIX="$PREFIX" -DCMAKE_INSTALL_LIBDIR=lib \
  -DLINKED_10BIT=ON -DLINKED_12BIT=ON \
  -DEXTRA_LIB="$SOURCES/arch/x265/build-10/libx265.a;$SOURCES/arch/x265/build-12/libx265.a"
cmake --build "$SOURCES/arch/x265/build-static" -j "$JOBS"
cmake --install "$SOURCES/arch/x265/build-static"
ar -M <<EOF
CREATE $PREFIX/lib/libx265.a
ADDLIB $SOURCES/arch/x265/build-static/libx265.a
ADDLIB $SOURCES/arch/x265/build-10/libx265.a
ADDLIB $SOURCES/arch/x265/build-12/libx265.a
SAVE
END
EOF
source_package wayland wayland
meson_static wayland -Ddocumentation=false -Dtests=false -Ddtd_validation=false
source_package harfbuzz harfbuzz
meson_static harfbuzz -Dtests=disabled -Ddocs=disabled -Dutilities=disabled -Dicu=disabled \
  -Dglib=disabled -Dgobject=disabled -Dcairo=disabled -Dfreetype=enabled -Dintrospection=disabled
source_package xkbcommon libxkbcommon
meson_static xkbcommon -Denable-x11=false -Denable-wayland=false -Denable-docs=false -Denable-tools=false -Denable-xkbregistry=false
source_package sdbus sdbus-cpp
cmake_static sdbus -DSDBUSCPP_BUILD_CODEGEN=OFF -DSDBUSCPP_BUILD_TESTS=OFF
source_package de265 libde265
cmake_static de265 -DENABLE_DECODER=OFF -DENABLE_SHERLOCK265=OFF -DENABLE_SDL=OFF
source_package dav1d dav1d
meson_static dav1d -Denable_tests=false -Denable_tools=false
source_package charls charls
cmake_static charls -DCHARLS_BUILD_TESTS=OFF -DCHARLS_BUILD_SAMPLES=OFF
source_package highway highway
cmake_static highway -DHWY_ENABLE_TESTS=OFF -DHWY_ENABLE_EXAMPLES=OFF -DHWY_ENABLE_CONTRIB=ON
source_package jxl libjxl
cmake_static jxl -DJPEGXL_ENABLE_TOOLS=OFF -DJPEGXL_ENABLE_DOXYGEN=OFF \
  -DJPEGXL_ENABLE_MANPAGES=OFF -DJPEGXL_ENABLE_JPEGLI=OFF -DJPEGXL_FORCE_SYSTEM_BROTLI=ON \
  -DJPEGXL_FORCE_SYSTEM_HWY=ON -DJPEGXL_ENABLE_BENCHMARK=OFF \
  -DJPEGXL_ENABLE_SKCMS=OFF -DJPEGXL_FORCE_SYSTEM_LCMS2=ON -DJPEGXL_ENABLE_SJPEG=OFF \
  -DJPEGXL_BUNDLE_LIBPNG=OFF -DJPEGXL_ENABLE_PLUGINS=OFF -DJPEGXL_ENABLE_VIEWERS=OFF
source_package x264 x264
(cd "$SOURCES/arch/x264"; ./configure --prefix="$PREFIX" --enable-static --disable-shared --disable-cli; make -j "$JOBS"; make install)
source_package svt-av1 svt-av1
cmake_static svt-av1 -DBUILD_APPS=OFF -DBUILD_TESTING=OFF
source_package kvazaar kvazaar
autotools_static kvazaar --without-cryptopp
source_package jxr jxrlib
(cd "$SOURCES/arch/jxr"; make -j "$JOBS")
find "$SOURCES/arch/jxr" -name 'libjpegxr.a' -exec install -m644 '{}' "$PREFIX/lib/" \;
find "$SOURCES/arch/jxr" -name 'libjxrglue.a' -exec install -m644 '{}' "$PREFIX/lib/" \;
source_package gst-source gstreamer
ln -sfn "$SOURCES/arch/gst-source/subprojects/gstreamer" "$SOURCES/arch/gst-core"
meson_static gst-core -Dauto_features=disabled -Dtests=disabled -Dexamples=disabled -Dbenchmarks=disabled -Dintrospection=disabled
ln -sfn "$SOURCES/arch/gst-source/subprojects/gst-plugins-bad" "$SOURCES/arch/gst-parsers"
meson_static gst-parsers -Dauto_features=disabled -Dtests=disabled -Dexamples=disabled -Dintrospection=disabled
source_package heif libheif
cmake_static heif -DWITH_EXAMPLES=OFF -DENABLE_PLUGIN_LOADING=OFF \
  -DWITH_LIBDE265=ON -DWITH_LIBDE265_PLUGIN=OFF -DWITH_DAV1D=ON -DWITH_DAV1D_PLUGIN=OFF \
  -DWITH_VVDEC=ON -DWITH_VVDEC_PLUGIN=OFF -DWITH_AOM_ENCODER=OFF -DWITH_AOM_DECODER=OFF \
  -DWITH_X265=OFF -DWITH_RAV1E=OFF -DWITH_SvtEnc=OFF -DWITH_GDK_PIXBUF=OFF -DWITH_LIBSHARPYUV=OFF
# cargo-c emits the complete Rust runtime into the archive and installs its .pc.
if [[ ! -d "$SOURCES/rav1e/.git" ]]; then git clone --depth 1 --branch v0.8.1 https://github.com/xiph/rav1e.git "$SOURCES/rav1e"; fi
(cd "$SOURCES/rav1e"; cargo cinstall --release --library-type staticlib --prefix "$PREFIX" --libdir lib)
