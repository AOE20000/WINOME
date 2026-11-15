// SPDX-License-Identifier: GPL-2.0-only
// Copyright (C) 2026 WINOME contributors
//
// Bundled Cantarell fonts + isolated curated Windows CJK/symbol fallback.
//
// Why an isolated FcConfig instead of FcConfigGetCurrent()?
// Calling FcConfigGetCurrent() loads the default system fonts.conf and scans
// the entire C:\Windows\Fonts tree (~290+ font families), delaying cold start
// and retaining hundreds of megabytes of unused glyph/pattern metadata in
// Pango's font map. Conversely, bundling *only* Cantarell breaks Chinese/CJK
// and symbol glyphs inside GTK4 shell windows. Creating a dedicated FcConfig
// in memory with bundled Cantarell + a targeted list of existing Windows CJK,
// UI, Symbol, and Emoji font files resolves in milliseconds with full glyph
// coverage and minimal RSS.

#include "fonts.h"
#include "host-utils.h"
#include "perf-metrics.h"

#include <glib/gstdio.h>
#include <fontconfig/fontconfig.h>
#include <pango/pangocairo.h>
#include <pango/pangofc-fontmap.h>

#include <cairo.h>
#include <string.h>
#include <windows.h>

#include <string>

namespace winome {

namespace {

const char *const kFontFiles[] = {
  "Cantarell-Regular-5.ttf",
  "Cantarell-Bold-2.ttf",
  "Cantarell-Oblique-4.ttf",
  "Cantarell-BoldOblique-3.ttf",
};

// Curated Windows system font files registered directly by exact path so CJK,
// UI symbols, and emoji render cleanly without crawling C:\Windows\Fonts.
const wchar_t *const kFallbackSystemFontFiles[] = {
  L"msyh.ttc",      // Microsoft YaHei / Microsoft YaHei UI (SC)
  L"msyhbd.ttc",    // Microsoft YaHei Bold
  L"msjh.ttc",      // Microsoft JhengHei / UI (TC)
  L"simsun.ttc",    // SimSun fallback
  L"segoeui.ttf",   // Segoe UI
  L"segoeuib.ttf",  // Segoe UI Bold
  L"seguisym.ttf",  // Segoe UI Symbol
  L"seguiemj.ttf",  // Segoe UI Emoji
  L"YuGothR.ttc",   // Yu Gothic (JP)
  L"malgun.ttf",    // Malgun Gothic (KR)
};

// Minimal in-memory Fontconfig rules providing Cantarell -> CJK/Symbol/Emoji
// alias chains with crisp subpixel/grayscale hinting and NO <dir> crawl.
const char *const kIsolatedFontconfigXml = R"(<?xml version="1.0"?>
<!DOCTYPE fontconfig SYSTEM "urn:fontconfig:fonts.dtd">
<fontconfig>
  <match target="font">
    <edit name="antialias" mode="assign"><bool>true</bool></edit>
    <edit name="hinting" mode="assign"><bool>true</bool></edit>
    <edit name="hintstyle" mode="assign"><const>hintslight</const></edit>
    <edit name="rgba" mode="assign"><const>rgb</const></edit>
  </match>
  <alias binding="weak">
    <family>Cantarell</family>
    <default><family>Microsoft YaHei UI</family></default>
  </alias>
  <alias binding="weak">
    <family>sans-serif</family>
    <prefer>
      <family>Cantarell</family>
      <family>Microsoft YaHei UI</family>
      <family>Microsoft YaHei</family>
      <family>Microsoft JhengHei UI</family>
      <family>Segoe UI</family>
      <family>Segoe UI Symbol</family>
      <family>Segoe UI Emoji</family>
      <family>SimSun</family>
    </prefer>
  </alias>
</fontconfig>
)";

int g_loaded_files = 0;
double g_cold_init_ms = 0.0;

char *
write_resource_to_file (const char *resource_path, const char *dir)
{
  GBytes *bytes =
      g_resources_lookup_data (resource_path, G_RESOURCE_LOOKUP_FLAGS_NONE,
                               NULL);
  if (bytes == NULL)
    return NULL;

  const char *base = strrchr (resource_path, '/');
  base = (base != NULL) ? base + 1 : resource_path;
  char *path = g_build_filename (dir, base, NULL);

  // Skip overwriting identical bytes on warm startups.
  gsize target_sz = 0;
  const void *target_data = g_bytes_get_data (bytes, &target_sz);
  GStatBuf st {};
  if (g_stat (path, &st) == 0 && static_cast<gsize> (st.st_size) == target_sz) {
    g_bytes_unref (bytes);
    return path;
  }

  GError *error = NULL;
  if (!g_file_set_contents (path, static_cast<const char *> (target_data),
                            target_sz, &error)) {
    g_printerr ("fonts: failed to write %s: %s\n", path,
                error ? error->message : "unknown");
    g_clear_error (&error);
    g_free (path);
    g_bytes_unref (bytes);
    return NULL;
  }

  g_bytes_unref (bytes);
  return path;
}

char *
fonts_cache_dir (void)
{
  std::string base = winome::app_data_dir_ascii ();
  if (base.empty ())
    return g_strdup ("winome-fonts");
  return g_build_filename (base.c_str (), "fonts", NULL);
}

std::string
wide_path_to_ascii_or_utf8 (const std::wstring &w)
{
  if (w.empty ())
    return {};
  wchar_t short_buf[MAX_PATH] {};
  DWORD slen = GetShortPathNameW (w.c_str (), short_buf, MAX_PATH);
  const std::wstring &chosen =
      (slen > 0 && slen < MAX_PATH) ? std::wstring (short_buf, slen) : w;
  int need = WideCharToMultiByte (CP_UTF8, 0, chosen.c_str (), -1, nullptr, 0,
                                  nullptr, nullptr);
  if (need <= 1)
    return {};
  std::string out (static_cast<size_t> (need - 1), '\0');
  WideCharToMultiByte (CP_UTF8, 0, chosen.c_str (), -1, out.data (), need,
                       nullptr, nullptr);
  return out;
}

std::wstring
windows_fonts_dir (void)
{
  wchar_t win_dir[MAX_PATH] {};
  UINT n = GetSystemWindowsDirectoryW (win_dir, MAX_PATH);
  if (n == 0 || n >= MAX_PATH)
    return L"C:\\Windows\\Fonts";
  std::wstring dir (win_dir, n);
  if (dir.back () != L'\\')
    dir.push_back (L'\\');
  dir += L"Fonts";
  return dir;
}

int
count_config_families (FcConfig *cfg)
{
  if (cfg == nullptr)
    return 0;
  FcPattern *pat = FcPatternCreate ();
  FcObjectSet *os = FcObjectSetBuild (FC_FAMILY, nullptr);
  FcFontSet *fs = FcFontList (cfg, pat, os);
  int count = fs ? fs->nfont : 0;
  if (fs)
    FcFontSetDestroy (fs);
  if (os)
    FcObjectSetDestroy (os);
  if (pat)
    FcPatternDestroy (pat);
  return count;
}

}  // namespace

