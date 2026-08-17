// SPDX-License-Identifier: GPL-2.0-only
// Copyright (C) 2026 WINOME contributors
// GNOME-style Quick Settings panel (matches quickSettings.js structure).
//
// GNOME layout (QuickSettingsMenu, nColumns=2):
//   Row 0: system item (battery + screenshot/settings/lock/power buttons)
//   Row 1: volume slider (full width)
//   Row 2: brightness slider (full width)
//   Row 3+: toggles grid (2 columns): WLAN, Bluetooth, power mode, night
//          light, dark mode, do-not-disturb, keyboard, airplane mode.

#include "quick-settings.h"
#include "system-status.h"

#include <stdio.h>
#include <math.h>
#include <string.h>

// Minimal zh/en localization based on the system UI language.
static const char *
tr (const char *en, const char *zh)
{
  return strcmp (winome::get_system_language (), "zh") == 0 ? zh : en;
}

// A QuickToggle: icon + title (+ subtitle) + optional ">" menu arrow.
// checked state uses the GNOME accent background (purple/blue), unchecked is
// the dark grey capsule (from gnome-shell-dark.css).
static GtkWidget *
make_quick_toggle (const char *icon_name, const char *title,
                   const char *subtitle, bool has_menu)
{
  GtkWidget *button;
  GtkWidget *box;
  GtkWidget *icon;
  GtkWidget *title_box;
  GtkWidget *title_label;
  GtkWidget *subtitle_label;

  button = gtk_toggle_button_new ();
  gtk_widget_add_css_class (button, "quick-toggle");
  gtk_widget_add_css_class (button, "button");
  if (has_menu)
    gtk_widget_add_css_class (button, "quick-toggle-has-menu");
  gtk_widget_set_focus_on_click (button, FALSE);

  box = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 12);
  gtk_widget_set_valign (box, GTK_ALIGN_CENTER);
  gtk_button_set_child (GTK_BUTTON (button), box);

  icon = gtk_image_new_from_icon_name (icon_name);
  gtk_image_set_pixel_size (GTK_IMAGE (icon), 16);
  gtk_widget_add_css_class (icon, "quick-toggle-icon");
  gtk_widget_set_valign (icon, GTK_ALIGN_CENTER);
  gtk_box_append (GTK_BOX (box), icon);

  title_box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  gtk_widget_set_hexpand (title_box, TRUE);
  gtk_widget_set_valign (title_box, GTK_ALIGN_CENTER);
  gtk_box_append (GTK_BOX (box), title_box);

  title_label = gtk_label_new (title);
  gtk_widget_add_css_class (title_label, "quick-toggle-title");
  gtk_widget_set_halign (title_label, GTK_ALIGN_START);
  gtk_widget_set_valign (title_label, GTK_ALIGN_CENTER);
  gtk_box_append (GTK_BOX (title_box), title_label);

  if (subtitle != NULL) {
    subtitle_label = gtk_label_new (subtitle);
    gtk_widget_add_css_class (subtitle_label, "quick-toggle-subtitle");
    gtk_widget_set_halign (subtitle_label, GTK_ALIGN_START);
    gtk_widget_set_valign (subtitle_label, GTK_ALIGN_CENTER);
    gtk_box_append (GTK_BOX (title_box), subtitle_label);
  }

  if (has_menu) {
    GtkWidget *arrow = gtk_image_new_from_icon_name ("go-next-symbolic");
    gtk_image_set_pixel_size (GTK_IMAGE (arrow), 12);
    gtk_widget_add_css_class (arrow, "quick-toggle-arrow");
    gtk_widget_set_valign (arrow, GTK_ALIGN_CENTER);
    gtk_box_append (GTK_BOX (box), arrow);
  }

  return button;
}

// A QuickSlider: icon button + horizontal slider, matching QuickSlider.
// GNOME wraps the icon in a .icon-button.flat (round dark-grey button).
static GtkWidget *
make_quick_slider (const char *icon_name, GtkWidget **slider_out,
                   GtkWidget **icon_out)
{
  GtkWidget *box;
  GtkWidget *icon_button;
  GtkWidget *icon;
  GtkWidget *slider;

  box = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 12);
  gtk_widget_add_css_class (box, "quick-slider");

  icon_button = gtk_button_new ();
  gtk_widget_add_css_class (icon_button, "icon-button");
  gtk_widget_add_css_class (icon_button, "flat");
  gtk_widget_set_focus_on_click (icon_button, FALSE);

  icon = gtk_image_new_from_icon_name (icon_name);
  gtk_image_set_pixel_size (GTK_IMAGE (icon), 16);
  gtk_button_set_child (GTK_BUTTON (icon_button), icon);

  gtk_box_append (GTK_BOX (box), icon_button);

  slider = gtk_scale_new_with_range (GTK_ORIENTATION_HORIZONTAL, 0.0, 1.0, 0.05);
  gtk_widget_set_hexpand (slider, TRUE);
  gtk_scale_set_draw_value (GTK_SCALE (slider), FALSE);
  gtk_range_set_show_fill_level (GTK_RANGE (slider), TRUE);
  gtk_range_set_fill_level (GTK_RANGE (slider), 0.5);
  gtk_widget_add_css_class (slider, "quick-slider-scale");
  gtk_box_append (GTK_BOX (box), slider);

  if (slider_out)
    *slider_out = slider;
  if (icon_out)
    *icon_out = icon;

  return box;
}

