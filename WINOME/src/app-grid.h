// SPDX-License-Identifier: GPL-2.0-only
// Copyright (C) 2026 WINOME contributors
//
// In-host GNOME-style paginated App Grid overlay rendered inside the Overview
// when "Show Apps" is activated or searched.

#pragma once

#include <gtk/gtk.h>

#include <string>

namespace winome {

struct AppGridSlice {
  int page = 0;
  int total_pages = 1;
  int begin = 0;
  int end = 0;
};

struct AppGridDimensions {
  int cols = 6;
  int rows = 4;
  int page_size = 24;
  int icon_size = 64;
};

// Pure layout & pagination helpers (unit-tested in winome-tests).
int compute_app_grid_page_count (int total_items, int page_size);
AppGridSlice compute_app_grid_slice (int total_items, int page, int page_size);
AppGridDimensions compute_app_grid_dimensions (int avail_w, int avail_h);

// Widget construction & control (logical px for rebuild).
GtkWidget *app_grid_new (void (*on_launch_done) (void *),
                         void (*on_pins_changed) (void *), void *user_data);
void app_grid_set_query (GtkWidget *grid, const std::string &query_lower);
void app_grid_rebuild (GtkWidget *grid, int avail_w, int avail_h);
void app_grid_next_page (GtkWidget *grid);
void app_grid_prev_page (GtkWidget *grid);
void app_grid_first_page (GtkWidget *grid);
void app_grid_last_page (GtkWidget *grid);
void app_grid_reset_page (GtkWidget *grid);
void app_grid_refresh_running (GtkWidget *grid);
void app_grid_prewarm (GtkWidget *grid, int avail_w, int avail_h);
void app_grid_set_transition_opacity (GtkWidget *grid, bool frozen,
                                      double opacity);
bool app_grid_launch_first (GtkWidget *grid);
void app_grid_close_menu (GtkWidget *grid);
bool app_grid_close_open_folder (GtkWidget *grid);
void app_grid_set_folder_toggled_cb (GtkWidget *grid,
                                     void (*on_folder_toggled) (bool open, void *),
                                     void *user_data);
void app_grid_set_host_bounds (GtkWidget *grid, int mon_x_px, int mon_y_px,
                               int mon_w_px, int mon_h_px, int panel_h_px,
                               int dash_top_px);
void *app_grid_get_folder_hwnd (GtkWidget *grid);

}  // namespace winome