PangoFontMap *
bundled_font_map (void)
{
  static PangoFontMap *map = NULL;
  static gboolean initialized = FALSE;

  if (initialized)
    return map;
  initialized = TRUE;

  const gint64 t0 = g_get_monotonic_time ();

  char *dir = fonts_cache_dir ();
  g_mkdir_with_parents (dir, 0700);

  FcConfig *config = FcConfigCreate ();
  bool owns_config = (config != nullptr);
  bool isolated_ok = false;
  if (config != nullptr &&
      FcConfigParseAndLoadFromMemory (
          config,
          reinterpret_cast<const FcChar8 *> (kIsolatedFontconfigXml),
          FcTrue)) {
    isolated_ok = true;
  } else {
    if (config != nullptr) {
      FcConfigDestroy (config);
      owns_config = false;
    }
    FcInit ();
    config = FcConfigGetCurrent ();
  }

  int files_added = 0;
  for (int i = 0; i < (int) G_N_ELEMENTS (kFontFiles); i++) {
    char *res_path =
        g_strdup_printf ("/org/winome/theme/%s", kFontFiles[i]);
    char *file = write_resource_to_file (res_path, dir);
    g_free (res_path);

    if (file != NULL) {
      if (FcConfigAppFontAddFile (config,
                                  reinterpret_cast<const FcChar8 *> (file))) {
        ++files_added;
      }
      g_free (file);
    }
  }
  g_free (dir);

  // Add curated existing Windows system fonts for CJK, symbol, and emoji
  // fallback without invoking a slow full-directory Fontconfig scan.
  if (isolated_ok) {
    std::wstring wfonts = windows_fonts_dir ();
    for (const wchar_t *fname : kFallbackSystemFontFiles) {
      std::wstring full = wfonts + L"\\" + fname;
      if (GetFileAttributesW (full.c_str ()) != INVALID_FILE_ATTRIBUTES) {
        std::string u8 = wide_path_to_ascii_or_utf8 (full);
        if (!u8.empty () &&
            FcConfigAppFontAddFile (config,
                                    reinterpret_cast<const FcChar8 *> (
                                        u8.c_str ()))) {
          ++files_added;
        }
      }
    }
    FcConfigBuildFonts (config);
  }

  map = pango_cairo_font_map_new_for_font_type (CAIRO_FONT_TYPE_FT);
  if (PANGO_IS_FC_FONT_MAP (map))
    pango_fc_font_map_set_config (PANGO_FC_FONT_MAP (map), config);

  const int family_entries = count_config_families (config);
  if (owns_config)
    FcConfigDestroy (config);

  const double elapsed_ms =
      static_cast<double> (g_get_monotonic_time () - t0) / 1000.0;
  g_loaded_files = files_added;
  g_cold_init_ms = elapsed_ms;

  perf_record_fonts_stats (files_added, family_entries, elapsed_ms);
  capability_set (
      SubsystemId::Fonts,
      (files_added >= 4) ? CapabilityState::Active : CapabilityState::Degraded,
      isolated_ok ? "isolated fontconfig + curated CJK fallback"
                  : "fallback system fontconfig");

  g_print ("[perf] fonts ready in %.1fms (files=%d fontset_entries=%d mode=%s)\n",
           elapsed_ms, files_added, family_entries,
           isolated_ok ? "isolated" : "system");

  return map;
}

void
install_bundled_fonts (GtkWidget *window)
{
  PangoFontMap *font_map = bundled_font_map ();
  if (font_map != NULL)
    gtk_widget_set_font_map (window, font_map);
}

int
fonts_loaded_file_count (void)
{
  return g_loaded_files;
}

double
fonts_cold_init_ms (void)
{
  return g_cold_init_ms;
}

}  // namespace winome
