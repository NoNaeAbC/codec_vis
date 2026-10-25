#!/bin/bash
set -euo pipefail
# Fixed regular-repository snapshot matching the tested development system.
ARCH_SNAPSHOT=${ARCH_SNAPSHOT:-2026/10/24}
printf 'Server = https://archive.archlinux.org/repos/%s/$repo/os/$arch\n' "$ARCH_SNAPSHOT" > /etc/pacman.d/mirrorlist
pacman -Syyu --noconfirm --needed \
  base-devel cmake ninja meson pkgconf python git ca-certificates curl \
  rust cargo-c nasm yasm autoconf automake libtool gettext patchelf file binutils \
  vulkan-headers vulkan-icd-loader libva libdrm wayland wayland-protocols \
  harfbuzz freetype2 fontconfig libxkbcommon sdbus-cpp systemd \
  libjpeg-turbo libraw lcms2 libde265 dav1d charls libpng libjxl openjpeg2 \
  x264 x265 kvazaar libwebp libvpx svt-av1 rav1e jxrlib libheif numactl \
  gstreamer gst-plugins-base-libs glib2 libunwind \
  openssl zlib bzip2 xz zstd lz4 brotli expat libffi pcre2 libcap \
  shaderc ttf-dejavu github-cli gperf python-jinja python-pyelftools texinfo
printf 'int main() {}\n' | g++ -std=c++2c -x c++ -fsyntax-only -
python3 - <<'PY'
import json
import subprocess
expected = {'gcc': '16.2.1+r23+gd564253eb6c8-1', 'glibc': '2.44+r50+g1848099f063e-1'}
for entry in json.load(open('packaging/ci/arch-sources.json')).values():
    expected[entry['package']] = entry['version']
for name, version in expected.items():
    actual = subprocess.check_output(['pacman', '-Q', name], text=True).strip().split(' ', 1)[1]
    if actual != version:
        raise SystemExit(f'{name}: expected tested version {version}, found {actual}')
print('Compiler, libc, and source-library package versions match the locked baseline')
PY
