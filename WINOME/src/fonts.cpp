// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 WINOME contributors
//
// Bundled Cantarell fonts.
//
// GTK4 on Windows uses the DirectWrite-based Win32 Pango backend, whose font
// collection is built once at process start and only refreshed with a slow
// re-scan (seconds per font). To render the panel in Cantarell without that
// penalty, the panel window gets its own fontconfig/FreeType-based font map:
// the bundled TTFs are registered with fontconfig (FcConfigAppFontAddFile) and
// the map's config is set to that config, so Cantarell resolves in
// milliseconds.

#include "fonts.h"

#include <fontconfig/fontconfig.h>
#include <pango/pangocairo.h>
#include <pango/pangofc-fontmap.h>

#include <cairo.h>

#include <string.h>

#include <windows.h>

namespace winome {

static const char *const kFontFiles[] = {
  "Cantarell-Regular-5.ttf",
  "Cantarell-Bold-2.ttf",
  "Cantarell-Oblique-4.ttf",
  "Cantarell-BoldOblique-3.ttf",
};

// Write a GResource item to a file under dir. Returns the file path
// (g_free() it) or NULL on failure.
static char *
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

  GError *error = NULL;
  if (!g_file_set_contents (path, static_cast<const char *> (g_bytes_get_data (bytes, NULL)),
                            g_bytes_get_size (bytes), &error)) {
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

// Pick a writable, ASCII-only directory for the extracted fonts. fontconfig's
// FreeType loader fails on non-ASCII paths (e.g. a Chinese username in the
// %TEMP% path), so the app's own directory is used.
static char *
fonts_cache_dir (void)
{
  wchar_t exe[MAX_PATH];
  if (GetModuleFileNameW (NULL, exe, MAX_PATH) == 0)
    return g_strdup ("winome-fonts");

  wchar_t *slash = wcsrchr (exe, L'\\');
  if (slash != NULL)
    *slash = 0;

  char *utf8 = g_utf16_to_utf8 (reinterpret_cast<const gunichar2 *> (exe), -1,
                                NULL, NULL, NULL);
  char *dir = g_build_filename (utf8, "winome-fonts", NULL);
  g_free (utf8);
  return dir;
}

PangoFontMap *
bundled_font_map (void)
{
  static PangoFontMap *map = NULL;
  static gboolean initialized = FALSE;

  if (initialized)
    return map;
  initialized = TRUE;

  char *dir = fonts_cache_dir ();
  g_mkdir_with_parents (dir, 0700);

  FcInit ();
  FcConfig *config = FcConfigGetCurrent ();

  for (int i = 0; i < (int) G_N_ELEMENTS (kFontFiles); i++) {
    char *res_path =
        g_strdup_printf ("/org/winome/theme/%s", kFontFiles[i]);
    char *file = write_resource_to_file (res_path, dir);
    g_free (res_path);

    if (file != NULL) {
      FcConfigAppFontAddFile (config,
                              reinterpret_cast<const FcChar8 *> (file));
      g_free (file);
    }
  }
  g_free (dir);

  // A fontconfig-based map renders via FreeType (cairo) instead of DirectWrite,
  // so it picks up the fonts above immediately, without a font-collection scan.
  map = pango_cairo_font_map_new_for_font_type (CAIRO_FONT_TYPE_FT);
  if (PANGO_IS_FC_FONT_MAP (map))
    pango_fc_font_map_set_config (PANGO_FC_FONT_MAP (map), config);

  return map;
}

void
install_bundled_fonts (GtkWidget *window)
{
  PangoFontMap *font_map = bundled_font_map ();
  if (font_map != NULL)
    gtk_widget_set_font_map (window, font_map);
}

}  // namespace winome
