#!/bin/bash
# =============================================================================
# WINOME Portable Packager (MSYS2 UCRT64)
#
# Packages WINOME/build-release/src/winome.exe along with all transitive
# /ucrt64/bin runtime DLLs, gdk-pixbuf loaders (including SVG), GLib schemas,
# Adwaita symbolic icons, fontconfig config, licenses, and recovery scripts
# into a self-contained portable bundle and .zip archive.
#
# Usage:
#   ./WINOME/package.sh [--no-build] [--skip-zip] [--out-dir <dir>]
# =============================================================================

set -euo pipefail

export PATH=/ucrt64/bin:$PATH

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
PROJ="$ROOT/WINOME"

NO_BUILD=0
SKIP_ZIP=0
OUT_DIR="$PROJ/dist"

while [ $# -gt 0 ]; do
  case "$1" in
    --no-build)
      NO_BUILD=1
      shift
      ;;
    --skip-zip)
      SKIP_ZIP=1
      shift
      ;;
    --out-dir)
      if [ $# -lt 2 ]; then
        echo "Missing argument for --out-dir" >&2
        exit 2
      fi
      OUT_DIR="$2"
      shift 2
      ;;
    -h|--help)
      sed -n '2,14p' "$0"
      exit 0
      ;;
    *)
      echo "Unknown option: $1" >&2
      exit 2
      ;;
  esac
done

info() { printf '==> %s\n' "$*"; }
ok()   { printf ' [OK] %s\n' "$*"; }
fail() { printf ' [FAIL] %s\n' "$*" >&2; exit 1; }

EXE_SRC="$PROJ/build-release/src/winome.exe"
CTL_SRC="$PROJ/build-release/ctl/winome-ctl.exe"
if [ "$NO_BUILD" -eq 0 ]; then
  info "Building release binary via build.sh..."
  "$ROOT/build.sh" release
fi

[ -f "$EXE_SRC" ] || fail "Release binary not found: $EXE_SRC (run ./build.sh release first)"
[ -f "$CTL_SRC" ] || fail "Release CLI binary not found: $CTL_SRC (run ./build.sh release first)"

UCRT_PREFIX="/ucrt64"
[ -d "$UCRT_PREFIX/bin" ] || fail "MSYS2 UCRT64 toolchain directory not found: $UCRT_PREFIX"

BUNDLE_NAME="WINOME-portable"
BUNDLE_DIR="$OUT_DIR/$BUNDLE_NAME"
STAGE_DIR="$OUT_DIR/.${BUNDLE_NAME}-staging"
ZIP_PATH="$OUT_DIR/WINOME-win64-portable.zip"

cleanup_stage() {
  rm -rf "$STAGE_DIR" 2>/dev/null || true
}
trap cleanup_stage EXIT

info "Staging bundle via temporary directory $STAGE_DIR (preserving $BUNDLE_DIR in-place)..."
rm -rf "$STAGE_DIR"
mkdir -p "$STAGE_DIR" "$BUNDLE_DIR"

# 1. Main executable, winome-ctl.exe, and GLib/GTK Windows helper binaries
cp -f "$EXE_SRC" "$STAGE_DIR/winome.exe"
cp -f "$CTL_SRC" "$STAGE_DIR/winome-ctl.exe"
for helper in gdbus.exe gspawn-win64-helper.exe gspawn-win64-helper-console.exe; do
  if [ -f "$UCRT_PREFIX/bin/$helper" ]; then
    cp -f "$UCRT_PREFIX/bin/$helper" "$STAGE_DIR/$helper"
  fi
done

# 2. GdkPixbuf loaders (DLLs only, normalized relative loaders.cache)
PIXBUF_REL="lib/gdk-pixbuf-2.0/2.10.0"
PIXBUF_SRC="$UCRT_PREFIX/$PIXBUF_REL"
PIXBUF_DST="$STAGE_DIR/$PIXBUF_REL"
[ -d "$PIXBUF_SRC/loaders" ] || fail "Missing gdk-pixbuf loaders: $PIXBUF_SRC/loaders"

mkdir -p "$PIXBUF_DST/loaders"
LOADER_COUNT=0
for dll in "$PIXBUF_SRC/loaders/"*.dll; do
  [ -f "$dll" ] || continue
  cp -f "$dll" "$PIXBUF_DST/loaders/"
  LOADER_COUNT=$((LOADER_COUNT + 1))
