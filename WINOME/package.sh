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
ZIP_PATH="$OUT_DIR/WINOME-win64-portable.zip"

info "Staging bundle in $BUNDLE_DIR..."
rm -rf "$BUNDLE_DIR"
mkdir -p "$BUNDLE_DIR"

# 1. Main executable, winome-ctl.exe, and GLib/GTK Windows helper binaries
cp -f "$EXE_SRC" "$BUNDLE_DIR/winome.exe"
cp -f "$CTL_SRC" "$BUNDLE_DIR/winome-ctl.exe"
for helper in gdbus.exe gspawn-win64-helper.exe gspawn-win64-helper-console.exe; do
  if [ -f "$UCRT_PREFIX/bin/$helper" ]; then
    cp -f "$UCRT_PREFIX/bin/$helper" "$BUNDLE_DIR/$helper"
  fi
done

# 2. GdkPixbuf loaders (DLLs only, normalized relative loaders.cache)
PIXBUF_REL="lib/gdk-pixbuf-2.0/2.10.0"
PIXBUF_SRC="$UCRT_PREFIX/$PIXBUF_REL"
PIXBUF_DST="$BUNDLE_DIR/$PIXBUF_REL"
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
DLL_COUNT="$(python3 - "$(cygpath -m "$BUNDLE_DIR")" "$(cygpath -m "$UCRT_PREFIX/bin")" << 'PYEOF'
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
SCHEMAS_DST="$BUNDLE_DIR/share/glib-2.0/schemas"
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
ADWAITA_DST="$BUNDLE_DIR/share/icons/Adwaita"
mkdir -p "$ADWAITA_DST" "$BUNDLE_DIR/share/icons/hicolor"

for f in index.theme icon-theme.cache; do
  [ -f "$ADWAITA_SRC/$f" ] && cp -f "$ADWAITA_SRC/$f" "$ADWAITA_DST/"
done
for sub in symbolic scalable; do
  [ -d "$ADWAITA_SRC/$sub" ] && cp -r "$ADWAITA_SRC/$sub" "$ADWAITA_DST/"
done
if [ -f "$UCRT_PREFIX/share/icons/hicolor/index.theme" ]; then
  cp -f "$UCRT_PREFIX/share/icons/hicolor/index.theme" "$BUNDLE_DIR/share/icons/hicolor/"
fi

# 6. Fontconfig configuration
if [ -d "$UCRT_PREFIX/etc/fonts" ]; then
  mkdir -p "$BUNDLE_DIR/etc"
  cp -r "$UCRT_PREFIX/etc/fonts" "$BUNDLE_DIR/etc/"
fi

# 7. License notices & sample out-of-process stdio JSON plugin
[ -f "$ROOT/LICENSE" ] && cp -f "$ROOT/LICENSE" "$BUNDLE_DIR/LICENSE"
[ -f "$PROJ/THIRD-PARTY-NOTICES" ] && cp -f "$PROJ/THIRD-PARTY-NOTICES" "$BUNDLE_DIR/THIRD-PARTY-NOTICES"
if [ -d "$PROJ/examples/plugins" ]; then
  mkdir -p "$BUNDLE_DIR/examples"
  cp -r "$PROJ/examples/plugins" "$BUNDLE_DIR/examples/"
fi

# 8. Helper scripts and portable usage note (CRLF line endings for cmd.exe/Notepad)
printf '@echo off\r\n"%%~dp0winome.exe" --restore\r\n' > "$BUNDLE_DIR/restore-taskbar.bat"
printf '@echo off\r\n"%%~dp0winome.exe" --settings\r\n' > "$BUNDLE_DIR/open-settings.bat"

python3 - "$(cygpath -m "$BUNDLE_DIR/README.txt")" << 'PYEOF'
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

# 9. Verify critical bundle files before archiving
for req in \
  "$BUNDLE_DIR/winome.exe" \
  "$BUNDLE_DIR/winome-ctl.exe" \
  "$BUNDLE_DIR/libgtk-4-1.dll" \
  "$BUNDLE_DIR/libglib-2.0-0.dll" \
  "$BUNDLE_DIR/$PIXBUF_REL/loaders.cache" \
  "$BUNDLE_DIR/$PIXBUF_REL/loaders/pixbufloader_svg.dll" \
  "$SCHEMAS_DST/gschemas.compiled" \
  "$ADWAITA_DST/index.theme" \
  "$BUNDLE_DIR/etc/fonts/fonts.conf" \
  "$BUNDLE_DIR/restore-taskbar.bat"
do
  [ -f "$req" ] || fail "Missing required staged artifact: $req"
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
