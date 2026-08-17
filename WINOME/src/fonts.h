// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 WINOME contributors
// Bundled Cantarell font support.

#pragma once

#include <gtk/gtk.h>

namespace winome {

// The shared fontconfig-based Pango font map with the bundled Cantarell fonts
// registered (created lazily on first use). The panel and its popovers are
// separate toplevel windows, so each must be given this map explicitly.
PangoFontMap *bundled_font_map (void);

// Apply the bundled Cantarell font map to the given toplevel window so its
// text renders in Cantarell (GNOME's default UI font). Must be called before
// the window is shown.
void install_bundled_fonts (GtkWidget *window);

}  // namespace winome
