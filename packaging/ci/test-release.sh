#!/bin/bash
set -euo pipefail
root=$(pwd)
work=$(mktemp -d)
trap 'if [[ -n ${compositor_pid:-} ]]; then kill "$compositor_pid" 2>/dev/null || true; fi; rm -rf "$work"' EXIT
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
export XDG_RUNTIME_DIR="$work/runtime"
mkdir -m700 "$XDG_RUNTIME_DIR"
weston --backend=headless --renderer=pixman --socket=codec-vis-ci --idle-time=0 --width=1280 --height=720 >weston.log 2>&1 &
compositor_pid=$!
for attempt in {1..100}; do
  [[ -S "$XDG_RUNTIME_DIR/codec-vis-ci" ]] && break
  kill -0 "$compositor_pid" 2>/dev/null || { cat weston.log; exit 1; }
  sleep .1
done
export WAYLAND_DISPLAY=codec-vis-ci
# Embedded fonts must work without a usable fontconfig installation.
printf '<fontconfig><dir>/does-not-exist</dir></fontconfig>\n' >empty-fonts.conf
export FONTCONFIG_FILE="$work/empty-fonts.conf"
./viewer --smoke-frames 2 --probe-image-pixels | tee "$root/dist/smoke-test.txt"
grep -q 'Image pixel probe: nonblank' "$root/dist/smoke-test.txt"
