#!/bin/bash
set -euo pipefail
root=$(pwd)
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
# Copy just the ELF, rename it, then run from an unrelated, empty directory.
install -m755 dist/codec_vis-linux-x86_64 "$work/viewer"
cd "$work"
unset CODEC_VIS_ASSET_DIR FONTCONFIG_FILE FONTCONFIG_PATH FONTCONFIG_SYSROOT LD_LIBRARY_PATH
./viewer --help
# Privileged helper dispatch must happen before touching any GUI/driver assets.
set +e
./viewer --energy-helper
status=$?
set -e
[[ "$status" == 2 ]]
# Hosted runners cannot promise FP16/color-managed Wayland presentation. Verify
# the real embedded shaders and both FT font faces without weakening the GUI.
printf '<fontconfig><dir>/does-not-exist</dir></fontconfig>\n' >empty-fonts.conf
export FONTCONFIG_FILE="$work/empty-fonts.conf"
./viewer --check-assets | tee "$root/dist/smoke-test.txt"
grep -q 'Embedded shaders and fonts verified' "$root/dist/smoke-test.txt"
# Exercise real software encoders/decoders, including AVM + actual TFLite and
# VVenC + VVdeC. These run without GPU hardware or a compositor.
for backend in 2 6 7; do
  "$root/build/test_codec_roundtrips" "$backend" 10 420 default 128 128
 done
