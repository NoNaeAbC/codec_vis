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
# Exercise every software codec. In particular, x265's higher bit depths must
# work from static multilib archives without loading a companion codec .so.
# These use the same libraries and link flags as the released executable.
for spec in 1:10:420 1:12:420 2:10:420 3:10:420 6:10:420 7:10:420 \
  8:8:444 9:8:444 10:8:444 11:8:444 12:8:444 13:8:444 14:10:420 \
  15:10:420 16:8:420 17:8:444 18:8:420 19:10:420 20:8:420 21:10:420; do
  IFS=: read -r backend depth chroma <<< "$spec"
  timeout 120 "$root/build/test_codec_roundtrips" "$backend" "$depth" "$chroma" default 128 128
done
