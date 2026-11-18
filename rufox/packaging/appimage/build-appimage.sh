#!/usr/bin/env bash
# Build and smoke-test a native Rufox AppImage on x86_64 or AArch64 Linux.
set -euo pipefail

SCRIPT_DIR=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
ROOT_DIR=$(cd -- "$SCRIPT_DIR/../.." && pwd)
REPO_ROOT=$(cd -- "$ROOT_DIR/.." && pwd)

case "$(uname -m)" in
  x86_64|amd64) ARCH=x86_64 ;;
  aarch64|arm64) ARCH=aarch64 ;;
  *) printf 'Unsupported architecture: %s\n' "$(uname -m)" >&2; exit 1 ;;
esac

WORK_DIR=${RUFOX_APPIMAGE_WORK:-"$ROOT_DIR/build-appimage-$ARCH"}
BUILD_DIR="$WORK_DIR/build"
APPDIR="$WORK_DIR/AppDir"
TOOLS_DIR="$WORK_DIR/tools"
OUT_DIR="$ROOT_DIR/out"
mkdir -p "$WORK_DIR" "$TOOLS_DIR" "$OUT_DIR"

fetch_tool() {
  local name=$1 url=$2 destination="$TOOLS_DIR/$1"
  if [[ ! -x "$destination" ]]; then
    if ! curl -fL --retry 3 --retry-delay 2 "$url" -o "$destination"; then
      rm -f "$destination"
      printf 'Failed to download %s\n' "$url" >&2
      return 1
    fi
    chmod +x "$destination"
  fi
  printf '%s\n' "$destination"
}

LINUXDEPLOY=$(fetch_tool "linuxdeploy-$ARCH.AppImage" \
  "https://github.com/linuxdeploy/linuxdeploy/releases/download/continuous/linuxdeploy-$ARCH.AppImage")
GTK_PLUGIN=$(fetch_tool "linuxdeploy-plugin-gtk.sh" \
  "https://raw.githubusercontent.com/linuxdeploy/linuxdeploy-plugin-gtk/master/linuxdeploy-plugin-gtk.sh")
APPIMAGETOOL=$(fetch_tool "appimagetool-$ARCH.AppImage" \
  "https://github.com/AppImage/appimagetool/releases/download/continuous/appimagetool-$ARCH.AppImage")
export PATH="$TOOLS_DIR:$PATH"
# linuxdeploy discovers plugins by their canonical, architecture-free name.
ln -sfn "$(basename "$GTK_PLUGIN")" "$TOOLS_DIR/linuxdeploy-plugin-gtk"

rm -rf "$BUILD_DIR" "$APPDIR"
meson setup "$BUILD_DIR" "$ROOT_DIR" \
  --prefix=/usr --libexecdir=libexec --buildtype=release \
  -Dgui=enabled -Dhotplug=enabled -Dudisks=enabled
meson compile -C "$BUILD_DIR"
FIXTURE="$WORK_DIR/rufox-fixture.iso"
sh "$ROOT_DIR/tests/make_fixture.sh" "$FIXTURE"
RUFOX_TEST_ISO="$FIXTURE" meson test -C "$BUILD_DIR" --print-errorlogs
DESTDIR="$APPDIR" meson install -C "$BUILD_DIR"

# Compile the schemas and validate the desktop/AppStream metadata before they
# become part of the release artifact.
glib-compile-schemas "$APPDIR/usr/share/glib-2.0/schemas"
desktop-file-validate "$APPDIR/usr/share/applications/rufox-gui.desktop"
appstreamcli validate --no-net "$APPDIR/usr/share/metainfo/io.github.rufox.metainfo.xml"
test -s "$APPDIR/usr/share/locale/ru/LC_MESSAGES/rufox.mo"

# pkexec clears LD_LIBRARY_PATH, so the privileged helper gets a small wrapper
# which reconstructs APPDIR from its own location instead of trusting the
# caller's environment.
mv "$APPDIR/usr/libexec/rufox-helper" "$APPDIR/usr/libexec/rufox-helper.bin"
install -m 0755 "$SCRIPT_DIR/rufox-helper.sh" "$APPDIR/usr/libexec/rufox-helper"
install -Dm 0644 "$REPO_ROOT/LICENSE" "$APPDIR/usr/share/doc/rufox/copyright"

export ARCH VERSION
VERSION=$(meson introspect "$BUILD_DIR" --projectinfo | python3 -c 'import json,sys; print(json.load(sys.stdin)["version"])')
"$LINUXDEPLOY" --appimage-extract-and-run \
  --appdir "$APPDIR" \
  --executable "$APPDIR/usr/bin/rufox-gui" \
  --executable "$APPDIR/usr/bin/rufox" \
  --executable "$APPDIR/usr/libexec/rufox-helper.bin" \
  --desktop-file "$APPDIR/usr/share/applications/rufox-gui.desktop" \
  --icon-file "$APPDIR/usr/share/icons/hicolor/256x256/apps/io.github.rufox.png" \
  --plugin gtk \
  --custom-apprun "$SCRIPT_DIR/AppRun"

# appimagetool needs a top-level raster icon to create .DirIcon.
install -m 0644 "$APPDIR/usr/share/icons/hicolor/256x256/apps/io.github.rufox.png" \
  "$APPDIR/io.github.rufox.png"
# AppRun points GIO_MODULE_DIR here so host modules cannot clash with the
# bundled GLib. The directory intentionally stays empty.
mkdir -p "$APPDIR/usr/lib/gio/modules"

OUTPUT="$OUT_DIR/Rufox-$VERSION-$ARCH.AppImage"
rm -f "$OUTPUT"
"$APPIMAGETOOL" --appimage-extract-and-run "$APPDIR" "$OUTPUT"
chmod +x "$OUTPUT"

# Exercise the actual packaged runtime, not just the build-tree binaries.
"$OUTPUT" --appimage-extract-and-run --cli list
"$OUTPUT" --appimage-extract-and-run --cli hash -i "$ROOT_DIR/data/io.github.rufox.svg" -H sha256
LANGUAGE=ru LANG=C.UTF-8 "$OUTPUT" --appimage-extract-and-run --help >"$WORK_DIR/help-ru.log"
grep -F 'Предварительно выбрать целевое устройство' "$WORK_DIR/help-ru.log"
if command -v xvfb-run >/dev/null; then
  GUI_LOG="$WORK_DIR/gui-smoke.log"
  set +e
  LANGUAGE=ru LANG=C.UTF-8 xvfb-run -a timeout 5s "$OUTPUT" --appimage-extract-and-run --dry-run >"$GUI_LOG" 2>&1
  GUI_RC=$?
  set -e
  cat "$GUI_LOG"
  if [[ $GUI_RC -ne 124 ]] || ! grep -F 'Запущен пробный режим' "$GUI_LOG" || \
      grep -Eiq 'critical|error|failed to load|undefined symbol|not found' "$GUI_LOG"; then
    printf 'Packaged Russian GUI smoke test failed (exit %s)\n' "$GUI_RC" >&2
    exit 1
  fi
fi
(
  cd "$OUT_DIR"
  sha256sum "$(basename "$OUTPUT")" > "$(basename "$OUTPUT").sha256"
)
printf '\nCreated %s\n' "$OUTPUT"
