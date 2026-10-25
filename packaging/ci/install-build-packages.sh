#!/bin/bash
set -euo pipefail
export DEBIAN_FRONTEND=noninteractive
apt-get update
apt-get install -y --no-install-recommends \
  build-essential g++ cmake ninja-build meson pkg-config python3 git ca-certificates \
  curl nasm yasm autoconf automake libtool gettext patch patchelf file binutils \
  libvulkan-dev libva-dev libdrm-dev libwayland-dev wayland-protocols \
  libharfbuzz-dev libfreetype-dev libfontconfig-dev libxkbcommon-dev libsdbus-c++-dev \
  libsystemd-dev libjpeg62-turbo-dev libraw-dev liblcms2-dev libde265-dev \
  libdav1d-dev libcharls-dev libpng-dev libjxl-dev libopenjp2-7-dev libx264-dev \
  libx265-dev libkvazaar-dev libwebp-dev libvpx-dev libsvtav1enc-dev librav1e-dev libjxr-dev \
  libheif-dev libnuma-dev libgstreamer-plugins-bad1.0-dev libglib2.0-dev \
  libssl-dev zlib1g-dev libbz2-dev liblzma-dev libzstd-dev liblz4-dev \
  libbrotli-dev libexpat1-dev libffi-dev libpcre2-dev libcap-dev \
  glslc fonts-dejavu-core weston mesa-vulkan-drivers gh
# CI intentionally requires a modern C++26-capable toolchain.
printf 'int main() {}\n' | g++ -std=c++2c -x c++ -fsyntax-only -