static const char *
volume_icon_name (double volume, bool muted)
{
  static const char *icons[] = {
    "audio-volume-muted-symbolic",
    "audio-volume-low-symbolic",
    "audio-volume-medium-symbolic",
    "audio-volume-high-symbolic",
    "audio-volume-overamplified-symbolic",
  };

  if (muted || volume <= 0.0)
    return icons[0];

  int n = (int) ceil (3.0 * volume);
  if (n < 1)
    n = 1;
  if (n > 4)
    n = 4;
  return icons[n];
}

static const char *
battery_icon_name (const winome::BatteryStatus &battery)
{
  static char name[64];
  int level = battery.percentage / 10 * 10;
  if (level > 100)
    level = 100;
  const char *suffix = battery.charging ? "-charging-symbolic" : "-symbolic";
  snprintf (name, sizeof (name), "battery-level-%d%s", level, suffix);
  return name;
}

static void
on_volume_changed (GtkRange *range, gpointer G_GNUC_UNUSED user_data)
{
  winome::set_volume (gtk_range_get_value (range));
}

static void
on_lock_clicked (GtkButton *button G_GNUC_UNUSED, gpointer G_GNUC_UNUSED user_data)
{
  winome::lock_screen ();
}

GtkWidget *
winome_quick_settings_new (void)
{
  GtkWidget *root;
  GtkWidget *grid;
  GtkWidget *volume_slider, *volume_icon;
  GtkWidget *brightness_slider;
  GtkWidget *battery_label, *battery_icon;

  root = gtk_box_new (GTK_ORIENTATION_VERTICAL, 12);
  gtk_widget_add_css_class (root, "quick-settings");

  grid = gtk_grid_new ();
  gtk_grid_set_row_spacing (GTK_GRID (grid), 12);
  gtk_grid_set_column_spacing (GTK_GRID (grid), 12);
  gtk_widget_add_css_class (grid, "quick-settings-grid");
  gtk_box_append (GTK_BOX (root), grid);

  // --- Row 0: system item (battery + action buttons), full width ---
  {
    GtkWidget *row = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 12);
    gtk_widget_add_css_class (row, "quick-settings-system-item");

    GtkWidget *battery_btn = gtk_button_new ();
    gtk_widget_add_css_class (battery_btn, "icon-button");
    gtk_widget_add_css_class (battery_btn, "flat");
    gtk_widget_set_focus_on_click (battery_btn, FALSE);

    battery_icon = gtk_image_new_from_icon_name ("battery-level-100-symbolic");
    gtk_image_set_pixel_size (GTK_IMAGE (battery_icon), 16);
    gtk_button_set_child (GTK_BUTTON (battery_btn), battery_icon);
    gtk_box_append (GTK_BOX (row), battery_btn);

    battery_label = gtk_label_new ("100%");
    gtk_widget_add_css_class (battery_label, "power-item");
    gtk_box_append (GTK_BOX (row), battery_label);

    GtkWidget *spacer = gtk_label_new ("");
    gtk_widget_set_hexpand (spacer, TRUE);
    gtk_box_append (GTK_BOX (row), spacer);

    GtkWidget *btn;
    btn = gtk_button_new_from_icon_name ("screenshooter-symbolic");
    gtk_widget_add_css_class (btn, "icon-button");
    gtk_widget_set_focus_on_click (btn, FALSE);
    gtk_box_append (GTK_BOX (row), btn);

    btn = gtk_button_new_from_icon_name ("applications-system-symbolic");
    gtk_widget_add_css_class (btn, "icon-button");
    gtk_widget_set_focus_on_click (btn, FALSE);
    gtk_box_append (GTK_BOX (row), btn);

    btn = gtk_button_new_from_icon_name ("system-lock-screen-symbolic");
    gtk_widget_add_css_class (btn, "icon-button");
    gtk_widget_set_focus_on_click (btn, FALSE);
    g_signal_connect (btn, "clicked", G_CALLBACK (on_lock_clicked), NULL);
    gtk_box_append (GTK_BOX (row), btn);

    btn = gtk_button_new_from_icon_name ("system-shutdown-symbolic");
    gtk_widget_add_css_class (btn, "icon-button");
    gtk_widget_set_focus_on_click (btn, FALSE);
    gtk_box_append (GTK_BOX (row), btn);

    gtk_grid_attach (GTK_GRID (grid), row, 0, 0, 2, 1);
  }

  // --- Row 1: volume slider, full width ---
  {
    GtkWidget *row = make_quick_slider ("audio-volume-high-symbolic",
                                        &volume_slider, &volume_icon);
    gtk_grid_attach (GTK_GRID (grid), row, 0, 1, 2, 1);
  }

  // --- Row 2: brightness slider, full width ---
  {
    GtkWidget *row = make_quick_slider ("display-brightness-symbolic",
                                        &brightness_slider, NULL);
    gtk_grid_attach (GTK_GRID (grid), row, 0, 2, 2, 1);
  }

  // --- Row 3+: toggles grid (2 columns) ---
  {
    GtkWidget *wlan, *bluetooth, *powermode, *nightlight;
    GtkWidget *darkmode, *dnd, *keyboard, *airplane;
    int row = 3;

    wlan = make_quick_toggle ("network-wireless-symbolic",
                              tr ("Wi-Fi", "无线网络"),
                              "Connected", true);
    gtk_toggle_button_set_active (GTK_TOGGLE_BUTTON (wlan), TRUE);
    gtk_grid_attach (GTK_GRID (grid), wlan, 0, row, 1, 1);

    bluetooth = make_quick_toggle ("bluetooth-active-symbolic",
                                   tr ("Bluetooth", "蓝牙"),
                                   NULL, true);
    gtk_grid_attach (GTK_GRID (grid), bluetooth, 1, row, 1, 1);
    row++;

    powermode = make_quick_toggle ("power-profile-balanced-symbolic",
                                   tr ("Power Mode", "电源模式"),
                                   tr ("Balanced", "均衡"), true);
    gtk_grid_attach (GTK_GRID (grid), powermode, 0, row, 1, 1);

    nightlight = make_quick_toggle ("night-light-symbolic",
                                    tr ("Night Light", "夜间模式"),
                                    NULL, false);
    gtk_grid_attach (GTK_GRID (grid), nightlight, 1, row, 1, 1);
    row++;

    darkmode = make_quick_toggle ("weather-clear-night-symbolic",
                                  tr ("Dark Style", "深色主题"),
                                  NULL, false);
    gtk_toggle_button_set_active (GTK_TOGGLE_BUTTON (darkmode), TRUE);
    gtk_grid_attach (GTK_GRID (grid), darkmode, 0, row, 1, 1);

    dnd = make_quick_toggle ("notifications-disabled-symbolic",
                             tr ("Do Not Disturb", "勿扰模式"),
                             NULL, false);
    gtk_grid_attach (GTK_GRID (grid), dnd, 1, row, 1, 1);
    row++;

    keyboard = make_quick_toggle ("input-keyboard-symbolic",
                                  tr ("Keyboard", "键盘"),
                                  NULL, true);
    gtk_toggle_button_set_active (GTK_TOGGLE_BUTTON (keyboard), TRUE);
    gtk_grid_attach (GTK_GRID (grid), keyboard, 0, row, 1, 1);

    airplane = make_quick_toggle ("airplane-mode-symbolic",
                                  tr ("Airplane Mode", "飞行模式"),
                                  NULL, false);
    gtk_grid_attach (GTK_GRID (grid), airplane, 1, row, 1, 1);
  }

  // Volume slider writes to the system.
  g_signal_connect (volume_slider, "value-changed",
                    G_CALLBACK (on_volume_changed), NULL);

  g_object_set_data (G_OBJECT (root), "battery-label", battery_label);
  g_object_set_data (G_OBJECT (root), "battery-icon", battery_icon);
  g_object_set_data (G_OBJECT (root), "volume-slider", volume_slider);
  g_object_set_data (G_OBJECT (root), "volume-icon", volume_icon);

  winome_quick_settings_refresh (root);

  return root;
}