done
[ "$LOADER_COUNT" -gt 0 ] || fail "No gdk-pixbuf loader DLLs copied"

if [ -f "$PIXBUF_SRC/loaders.cache" ]; then
  # Normalize any MSYS/Windows prefix lines while preserving relative loader paths.
  sed -E 's|"[A-Za-z]:\\\\[^"]*\\\\lib\\\\gdk-pixbuf-2\.0\\\\2\.10\.0\\\\loaders\\\\([^"]+)"|"lib\\\\gdk-pixbuf-2.0\\\\2.10.0\\\\loaders\\\\\1"|g' \
    "$PIXBUF_SRC/loaders.cache" > "$PIXBUF_DST/loaders.cache"
elif command -v gdk-pixbuf-query-loaders >/dev/null 2>&1; then
  gdk-pixbuf-query-loaders > "$PIXBUF_DST/loaders.cache"
else
  fail "Neither loaders.cache nor gdk-pixbuf-query-loaders is available"
fi

# 3. Transitive /ucrt64/bin/*.dll closure across winome.exe, helpers, and loaders
info "Resolving and copying transitive UCRT64 DLL dependencies..."
DLL_COUNT="$(python3 - "$(cygpath -m "$STAGE_DIR")" "$(cygpath -m "$UCRT_PREFIX/bin")" << 'PYEOF'
import os
import re
import shutil
import subprocess
import sys
from collections import deque

bundle_dir = sys.argv[1]
ucrt_bin_host = sys.argv[2].rstrip("/\\")

seeds = []
for root, _, files in os.walk(bundle_dir):
    for f in files:
        if f.lower().endswith((".exe", ".dll")):
            seeds.append(os.path.join(root, f))

seen = set()
copied_count = 0
queue = deque(seeds)
pat = re.compile(r"=>\s+/ucrt64/bin/([^\s]+\.dll)\b", re.IGNORECASE)

while queue:
    target = queue.popleft()
    try:
        out = subprocess.check_output(["ldd", target], text=True, stderr=subprocess.DEVNULL)
    except subprocess.CalledProcessError:
        continue
    for line in out.splitlines():
        m = pat.search(line)
        if not m:
            continue
        dll_name = m.group(1)
        host_path = os.path.join(ucrt_bin_host, dll_name)
        key = dll_name.lower()
        if key not in seen and os.path.isfile(host_path):
            seen.add(key)
            shutil.copy2(host_path, os.path.join(bundle_dir, dll_name))
            copied_count += 1
            queue.append(host_path)

print(copied_count, end="")
PYEOF
)"
DLL_COUNT="${DLL_COUNT//$'\r'/}"
[ "${DLL_COUNT:-0}" -gt 0 ] || fail "Failed to resolve any UCRT64 DLLs"
ok "Copied $DLL_COUNT UCRT64 runtime DLLs and $LOADER_COUNT gdk-pixbuf loaders"

# 4. GLib schemas (compile cleanly inside staging dir)
SCHEMAS_DST="$STAGE_DIR/share/glib-2.0/schemas"
mkdir -p "$SCHEMAS_DST"
if [ -d "$UCRT_PREFIX/share/glib-2.0/schemas" ]; then
  cp -f "$UCRT_PREFIX/share/glib-2.0/schemas/"*.xml "$SCHEMAS_DST/" 2>/dev/null || true
  cp -f "$UCRT_PREFIX/share/glib-2.0/schemas/gschema.dtd" "$SCHEMAS_DST/" 2>/dev/null || true
  if command -v glib-compile-schemas >/dev/null 2>&1; then
    glib-compile-schemas "$SCHEMAS_DST"
  elif [ -f "$UCRT_PREFIX/share/glib-2.0/schemas/gschemas.compiled" ]; then
    cp -f "$UCRT_PREFIX/share/glib-2.0/schemas/gschemas.compiled" "$SCHEMAS_DST/"
  fi
fi

# 5. Icon themes (Adwaita symbolic + scalable + hicolor index)
ADWAITA_SRC="$UCRT_PREFIX/share/icons/Adwaita"
ADWAITA_DST="$STAGE_DIR/share/icons/Adwaita"
mkdir -p "$ADWAITA_DST" "$STAGE_DIR/share/icons/hicolor"

