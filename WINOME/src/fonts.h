// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 WINOME contributors
// Bundled Cantarell font support.

#pragma once

#include <gtk/gtk.h>

namespace winome {

// Extract the bundled Cantarell fonts (embedded in GResource) and give the
// given toplevel window its own fontconfig-based Pango font map so the panel
// text renders in Cantarell (GNOME's default UI font). Must be called before
// the window is shown.
void install_bundled_fonts (GtkWidget *window);

}  // namespace winome