void
winome_quick_settings_refresh (GtkWidget *quick_settings)
{
  GtkWidget *battery_label =
      GTK_WIDGET (g_object_get_data (G_OBJECT (quick_settings), "battery-label"));
  GtkWidget *battery_icon =
      GTK_WIDGET (g_object_get_data (G_OBJECT (quick_settings), "battery-icon"));
  GtkWidget *volume_slider =
      GTK_WIDGET (g_object_get_data (G_OBJECT (quick_settings), "volume-slider"));
  GtkWidget *volume_icon =
      GTK_WIDGET (g_object_get_data (G_OBJECT (quick_settings), "volume-icon"));

  winome::BatteryStatus battery;
  if (battery_label && winome::get_battery_status (&battery)) {
    gchar text[16];
    snprintf (text, sizeof (text), "%d%%", battery.percentage);
    gtk_label_set_text (GTK_LABEL (battery_label), text);

    if (battery_icon)
      gtk_image_set_from_icon_name (GTK_IMAGE (battery_icon),
                                    battery_icon_name (battery));
  } else if (battery_label) {
    gtk_label_set_text (GTK_LABEL (battery_label), "");
  }

  double volume = 0.0;
  bool muted = false;
  if (volume_slider && winome::get_volume (&volume)) {
    g_signal_handlers_block_by_func (volume_slider,
        (gpointer) on_volume_changed, NULL);
    gtk_range_set_value (GTK_RANGE (volume_slider), volume);
    gtk_range_set_fill_level (GTK_RANGE (volume_slider), volume);
    g_signal_handlers_unblock_by_func (volume_slider,
        (gpointer) on_volume_changed, NULL);

    if (volume_icon) {
      winome::get_mute (&muted);
      gtk_image_set_from_icon_name (GTK_IMAGE (volume_icon),
                                    volume_icon_name (volume, muted));
    }
  }
}