for f in index.theme icon-theme.cache; do
  [ -f "$ADWAITA_SRC/$f" ] && cp -f "$ADWAITA_SRC/$f" "$ADWAITA_DST/"
done
for sub in symbolic scalable; do
  [ -d "$ADWAITA_SRC/$sub" ] && cp -r "$ADWAITA_SRC/$sub" "$ADWAITA_DST/"
done
if [ -f "$UCRT_PREFIX/share/icons/hicolor/index.theme" ]; then
  cp -f "$UCRT_PREFIX/share/icons/hicolor/index.theme" "$STAGE_DIR/share/icons/hicolor/"
fi

# 6. Fontconfig configuration
if [ -d "$UCRT_PREFIX/etc/fonts" ]; then
  mkdir -p "$STAGE_DIR/etc"
  cp -r "$UCRT_PREFIX/etc/fonts" "$STAGE_DIR/etc/"
fi

# 7. License notices & sample out-of-process stdio JSON plugin
[ -f "$ROOT/LICENSE" ] && cp -f "$ROOT/LICENSE" "$STAGE_DIR/LICENSE"
[ -f "$PROJ/THIRD-PARTY-NOTICES" ] && cp -f "$PROJ/THIRD-PARTY-NOTICES" "$STAGE_DIR/THIRD-PARTY-NOTICES"
if [ -d "$PROJ/examples/plugins" ]; then
  mkdir -p "$STAGE_DIR/examples"
  cp -r "$PROJ/examples/plugins" "$STAGE_DIR/examples/"
fi

# 8. Helper scripts and portable usage note (CRLF line endings for cmd.exe/Notepad)
printf '@echo off\r\n"%%~dp0winome.exe" --restore\r\n' > "$STAGE_DIR/restore-taskbar.bat"
printf '@echo off\r\n"%%~dp0winome.exe" --settings\r\n' > "$STAGE_DIR/open-settings.bat"

python3 - "$(cygpath -m "$STAGE_DIR/README.txt")" << 'PYEOF'
import pathlib, sys

text = (
    "WINOME Portable (Windows 10/11)\r\n"
    "===============================\r\n\r\n"
    "Quick Start:\r\n"
    "1. Right-click winome.exe and choose 'Run as administrator'\r\n"
    "   (required to hide the native Windows taskbar and manage virtual desktops).\r\n"
    "2. Press bare Win key or move pointer to the top-left hot corner to open Overview.\r\n"
    "3. Double-click open-settings.bat (or run 'winome.exe --settings') to configure WINOME.\r\n\r\n"
    "Recovery / CLI Commands:\r\n"
    "- restore-taskbar.bat        : Restore the native taskbar & work area (winome.exe --restore)\r\n"
    "- winome.exe --safe-mode     : Start without window theming overrides or external plugins\r\n"
    "- winome-ctl.exe status      : Un-elevated runtime control CLI (desktops, logs, plugins, themes)\r\n"
    "- winome.exe --show-overview : Toggle/show Activities Overview on the running instance\r\n"
    "- winome.exe --reload        : Reload config.json on the running instance\r\n"
    "- winome.exe --quit          : Gracefully exit WINOME and restore the taskbar\r\n\r\n"
    "Logs, Config & Plugins:\r\n"
    "- Config : %LOCALAPPDATA%\\WINOME\\config.json\r\n"
    "- Logs   : %LOCALAPPDATA%\\WINOME\\winome.log\r\n"
    "- Plugins: %LOCALAPPDATA%\\WINOME\\plugins\\<id>\\manifest.json (sample in examples\\plugins)\r\n"
    "- Dumps  : %LOCALAPPDATA%\\WINOME\\crashes\\\r\n"
)
pathlib.Path(sys.argv[1]).write_bytes(text.encode("utf-8"))
PYEOF

