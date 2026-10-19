#!/usr/bin/env bash
set -euo pipefail

ROOT=$(CDPATH= cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd -P)
ARCH=$(uname -m)
BUNDLE_NAME=${PORTABLE_BUNDLE_NAME:-codec_vis-linux-${ARCH}}
DIST_DIR=${PORTABLE_DIST_DIR:-"$ROOT/dist"}
APP_DIR="$DIST_DIR/$BUNDLE_NAME"
ARCHIVE="$DIST_DIR/$BUNDLE_NAME.tar.zst"

for tool in ldd patchelf readelf sed tar zstd pkg-config; do
	command -v "$tool" >/dev/null || {
		printf 'portable bundle: required tool is missing: %s\n' "$tool" >&2
		exit 1
	}
done

for file in codec_vis_gui codec_vis_cli codec_vis_energy_helper \
	build/compose.comp.spv build/linearize.comp.spv; do
	if [[ ! -f "$ROOT/$file" ]]; then
		printf 'portable bundle: build output is missing: %s\n' "$file" >&2
		exit 1
	fi
done

case "$APP_DIR" in
	"$DIST_DIR"/*) ;;
	*) printf 'portable bundle: refusing unsafe output path: %s\n' "$APP_DIR" >&2; exit 1 ;;
esac
rm -rf -- "$APP_DIR" "$ARCHIVE"
install -d "$APP_DIR/bin" "$APP_DIR/lib" "$APP_DIR/etc/fonts" \
	"$APP_DIR/share/codec_vis/shaders" "$APP_DIR/share/fonts" \
	"$APP_DIR/share/licenses" "$APP_DIR/var/cache/fontconfig"
install -m 0755 "$ROOT/codec_vis_gui" "$ROOT/codec_vis_cli" "$ROOT/codec_vis_energy_helper" "$APP_DIR/bin/"
install -m 0644 "$ROOT"/build/compose.comp.spv "$ROOT"/build/linearize.comp.spv "$APP_DIR/share/codec_vis/shaders/"
install -m 0644 "$ROOT/packaging/fonts.conf" "$APP_DIR/etc/fonts/fonts.conf"

declare -A COPIED_SOURCES=()
declare -A PACKAGES=()
declare -a QUEUE=("$ROOT/codec_vis_gui" "$ROOT/codec_vis_cli" "$ROOT/codec_vis_energy_helper")
MANIFEST_TMP=$(mktemp)
trap 'rm -f -- "$MANIFEST_TMP"' EXIT

elf_dependencies() {
	local elf=$1
	local output
	if ! output=$(ldd "$elf" 2>&1); then
		printf 'portable bundle: ldd failed for %s\n%s\n' "$elf" "$output" >&2
		return 1
	fi
	if grep -q 'not found' <<<"$output"; then
		printf 'portable bundle: unresolved dependency for %s\n%s\n' "$elf" "$output" >&2
		return 1
	fi
	awk '
		/=> \/[^ ]+/ { print $3; next }
		/^[[:space:]]*\/[^ ]+/ { print $1 }
	' <<<"$output"
}

queue_dependency() {
	local source=$1
	local real base destination
	real=$(readlink -f -- "$source")
	base=$(basename -- "$source")
	destination="$APP_DIR/lib/$base"
	if [[ -v "COPIED_SOURCES[$base]" ]]; then
		if [[ "${COPIED_SOURCES[$base]}" != "$real" ]] && ! cmp -s -- "$real" "$destination"; then
			printf 'portable bundle: conflicting libraries named %s: %s and %s\n' \
				"$base" "${COPIED_SOURCES[$base]}" "$real" >&2
			exit 1
		fi
		return
	fi
	install -m 0755 "$real" "$destination"
	COPIED_SOURCES[$base]=$real
	QUEUE+=("$real")

	local package='unmanaged'
	if command -v pacman >/dev/null 2>&1; then
		package=$(pacman -Qoq "$real" 2>/dev/null | head -n 1 || true)
		package=${package:-unmanaged}
		if [[ "$package" != unmanaged && ! -v "PACKAGES[$package]" ]]; then
			PACKAGES[$package]=1
			if [[ -d "/usr/share/licenses/$package" ]]; then
				cp -a "/usr/share/licenses/$package" "$APP_DIR/share/licenses/"
			fi
		fi
	fi
	printf '%s\t%s\t%s\n' "$base" "$package" "$(sha256sum "$real" | awk '{print $1}')" >>"$MANIFEST_TMP"
}

# libheif loads optional codec plugins with dlopen; ldd on the GUI cannot see them.
HEIF_PLUGIN_DIR=${PORTABLE_HEIF_PLUGIN_DIR:-"$(pkg-config --variable=libdir libheif)/libheif"}
install -d "$APP_DIR/lib/libheif"
if [[ -d "$HEIF_PLUGIN_DIR" ]]; then
	for plugin in "$HEIF_PLUGIN_DIR"/*.so; do
		[[ -f "$plugin" ]] || continue
		queue_dependency "$plugin"
		ln -s "../$(basename -- "$plugin")" "$APP_DIR/lib/libheif/$(basename -- "$plugin")"
	done
fi

for ((index = 0; index < ${#QUEUE[@]}; ++index)); do
	while IFS= read -r dependency; do
		[[ -n "$dependency" ]] && queue_dependency "$dependency"
	done < <(elf_dependencies "${QUEUE[index]}")
done

LOADER=''
for library in "${!COPIED_SOURCES[@]}"; do
	case "$library" in
		ld-linux*.so.*|ld-musl-*.so.*) LOADER=$library ;;
	esac
done
if [[ -z "$LOADER" ]]; then
	printf 'portable bundle: could not identify the bundled ELF loader\n' >&2
	exit 1
fi

for program in codec_vis_gui codec_vis_cli codec_vis_energy_helper; do
	patchelf --set-rpath '$ORIGIN/../lib' "$APP_DIR/bin/$program"
	sed -e "s|@LOADER@|$LOADER|g" -e "s|@PROGRAM@|$program|g" \
		"$ROOT/packaging/portable-launcher.sh.in" >"$APP_DIR/$program"
	chmod 0755 "$APP_DIR/$program"
done

declare -A COPIED_FONTS=()
if command -v fc-match >/dev/null 2>&1; then
	for pattern in sans 'Noto Sans' 'Noto Sans Symbols' 'DejaVu Sans' FreeSans; do
		font=$(fc-match -f '%{file}\n' "$pattern" 2>/dev/null || true)
		font=${font%%$'\n'*}
		if [[ -f "$font" ]]; then
			font_name=$(basename -- "$font")
			if [[ ! -v "COPIED_FONTS[$font_name]" ]]; then
				install -m 0644 "$font" "$APP_DIR/share/fonts/$font_name"
				COPIED_FONTS[$font_name]=$font
			fi
		fi
	done
fi
if (( ${#COPIED_FONTS[@]} == 0 )); then
	printf 'portable bundle: no UI font could be bundled\n' >&2
	exit 1
fi

{
	printf '# Bundled ELF libraries\n'
	printf '# name\tpackage\tsha256\n'
	sort -u "$MANIFEST_TMP"
	printf '\n# Bundled fonts\n'
	for font_name in "${!COPIED_FONTS[@]}"; do
		printf '%s\t%s\n' "$font_name" "$(sha256sum "${COPIED_FONTS[$font_name]}" | awk '{print $1}')"
	done | sort
} >"$APP_DIR/DEPENDENCIES.txt"

cat >"$APP_DIR/README.txt" <<'EOF'
codec_vis portable Linux bundle

Run:
  ./codec_vis_gui
  ./codec_vis_cli --help

The software codec libraries, C/C++ runtime, ELF loader, fonts, and Vulkan
shaders are included. The GUI still requires a Wayland session and a Vulkan
driver compatible with the target GPU. VA-API hardware encoding additionally
requires a matching VA driver on the target. Hardware drivers are deliberately
not bundled because they are kernel-, GPU-, and machine-specific; all software
backends remain self-contained.
EOF

for program in codec_vis_gui codec_vis_cli codec_vis_energy_helper; do
	loader_output=$("$APP_DIR/lib/$LOADER" --library-path "$APP_DIR/lib" \
		--list "$APP_DIR/bin/$program" 2>&1) || {
		printf 'portable bundle: loader validation failed for %s\n%s\n' \
			"$program" "$loader_output" >&2
		exit 1
	}
	if grep -q 'not found' <<<"$loader_output"; then
		printf 'portable bundle: unresolved packaged dependency for %s\n%s\n' \
			"$program" "$loader_output" >&2
		exit 1
	fi
done
env -i HOME="${TMPDIR:-/tmp}" PATH=/usr/bin:/bin \
	"$APP_DIR/codec_vis_cli" --help >/dev/null

tar --zstd -cf "$ARCHIVE" -C "$DIST_DIR" "$BUNDLE_NAME"
printf 'Portable bundle: %s\nArchive: %s\n' "$APP_DIR" "$ARCHIVE"