# 9. Verify critical bundle files in staging dir BEFORE syncing to BUNDLE_DIR
for req in \
  "$STAGE_DIR/winome.exe" \
  "$STAGE_DIR/winome-ctl.exe" \
  "$STAGE_DIR/libgtk-4-1.dll" \
  "$STAGE_DIR/libglib-2.0-0.dll" \
  "$STAGE_DIR/libcairo-2.dll" \
  "$STAGE_DIR/libstdc++-6.dll" \
  "$STAGE_DIR/libgcc_s_seh-1.dll" \
  "$STAGE_DIR/$PIXBUF_REL/loaders.cache" \
  "$STAGE_DIR/$PIXBUF_REL/loaders/pixbufloader_svg.dll" \
  "$SCHEMAS_DST/gschemas.compiled" \
  "$ADWAITA_DST/index.theme" \
  "$STAGE_DIR/etc/fonts/fonts.conf" \
  "$STAGE_DIR/restore-taskbar.bat"
do
  [ -f "$req" ] || fail "Missing required staged artifact: $req"
done

# Sync staged files into BUNDLE_DIR in-place without deleting BUNDLE_DIR itself
# (avoids EBUSY when Windows Explorer or a terminal has BUNDLE_DIR open).
cp -rf "$STAGE_DIR"/. "$BUNDLE_DIR"/

# Also populate runtime DLLs + assets next to build-release/src/winome.exe and
# build/src/winome.exe so running Release/Debug binaries directly from Explorer
# never fails with missing libcairo-2.dll / libstdc++-6.dll / libgcc_s_seh-1.dll.
for build_src_dir in "$PROJ/build-release/src" "$PROJ/build/src" "$PROJ/build-debug/src"; do
  if [ -d "$build_src_dir" ]; then
    cp -lf "$STAGE_DIR/"*.dll "$build_src_dir/" 2>/dev/null || cp -f "$STAGE_DIR/"*.dll "$build_src_dir/"
    for sub_asset in lib share etc; do
      if [ -d "$STAGE_DIR/$sub_asset" ]; then
        cp -rf "$STAGE_DIR/$sub_asset" "$build_src_dir/"
      fi
    done
  fi
done
rm -rf "$STAGE_DIR"

# Final check on BUNDLE_DIR
for req in \
  "$BUNDLE_DIR/winome.exe" \
  "$BUNDLE_DIR/winome-ctl.exe" \
  "$BUNDLE_DIR/libgtk-4-1.dll" \
  "$BUNDLE_DIR/libglib-2.0-0.dll" \
  "$BUNDLE_DIR/libcairo-2.dll" \
  "$BUNDLE_DIR/libstdc++-6.dll" \
  "$BUNDLE_DIR/libgcc_s_seh-1.dll" \
  "$BUNDLE_DIR/$PIXBUF_REL/loaders.cache" \
  "$BUNDLE_DIR/$PIXBUF_REL/loaders/pixbufloader_svg.dll" \
  "$BUNDLE_DIR/share/glib-2.0/schemas/gschemas.compiled" \
  "$BUNDLE_DIR/share/icons/Adwaita/index.theme" \
  "$BUNDLE_DIR/etc/fonts/fonts.conf" \
  "$BUNDLE_DIR/restore-taskbar.bat"
do
  [ -f "$req" ] || fail "Missing required final artifact in $BUNDLE_DIR: $req"
done

BUNDLE_SIZE="$(du -sh "$BUNDLE_DIR" | cut -f1)"
ok "Portable bundle staged: $BUNDLE_DIR ($BUNDLE_SIZE)"

if [ "$SKIP_ZIP" -eq 0 ]; then
  info "Creating zip archive $ZIP_PATH..."
  rm -f "$ZIP_PATH"
  python3 - "$(cygpath -m "$BUNDLE_DIR")" "$(cygpath -m "$ZIP_PATH")" << 'PYEOF'
import os
import sys
import zipfile

bundle_dir = os.path.abspath(sys.argv[1])
zip_path = os.path.abspath(sys.argv[2])
parent_dir = os.path.dirname(bundle_dir)

with zipfile.ZipFile(zip_path, "w", compression=zipfile.ZIP_DEFLATED, compresslevel=6) as zf:
    for root, _, files in os.walk(bundle_dir):
        for fname in sorted(files):
            full = os.path.join(root, fname)
            rel = os.path.relpath(full, parent_dir)
            zf.write(full, rel)
PYEOF
  ZIP_SIZE="$(du -sh "$ZIP_PATH" | cut -f1)"
  ok "Archive created: $ZIP_PATH ($ZIP_SIZE)"
fi
