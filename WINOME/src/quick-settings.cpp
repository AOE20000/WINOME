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
#include "easing.h"
#include "fonts.h"
#include "i18n.h"
#include "notifications.h"
#include "settings-window.h"
#include "st-engine.h"
#include "system-status.h"
#include "power-modes.h"
#include "wifi-networks.h"
#include "shell-panel.h"

#include <gdk/win32/gdkwin32.h>
#include <windows.h>
#include <shellapi.h>

#include <stdio.h>
#include <math.h>
#include <string.h>
#include <string>
#include <thread>

// Centralized zh/en localization respecting config overrides.
static const char *
tr (const char *en, const char *zh)
{
  return winome::tr (en, zh);
}

// $scalable_icon_size from gnome-shell-sass (1.091em at "Sans 11" -> 16px).
#define QUICK_ICON_SIZE 16

// $medium_scalable_icon_size = $scalable_icon_size * 1.5 -> 24px. Used by the
// QuickToggleMenu header badge (`.quick-toggle-menu .header .icon`).
#define QUICK_HEADER_ICON_SIZE 24

static GtkWidget *find_quick_settings_root (GtkWidget *widget);
static gboolean quick_settings_is_menu_open_for (GtkWidget *quick_settings,
                                                 GtkWidget *anchor);
static void quick_settings_toggle_inline_menu (GtkWidget *quick_settings,
                                               GtkWidget *anchor,
                                               GtkWidget *menu);

// --- QuickToggle -------------------------------------------------------------
//
// Standalone toggle (or non-toggling .power-item capsule), matching QuickToggle
// / QuickSettingsItem in quickSettings.js:
//   .quick-toggle.button
//     box  (St.BoxLayout, spacing 9px)
//       .quick-toggle-icon   (16px)
//       titleBox             (x_expand, vertical)
//         .quick-toggle-title
//         .quick-toggle-subtitle   (hidden when subtitle == NULL)
static GtkWidget *
create_toggle_capsule (bool toggle_mode,
                       const char *icon_name, const char *title,
                       const char *subtitle,
                       GtkWidget **icon_out,
                       GtkWidget **title_out,
                       GtkWidget **subtitle_out)
{
  GtkWidget *button;
  GtkWidget *box;
  GtkWidget *icon;
  GtkWidget *title_box;
  GtkWidget *title_label;
  GtkWidget *subtitle_label = NULL;

  button = toggle_mode ? gtk_toggle_button_new () : gtk_button_new ();
  gtk_widget_add_css_class (button, "quick-toggle");
  gtk_widget_add_css_class (button, "button");
  gtk_widget_set_focus_on_click (button, FALSE);

  box = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 9);
  gtk_widget_set_valign (box, GTK_ALIGN_CENTER);
  gtk_widget_set_halign (box, GTK_ALIGN_FILL);
  gtk_widget_set_hexpand (box, TRUE);
  gtk_button_set_child (GTK_BUTTON (button), box);

  icon = gtk_image_new_from_icon_name (icon_name);
  gtk_image_set_pixel_size (GTK_IMAGE (icon), QUICK_ICON_SIZE);
  gtk_widget_add_css_class (icon, "quick-toggle-icon");
  gtk_widget_set_valign (icon, GTK_ALIGN_CENTER);
  gtk_box_append (GTK_BOX (box), icon);

  title_box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  gtk_widget_set_hexpand (title_box, TRUE);
  gtk_widget_set_valign (title_box, GTK_ALIGN_CENTER);
  gtk_box_append (GTK_BOX (box), title_box);

  title_label = gtk_label_new (title);
  gtk_widget_add_css_class (title_label, "quick-toggle-title");
  gtk_label_set_ellipsize (GTK_LABEL (title_label), PANGO_ELLIPSIZE_END);
  gtk_widget_set_halign (title_label, GTK_ALIGN_START);
  gtk_widget_set_valign (title_label, GTK_ALIGN_CENTER);
  gtk_box_append (GTK_BOX (title_box), title_label);

  if (subtitle != NULL) {
    subtitle_label = gtk_label_new (subtitle);
    gtk_widget_add_css_class (subtitle_label, "quick-toggle-subtitle");
    gtk_label_set_ellipsize (GTK_LABEL (subtitle_label), PANGO_ELLIPSIZE_END);
    gtk_widget_set_halign (subtitle_label, GTK_ALIGN_START);
    gtk_widget_set_valign (subtitle_label, GTK_ALIGN_CENTER);
    gtk_box_append (GTK_BOX (title_box), subtitle_label);
  }

  if (icon_out)
    *icon_out = icon;
  if (title_out)
    *title_out = title_label;
  if (subtitle_out)
    *subtitle_out = subtitle_label;

  return button;
}

static GtkWidget *
make_quick_toggle (const char *icon_name, const char *title,
                   const char *subtitle,
                   GtkWidget **icon_out, GtkWidget **title_out)
{
  return create_toggle_capsule (true, icon_name, title, subtitle,
                                icon_out, title_out, NULL);
}

// --- QuickToggleMenu (inline accordion menu) ---------------------------------
//
// Expanded inline inside WinomeQuickSettingsGrid immediately under the row that
// triggered it, matching QuickToggleMenu in quickSettings.js:
//   .quick-toggle-menu
//     .header
//       .icon[.active]   24px glyph in a round badge
//       .title            %title_3
//       .subtitle         %caption_heading
//     ScrolledWindow (natural height up to 280px)
//       .popup-menu-item (12px radius, spacing 6px)

typedef struct {
  WinomeMenuItemCallback callback;
  gpointer               user_data;
} QuickMenuItemData;

// Build one `.popup-menu-item` row. Items with a callback become clickable.
static void
on_quick_menu_item_activated (GtkGestureClick *gesture, int n_press,
                              double x, double y, gpointer user_data)
{
  const QuickMenuItemData *data =
      static_cast<const QuickMenuItemData *> (user_data);

  (void)n_press;
  (void)x;
  (void)y;

  gtk_gesture_set_state (GTK_GESTURE (gesture), GTK_EVENT_SEQUENCE_CLAIMED);

  WinomeMenuItemCallback cb = data != NULL ? data->callback : NULL;
  gpointer ud = data != NULL ? data->user_data : NULL;
  GtkWidget *item =
      gtk_event_controller_get_widget (GTK_EVENT_CONTROLLER (gesture));
  GtkWidget *qs = find_quick_settings_root (item);

  // Invoke callback before closing so row metadata stays live during cb().
  if (cb != NULL)
    cb (ud);

  if (qs != NULL)
    winome_quick_settings_close_menu (qs, TRUE);
}

static GtkWidget *
make_quick_menu_item (const WinomeMenuEntry *entry)
{
  if (entry->label == NULL) {
    GtkWidget *sep_row = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_add_css_class (sep_row, "popup-separator-menu-item");
    gtk_widget_set_margin_top (sep_row, 6);
    gtk_widget_set_margin_bottom (sep_row, 6);
    gtk_widget_set_margin_start (sep_row, 12);
    gtk_widget_set_margin_end (sep_row, 12);

    GtkWidget *sep_line = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_add_css_class (sep_line, "popup-separator-menu-item-separator");
    gtk_widget_set_hexpand (sep_line, TRUE);
    gtk_widget_set_valign (sep_line, GTK_ALIGN_CENTER);
    gtk_box_append (GTK_BOX (sep_row), sep_line);
    return sep_row;
  }

  GtkWidget *item;
  GtkWidget *box;
  GtkWidget *icon = NULL;
  GtkWidget *label;

  item = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 12);
  gtk_widget_add_css_class (item, "popup-menu-item");
  gtk_widget_set_focus_on_click (item, FALSE);

  box = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 6);
  gtk_widget_set_valign (box, GTK_ALIGN_CENTER);
  gtk_widget_set_halign (box, GTK_ALIGN_FILL);
  gtk_widget_set_hexpand (box, TRUE);
  gtk_box_append (GTK_BOX (item), box);

  if (entry->icon_name != NULL) {
    icon = gtk_image_new_from_icon_name (entry->icon_name);
    gtk_image_set_pixel_size (GTK_IMAGE (icon), QUICK_ICON_SIZE);
    gtk_widget_add_css_class (icon, "popup-menu-icon");
    gtk_widget_set_valign (icon, GTK_ALIGN_CENTER);
    gtk_box_append (GTK_BOX (box), icon);
  }

  label = gtk_label_new (entry->label);
  gtk_label_set_ellipsize (GTK_LABEL (label), PANGO_ELLIPSIZE_END);
  gtk_label_set_xalign (GTK_LABEL (label), 0.0);
  gtk_widget_set_valign (label, GTK_ALIGN_CENTER);
  gtk_widget_set_hexpand (label, TRUE);
  gtk_box_append (GTK_BOX (box), label);

  // Radio-style active marker, matching `.popup-menu-ornament` upstream.
  if (entry->checked) {
    GtkWidget *ornament =
        gtk_image_new_from_icon_name ("ornament-check-symbolic");
    gtk_image_set_pixel_size (GTK_IMAGE (ornament), QUICK_ICON_SIZE);
    gtk_widget_add_css_class (ornament, "popup-menu-ornament");
    gtk_widget_set_valign (ornament, GTK_ALIGN_CENTER);
    gtk_box_append (GTK_BOX (item), ornament);
  }

  if (entry->callback != NULL) {
    QuickMenuItemData *data = g_new0 (QuickMenuItemData, 1);
    data->callback = entry->callback;
    data->user_data = entry->user_data;
    // Tie lifetime to the menu item widget; callers often pass stack-allocated
    // WinomeMenuEntry arrays when constructing the menu.
    g_object_set_data_full (G_OBJECT (item), "menu-item-data", data, g_free);

    GtkGesture *click = gtk_gesture_click_new ();

    gtk_gesture_single_set_button (GTK_GESTURE_SINGLE (click), 0);
    g_signal_connect (click, "pressed",
                      G_CALLBACK (on_quick_menu_item_activated), data);
    gtk_widget_add_controller (item, GTK_EVENT_CONTROLLER (click));
  }

  return item;
}

static GtkWidget *
build_quick_menu_header (const char *icon_name, const char *title,
                         const char *subtitle, gboolean active)
{
  GtkWidget *header;
  GtkWidget *row;
  GtkWidget *text_col;
  GtkWidget *icon = NULL;
  GtkWidget *title_label = NULL;
  GtkWidget *subtitle_label = NULL;

  if (icon_name == NULL)
    return NULL;

  header = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  gtk_widget_add_css_class (header, "header");

  row = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 12);
  gtk_widget_set_valign (row, GTK_ALIGN_CENTER);

  icon = gtk_image_new_from_icon_name (icon_name);
  gtk_image_set_pixel_size (GTK_IMAGE (icon), QUICK_HEADER_ICON_SIZE);
  gtk_widget_add_css_class (icon, "icon");
  if (active)
    gtk_widget_add_css_class (icon, "active");
  gtk_widget_set_valign (icon, GTK_ALIGN_CENTER);
  gtk_box_append (GTK_BOX (row), icon);

  text_col = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  gtk_widget_set_valign (text_col, GTK_ALIGN_CENTER);
  gtk_widget_set_hexpand (text_col, TRUE);

  title_label = gtk_label_new (title != NULL ? title : "");
  gtk_widget_add_css_class (title_label, "title");
  gtk_label_set_ellipsize (GTK_LABEL (title_label), PANGO_ELLIPSIZE_END);
  gtk_label_set_xalign (GTK_LABEL (title_label), 0.0);
  gtk_box_append (GTK_BOX (text_col), title_label);

  if (subtitle != NULL && subtitle[0] != '\0') {
    subtitle_label = gtk_label_new (subtitle);
    gtk_widget_add_css_class (subtitle_label, "subtitle");
    gtk_label_set_ellipsize (GTK_LABEL (subtitle_label), PANGO_ELLIPSIZE_END);
    gtk_label_set_xalign (GTK_LABEL (subtitle_label), 0.0);
    gtk_box_append (GTK_BOX (text_col), subtitle_label);
  }

  gtk_box_append (GTK_BOX (row), text_col);
  gtk_box_append (GTK_BOX (header), row);

  return header;
}

// Build the card content for an inline QuickToggleMenu drawer.
static GtkWidget *
build_quick_menu_full (const WinomeMenuEntry *entries, guint n_entries,
                       const char *header_icon, const char *header_title,
                       const char *header_subtitle, gboolean header_active)
{
  GtkWidget *menu;
  GtkWidget *header;
  guint i;

  menu = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  gtk_widget_add_css_class (menu, "quick-toggle-menu");
  gtk_widget_set_hexpand (menu, TRUE);

  header = build_quick_menu_header (header_icon, header_title,
                                    header_subtitle, header_active);
  if (header != NULL)
    gtk_box_append (GTK_BOX (menu), header);

  if (n_entries > 0) {
    GtkWidget *scroll = gtk_scrolled_window_new ();
    gtk_scrolled_window_set_policy (GTK_SCROLLED_WINDOW (scroll),
                                    GTK_POLICY_NEVER,
                                    GTK_POLICY_AUTOMATIC);
    gtk_scrolled_window_set_propagate_natural_width (
        GTK_SCROLLED_WINDOW (scroll), TRUE);
    gtk_scrolled_window_set_propagate_natural_height (
        GTK_SCROLLED_WINDOW (scroll), TRUE);
    gtk_scrolled_window_set_max_content_height (
        GTK_SCROLLED_WINDOW (scroll), 280);

    GtkWidget *items_box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
    for (i = 0; i < n_entries; i++) {
      GtkWidget *item = make_quick_menu_item (&entries[i]);
      gtk_box_append (GTK_BOX (items_box), item);
    }
    gtk_scrolled_window_set_child (GTK_SCROLLED_WINDOW (scroll), items_box);
    gtk_box_append (GTK_BOX (menu), scroll);
  }

  return menu;
}

static GtkWidget *
build_quick_menu (const WinomeMenuEntry *entries, guint n_entries)
{
  return build_quick_menu_full (entries, n_entries, NULL, NULL, NULL, FALSE);
}

GtkWidget *
winome_quick_menu_new_full (const WinomeMenuEntry *entries, guint n_entries,
                            const char *header_icon,
                            const char *header_title,
                            const char *header_subtitle,
                            gboolean header_active)
{
  g_return_val_if_fail (entries != NULL || n_entries == 0, NULL);
  return build_quick_menu_full (entries, n_entries, header_icon,
                                header_title, header_subtitle, header_active);
}

GtkWidget *
winome_quick_menu_new (const WinomeMenuEntry *entries, guint n_entries)
{
  g_return_val_if_fail (entries != NULL || n_entries == 0, NULL);
  return build_quick_menu (entries, n_entries);
}

// --- QuickMenuToggle ---------------------------------------------------------
//
// Split pill toggle matching QuickMenuToggle in upstream quickSettings.js and
// GTK4's split-button pattern (outer GtkBox holding independent toggle +
// separator + arrow buttons):
//   .quick-toggle-has-menu        (outer GtkBox container)
//     .quick-toggle.button        (GtkToggleButton: icon + title + subtitle)
//     .quick-toggle-separator     (GtkSeparator, 1px vertical)
//     .quick-toggle-menu-button   (GtkButton.icon-button: go-next-symbolic)
//
// When @menu_builder is NULL, the separator and arrow button are omitted so
// `.quick-toggle:last-child` rounds all four capsule corners automatically.

// Builds the inline drawer widget for a toggle's arrow. Returns a new widget
// or NULL when no items are available.
typedef GtkWidget *(*QuickMenuBuilder) (gpointer user_data);

typedef struct {
  GtkWidget        *container;        // outer .quick-toggle-has-menu box
  GtkWidget        *toggle_part;      // left GtkToggleButton (.quick-toggle)
  GtkWidget        *menu_button_part; // right GtkButton (.quick-toggle-menu-button), may be NULL
  GtkWidget        *icon;             // icon image inside toggle_part
  GtkWidget        *subtitle_label;   // optional subtitle label, may be NULL
  QuickMenuBuilder  menu_builder;
  gpointer          menu_builder_data;
} MenuToggleParts;

static void
sync_menu_toggle_checked (GtkToggleButton *button, gpointer user_data)
{
  MenuToggleParts *parts = static_cast<MenuToggleParts *> (user_data);
  if (gtk_toggle_button_get_active (button)) {
    gtk_widget_set_state_flags (parts->container,
                                GTK_STATE_FLAG_CHECKED, FALSE);
    if (parts->menu_button_part != NULL) {
      gtk_widget_set_state_flags (parts->menu_button_part,
                                  GTK_STATE_FLAG_CHECKED, FALSE);
    }
  } else {
    gtk_widget_unset_state_flags (parts->container,
                                  GTK_STATE_FLAG_CHECKED);
    if (parts->menu_button_part != NULL) {
      gtk_widget_unset_state_flags (parts->menu_button_part,
                                    GTK_STATE_FLAG_CHECKED);
    }
  }
}

static void
on_menu_button_clicked (GtkButton *button G_GNUC_UNUSED, gpointer user_data)
{
  MenuToggleParts *parts = static_cast<MenuToggleParts *> (user_data);
  if (parts == NULL || parts->menu_builder == NULL)
    return;

  GtkWidget *qs = find_quick_settings_root (parts->container);
  if (qs == NULL)
    return;

  if (quick_settings_is_menu_open_for (qs, parts->container)) {
    winome_quick_settings_close_menu (qs, TRUE);
    return;
  }

  GtkWidget *menu = parts->menu_builder (parts->menu_builder_data);
  if (menu != NULL)
    quick_settings_toggle_inline_menu (qs, parts->container, menu);
}

static GtkWidget *
make_quick_menu_toggle (const char *icon_name, const char *title,
                        const char *subtitle,
                        QuickMenuBuilder menu_builder,
                        gpointer menu_builder_data)
{
  GtkWidget *container;
  GtkWidget *toggle_part;
  GtkWidget *icon = NULL;
  GtkWidget *subtitle_label = NULL;
  GtkWidget *separator = NULL;
  GtkWidget *menu_button = NULL;
  MenuToggleParts *parts;

  container = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);
  gtk_widget_add_css_class (container, "quick-toggle-has-menu");
  gtk_widget_set_overflow (container, GTK_OVERFLOW_HIDDEN);

  toggle_part = create_toggle_capsule (true, icon_name, title, subtitle,
                                       &icon, NULL, &subtitle_label);
  gtk_widget_set_hexpand (toggle_part, TRUE);
  gtk_widget_set_valign (toggle_part, GTK_ALIGN_FILL);
  gtk_box_append (GTK_BOX (container), toggle_part);

  parts = g_new0 (MenuToggleParts, 1);
  parts->container = container;
  parts->toggle_part = toggle_part;
  parts->icon = icon;
  parts->subtitle_label = subtitle_label;
  parts->menu_builder = menu_builder;
  parts->menu_builder_data = menu_builder_data;

  if (menu_builder != NULL) {
    separator = gtk_separator_new (GTK_ORIENTATION_VERTICAL);
    gtk_widget_add_css_class (separator, "quick-toggle-separator");
    gtk_widget_set_valign (separator, GTK_ALIGN_FILL);
    gtk_box_append (GTK_BOX (container), separator);

    menu_button = gtk_button_new ();
    gtk_widget_add_css_class (menu_button, "quick-toggle-menu-button");
    gtk_widget_add_css_class (menu_button, "icon-button");
    gtk_widget_set_focus_on_click (menu_button, FALSE);
    gtk_widget_set_valign (menu_button, GTK_ALIGN_FILL);

    GtkWidget *arrow = gtk_image_new_from_icon_name ("go-next-symbolic");
    gtk_image_set_pixel_size (GTK_IMAGE (arrow), QUICK_ICON_SIZE);
    gtk_widget_set_valign (arrow, GTK_ALIGN_CENTER);
    gtk_widget_set_halign (arrow, GTK_ALIGN_CENTER);
    gtk_button_set_child (GTK_BUTTON (menu_button), arrow);
    gtk_box_append (GTK_BOX (container), menu_button);

    parts->menu_button_part = menu_button;
    g_signal_connect (menu_button, "clicked",
                      G_CALLBACK (on_menu_button_clicked), parts);
  }

  g_object_set_data_full (G_OBJECT (container), "menu-toggle-parts",
                          parts, g_free);
  g_signal_connect (toggle_part, "toggled",
                    G_CALLBACK (sync_menu_toggle_checked), parts);
  sync_menu_toggle_checked (GTK_TOGGLE_BUTTON (toggle_part), parts);

  return container;
}

static GtkWidget *
quick_menu_toggle_button (GtkWidget *widget)
{
  MenuToggleParts *parts = static_cast<MenuToggleParts *> (
      g_object_get_data (G_OBJECT (widget), "menu-toggle-parts"));
  return parts != NULL ? parts->toggle_part : widget;
}

static void
set_toggle_active_blocked (GtkWidget *widget, gboolean active,
                           GCallback blocked_cb, gpointer cb_data)
{
  GtkWidget *btn = quick_menu_toggle_button (widget);
  if (blocked_cb != NULL)
    g_signal_handlers_block_by_func (btn, (gpointer) blocked_cb, cb_data);
  gtk_toggle_button_set_active (GTK_TOGGLE_BUTTON (btn), active);
  if (blocked_cb != NULL)
    g_signal_handlers_unblock_by_func (btn, (gpointer) blocked_cb, cb_data);

  MenuToggleParts *parts = static_cast<MenuToggleParts *> (
      g_object_get_data (G_OBJECT (widget), "menu-toggle-parts"));
  if (parts != NULL)
    sync_menu_toggle_checked (GTK_TOGGLE_BUTTON (btn), parts);
}

// --- QuickSlider -------------------------------------------------------------
//
// Matching QuickSlider in quickSettings.js:
//   .quick-slider (St.BoxLayout, spacing 6px)
//     .icon-button.flat       (16px icon button)
//     .slider-bin             (wraps the slider; padding 6px, radius 999px)
//       scale
// The trailing ">" menu button exists in native only when menuEnabled, which
// is false here, so it is omitted.
//
// The slider itself is a custom widget (WinomeQuickSlider): GTK4's GtkScale
// depends on the GTK theme's CSS for its trough/slider sizing, and this
// environment ships no GTK theme, so GtkScale renders a 2x2 px handle that is
// neither visible nor draggable. The custom widget draws the native GNOME
// slider (4px rgba(255,255,255,0.15) trough, accent #3584e4 highlight, 18px
// white handle) and handles pointer drag directly.

typedef struct {
  GtkWidget parent_instance;
  double value;
} WinomeQuickSlider;

typedef struct {
  GtkWidgetClass parent_class;
} WinomeQuickSliderClass;

enum {
  SLIDER_VALUE_CHANGED,
  SLIDER_LAST_SIGNAL
};

static guint slider_signals[SLIDER_LAST_SIGNAL];

#define WINOME_TYPE_QUICK_SLIDER (winome_quick_slider_get_type ())

#define WINOME_QUICK_SLIDER(obj) (G_TYPE_CHECK_INSTANCE_CAST ((obj), \
                                   WINOME_TYPE_QUICK_SLIDER, WinomeQuickSlider))

G_DEFINE_TYPE (WinomeQuickSlider, winome_quick_slider, GTK_TYPE_WIDGET)

#define SLIDER_HANDLE_RADIUS 9.0
#define SLIDER_TROUGH_HEIGHT 4.0

struct SliderTheme {
  double handle_radius = SLIDER_HANDLE_RADIUS;
  double trough_height = SLIDER_TROUGH_HEIGHT;
  GdkRGBA trough_color = {1.0f, 1.0f, 1.0f, 0.15f};
  GdkRGBA highlight_color = {0.208f, 0.518f, 0.894f, 1.0f};
  GdkRGBA handle_color = {1.0f, 1.0f, 1.0f, 1.0f};
  bool resolved = false;
};

static const SliderTheme &
get_slider_theme (void)
{
  static SliderTheme theme;
  if (theme.resolved)
    return theme;
  theme.resolved = true;

  winome::StEngine *e = winome::StEngine::get ();
  if (e == nullptr)
    return theme;

  winome::StEngine::Node node (*e, nullptr, "", "slider", "");
  double v = 0.0;
  if (node.lookup_length ("-slider-handle-radius", &v) && v > 0.0)
    theme.handle_radius = v;
  if (node.lookup_length ("-barlevel-height", &v) && v > 0.0)
    theme.trough_height = v;

  std::string color_hex;
  GdkRGBA parsed{};
  if (node.lookup_color ("-barlevel-background-color", &color_hex) &&
      gdk_rgba_parse (&parsed, color_hex.c_str ()))
    theme.trough_color = parsed;
  if (node.lookup_color ("-barlevel-active-background-color", &color_hex) &&
      gdk_rgba_parse (&parsed, color_hex.c_str ()))
    theme.highlight_color = parsed;
  if (node.lookup_color ("color", &color_hex) &&
      gdk_rgba_parse (&parsed, color_hex.c_str ()))
    theme.handle_color = parsed;

  return theme;
}

static int
qs_grid_spacing (const char *prop, int fallback)
{
  winome::StEngine *e = winome::StEngine::get ();
  if (e == nullptr)
    return fallback;
  winome::StEngine::Node grid (*e, nullptr, "", "quick-settings-grid", "");
  double v = 0.0;
  if (grid.lookup_length (prop, &v) && v > 0.0)
    return (int)(v + 0.5);
  return fallback;
}

static void
winome_quick_slider_set_value_from_x (WinomeQuickSlider *self, double x)
{
  const SliderTheme &st = get_slider_theme ();
  int width = gtk_widget_get_width (GTK_WIDGET (self));
  double min_x = st.handle_radius;
  double max_x = width - st.handle_radius;
  double value;

  if (max_x <= min_x) {
    value = 0.5;
  } else {
    value = CLAMP ((x - min_x) / (max_x - min_x), 0.0, 1.0);
  }

  if (value != self->value) {
    self->value = value;
    g_signal_emit (self, slider_signals[SLIDER_VALUE_CHANGED], 0);
    gtk_widget_queue_draw (GTK_WIDGET (self));
  }
}

static void
on_slider_drag_update (GtkGestureDrag *gesture G_GNUC_UNUSED,
                       double offset_x G_GNUC_UNUSED,
                       double offset_y G_GNUC_UNUSED,
                       gpointer user_data)
{
  double x, y;
  gtk_gesture_get_point (GTK_GESTURE (gesture), NULL, &x, &y);
  winome_quick_slider_set_value_from_x (
      WINOME_QUICK_SLIDER (user_data), x);
}

static void
on_slider_pressed (GtkGestureClick *gesture G_GNUC_UNUSED,
                   int n_press G_GNUC_UNUSED,
                   double x, double y G_GNUC_UNUSED,
                   gpointer user_data)
{
  winome_quick_slider_set_value_from_x (
      WINOME_QUICK_SLIDER (user_data), x);
}

// Append a solid-color rounded rectangle (clip + color).
static void
append_rounded_color (GtkSnapshot *snapshot, const GdkRGBA *color,
                      double x, double y, double w, double h, float radius)
{
  GskRoundedRect round;
  graphene_rect_t r;
  graphene_size_t corner = {radius, radius};

  graphene_rect_init (&r, (float) x, (float) y, (float) w, (float) h);
  gsk_rounded_rect_init (&round, &r, &corner, &corner, &corner, &corner);
  gtk_snapshot_push_rounded_clip (snapshot, &round);
  gtk_snapshot_append_color (snapshot, color, &r);
  gtk_snapshot_pop (snapshot);
}

static void
winome_quick_slider_snapshot (GtkWidget *widget, GtkSnapshot *snapshot)
{
  WinomeQuickSlider *self = WINOME_QUICK_SLIDER (widget);
  const SliderTheme &st = get_slider_theme ();
  double w = gtk_widget_get_width (widget);
  double h = gtk_widget_get_height (widget);
  double cy = h / 2.0;

  // Handle center: value * (width - handle) + handle/2, clamped like native.
  double cx = st.handle_radius +
              CLAMP (self->value, 0.0, 1.0) *
                  (w - 2.0 * st.handle_radius);
  float trough_r = (float)(st.trough_height / 2.0);

  // Trough: full width, rounded.
  append_rounded_color (snapshot, &st.trough_color,
                        0, cy - st.trough_height / 2.0,
                        w, st.trough_height, trough_r);

  // Highlight: from the origin (0) to the handle.
  if (cx > 0)
    append_rounded_color (snapshot, &st.highlight_color,
                          0, cy - st.trough_height / 2.0,
                          cx, st.trough_height, trough_r);

  // Handle: white circle.
  if (w > 0)
    append_rounded_color (snapshot, &st.handle_color,
                          cx - st.handle_radius,
                          cy - st.handle_radius,
                          2.0 * st.handle_radius,
                          2.0 * st.handle_radius,
                          (float)st.handle_radius);
}

static void
winome_quick_slider_measure (GtkWidget *widget G_GNUC_UNUSED,
                             GtkOrientation orientation,
                             int for_size G_GNUC_UNUSED,
                             int *minimum, int *natural,
                             int *minimum_baseline, int *natural_baseline)
{
  const SliderTheme &st = get_slider_theme ();
  if (orientation == GTK_ORIENTATION_HORIZONTAL) {
    *minimum = 0;
    *natural = 150;
  } else {
    int h = (int)(2.0 * st.handle_radius + 4.0 + 0.5);
    *minimum = h;
    *natural = h;
  }
  if (minimum_baseline)
    *minimum_baseline = -1;
  if (natural_baseline)
    *natural_baseline = -1;
}

static void
winome_quick_slider_init (WinomeQuickSlider *self)
{
  GtkWidget *widget = GTK_WIDGET (self);
  GtkGesture *gesture;

  self->value = 0.0;

  gesture = gtk_gesture_drag_new ();
  gtk_gesture_single_set_button (GTK_GESTURE_SINGLE (gesture), 0);
  g_signal_connect (gesture, "drag-update",
                    G_CALLBACK (on_slider_drag_update), self);
  gtk_widget_add_controller (widget, GTK_EVENT_CONTROLLER (gesture));

  gesture = gtk_gesture_click_new ();
  gtk_gesture_single_set_button (GTK_GESTURE_SINGLE (gesture), 0);
  g_signal_connect (gesture, "pressed",
                    G_CALLBACK (on_slider_pressed), self);
  gtk_widget_add_controller (widget, GTK_EVENT_CONTROLLER (gesture));

  gtk_widget_set_focusable (widget, TRUE);
}

static void
winome_quick_slider_class_init (WinomeQuickSliderClass *klass)
{
  GtkWidgetClass *widget_class = GTK_WIDGET_CLASS (klass);

  widget_class->snapshot = winome_quick_slider_snapshot;
  widget_class->measure = winome_quick_slider_measure;
  gtk_widget_class_set_css_name (widget_class, "slider");

  slider_signals[SLIDER_VALUE_CHANGED] =
      g_signal_new ("value-changed",
                    G_TYPE_FROM_CLASS (klass),
                    G_SIGNAL_RUN_LAST,
                    0, NULL, NULL, NULL,
                    G_TYPE_NONE, 0);
}

static GtkWidget *
quick_slider_widget_new (void)
{
  return GTK_WIDGET (g_object_new (winome_quick_slider_get_type (), NULL));
}

static void
quick_slider_set_value (GtkWidget *slider, double value)
{
  WinomeQuickSlider *self = WINOME_QUICK_SLIDER (slider);
  value = CLAMP (value, 0.0, 1.0);
  if (value != self->value) {
    self->value = value;
    gtk_widget_queue_draw (slider);
  }
}

static double
quick_slider_get_value (GtkWidget *slider)
{
  return WINOME_QUICK_SLIDER (slider)->value;
}

static void
quick_slider_connect_value_changed (GtkWidget *slider,
                                    GCallback callback, gpointer data)
{
  g_signal_connect (slider, "value-changed", callback, data);
}

static GtkWidget *
make_quick_slider (const char *icon_name, GtkWidget **slider_out,
                   GtkWidget **icon_out, GtkWidget **button_out)
{
  GtkWidget *box;
  GtkWidget *icon_button;
  GtkWidget *icon;
  GtkWidget *slider_bin;
  GtkWidget *slider;

  box = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 6);
  gtk_widget_add_css_class (box, "quick-slider");

  icon_button = gtk_button_new ();
  gtk_widget_add_css_class (icon_button, "icon-button");
  gtk_widget_add_css_class (icon_button, "flat");
  gtk_widget_set_focus_on_click (icon_button, FALSE);
  gtk_widget_set_valign (icon_button, GTK_ALIGN_CENTER);

  icon = gtk_image_new_from_icon_name (icon_name);
  gtk_image_set_pixel_size (GTK_IMAGE (icon), QUICK_ICON_SIZE);
  gtk_widget_set_halign (icon, GTK_ALIGN_CENTER);
  gtk_widget_set_valign (icon, GTK_ALIGN_CENTER);
  gtk_button_set_child (GTK_BUTTON (icon_button), icon);

  gtk_box_append (GTK_BOX (box), icon_button);

  slider_bin = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);
  gtk_widget_add_css_class (slider_bin, "slider-bin");
  gtk_widget_set_hexpand (slider_bin, TRUE);
  gtk_widget_set_valign (slider_bin, GTK_ALIGN_CENTER);

  slider = quick_slider_widget_new ();
  gtk_widget_set_hexpand (slider, TRUE);
  gtk_widget_set_valign (slider, GTK_ALIGN_CENTER);
  gtk_widget_set_halign (slider, GTK_ALIGN_FILL);
  gtk_box_append (GTK_BOX (slider_bin), slider);

  gtk_box_append (GTK_BOX (box), slider_bin);

  if (slider_out)
    *slider_out = slider;
  if (icon_out)
    *icon_out = icon;
  if (button_out)
    *button_out = icon_button;

  return box;
}

// A round .icon-button with a centered 16px symbolic icon (the system item's
// screenshot/settings/lock/power buttons). Built manually so GTK4's
// button-with-icon child cannot stretch the icon to fill the button.
static GtkWidget *
make_icon_button (const char *icon_name)
{
  GtkWidget *button;
  GtkWidget *icon;

  button = gtk_button_new ();
  gtk_widget_add_css_class (button, "icon-button");
  gtk_widget_set_focus_on_click (button, FALSE);
  gtk_widget_set_hexpand (button, FALSE);

  icon = gtk_image_new_from_icon_name (icon_name);
  gtk_image_set_pixel_size (GTK_IMAGE (icon), QUICK_ICON_SIZE);
  gtk_widget_set_halign (icon, GTK_ALIGN_CENTER);
  gtk_widget_set_valign (icon, GTK_ALIGN_CENTER);
  gtk_button_set_child (GTK_BUTTON (button), icon);

  return button;
}

// --- Icon helpers ------------------------------------------------------------

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

// --- Callbacks ---------------------------------------------------------------

static void
on_volume_changed (GtkWidget *slider G_GNUC_UNUSED, gpointer G_GNUC_UNUSED user_data)
{
  winome::set_volume (quick_slider_get_value (slider));
}

static void
on_volume_mute_clicked (GtkButton *button G_GNUC_UNUSED, gpointer user_data)
{
  winome::toggle_mute ();
  winome_quick_settings_refresh (GTK_WIDGET (user_data));
}

static void
on_brightness_changed (GtkWidget *slider, gpointer G_GNUC_UNUSED user_data)
{
  winome::set_brightness (quick_slider_get_value (slider));
}

static void
on_screenshot_clicked (GtkButton *button G_GNUC_UNUSED,
                       gpointer G_GNUC_UNUSED user_data)
{
  ShellExecuteW (nullptr, L"open", L"ms-screenclip:", nullptr, nullptr,
                 SW_SHOWNORMAL);
}

static void
on_settings_clicked (GtkButton *button G_GNUC_UNUSED,
                     gpointer G_GNUC_UNUSED user_data)
{
  winome_shell_panel_close_popovers ();
  winome::open_settings_window ();
}

static void
on_dark_mode_toggled (GtkToggleButton *button,
                      gpointer G_GNUC_UNUSED user_data)
{
  winome::set_dark_mode (gtk_toggle_button_get_active (button) != FALSE);
}

static void
on_wlan_toggled (GtkToggleButton *button, gpointer user_data)
{
  gboolean active = gtk_toggle_button_get_active (button);
  winome_wifi_set_enabled (active);
  if (user_data != nullptr)
    winome_quick_settings_refresh (GTK_WIDGET (user_data));
}

struct BluetoothTaskResult {
  GtkWidget *panel;
  GtkWidget *button;
  bool       ok;
};

static gboolean
idle_bluetooth_task_done (gpointer user_data)
{
  BluetoothTaskResult *res = static_cast<BluetoothTaskResult *> (user_data);
  if (res->button != nullptr)
    gtk_widget_set_sensitive (res->button, TRUE);
  if (res->panel != nullptr) {
    winome_quick_settings_refresh (res->panel);
    g_object_unref (res->panel);
  }
  if (res->button != nullptr)
    g_object_unref (res->button);
  if (!res->ok) {
    winome::notify_shell (
        tr ("Bluetooth", "蓝牙"),
        tr ("Could not change Bluetooth radio state.",
            "无法切换蓝牙开关状态。"),
        "bluetooth-disabled-symbolic");
  }
  delete res;
  return G_SOURCE_REMOVE;
}

static void
on_bluetooth_toggled (GtkToggleButton *button, gpointer user_data)
{
  const bool active = gtk_toggle_button_get_active (button) != FALSE;
  gtk_widget_set_sensitive (GTK_WIDGET (button), FALSE);
  GtkWidget *panel = user_data != nullptr
                         ? GTK_WIDGET (g_object_ref (G_OBJECT (user_data)))
                         : nullptr;
  GtkWidget *btn = GTK_WIDGET (g_object_ref (G_OBJECT (button)));
  std::thread ([active, panel, btn] () {
    bool ok = winome::set_bluetooth_enabled (active);
    auto *res = new BluetoothTaskResult{panel, btn, ok};
    g_idle_add (idle_bluetooth_task_done, res);
  }).detach ();
}

static void
on_night_light_toggled (GtkToggleButton *button,
                        gpointer G_GNUC_UNUSED user_data)
{
  winome::set_night_light (gtk_toggle_button_get_active (button) != FALSE);
}

static void
on_dnd_toggled (GtkToggleButton *button, gpointer G_GNUC_UNUSED user_data)
{
  winome::set_do_not_disturb (gtk_toggle_button_get_active (button) != FALSE);
}

static void
on_airplane_toggled (GtkToggleButton *button, gpointer user_data)
{
  winome::set_airplane_mode (gtk_toggle_button_get_active (button) != FALSE);
  if (user_data != nullptr)
    winome_quick_settings_refresh (GTK_WIDGET (user_data));
}

static void
on_lock_clicked (GtkButton *button G_GNUC_UNUSED, gpointer G_GNUC_UNUSED user_data)
{
  winome::lock_screen ();
}

// --- QuickSettingsGrid -------------------------------------------------------
//
// Port of the native QuickSettingsLayout (quickSettings.js) into a custom
// widget. GtkGrid would work, but it distributes surplus height to rows that
// report expand and misaligns the toggle capsules; the native layout sizes
// each row to its natural height and gives each child exactly its share, while
// expanding any active QuickToggleMenu inline in a GtkRevealer directly under
// its trigger row.
//
// Layout (nColumns=2, row/column spacing 12px):
//   - Invisible children (e.g. missing brightness, Bluetooth, keyboard) are
//     skipped during row packing so remaining tiles pack without blank holes.
//   - Children with column-span 1 are capped horizontally at `.quick-toggle`'s
//     theme `max-width` (12em) resolved through StEngine.
//   - childWidth = floor((width - columnSpacing) / 2); a spanned child gets
//     childWidth * span + columnSpacing * (span - 1).

static int
qs_toggle_max_width (void)
{
  winome::StEngine *e = winome::StEngine::get ();
  if (e == nullptr)
    return 176;
  winome::StEngine::Node toggle (*e, nullptr, "", "quick-toggle", "");
  double v = 0.0;
  if (toggle.lookup_length ("max-width", &v) && v > 0.0)
    return (int)(v + 0.5);
  return 176;
}

typedef struct {
  GtkWidget *widget;
  int col_span;
} QuickGridChild;

typedef struct {
  GtkWidget parent_instance;
  GPtrArray *children;
  GtkWidget *drawer_revealer;
  GtkWidget *drawer_anchor;
  int row_spacing;
  int column_spacing;
  int toggle_max_width;
} WinomeQuickSettingsGrid;

typedef struct {
  GtkWidgetClass parent_class;
} WinomeQuickSettingsGridClass;

#define WINOME_TYPE_QUICK_SETTINGS_GRID (winome_quick_settings_grid_get_type ())
#define WINOME_QUICK_SETTINGS_GRID(obj) (G_TYPE_CHECK_INSTANCE_CAST ((obj), \
    WINOME_TYPE_QUICK_SETTINGS_GRID, WinomeQuickSettingsGrid))

G_DEFINE_TYPE (WinomeQuickSettingsGrid, winome_quick_settings_grid, GTK_TYPE_WIDGET)

static gboolean
grid_is_drawer_active (WinomeQuickSettingsGrid *self)
{
  if (self->drawer_revealer == NULL || self->drawer_anchor == NULL)
    return FALSE;
  GtkRevealer *rev = GTK_REVEALER (self->drawer_revealer);
  return gtk_revealer_get_child (rev) != NULL &&
         (gtk_revealer_get_reveal_child (rev) ||
          gtk_revealer_get_child_revealed (rev));
}

static int
grid_measure_drawer (WinomeQuickSettingsGrid *self, int for_width)
{
  if (!grid_is_drawer_active (self))
    return 0;
  int m = 0, n = 0;
  gtk_widget_measure (self->drawer_revealer, GTK_ORIENTATION_VERTICAL,
                      for_width, &m, &n, NULL, NULL);
  return n;
}

static void
grid_set_dimmed (WinomeQuickSettingsGrid *self, GtkWidget *keep_clear)
{
  for (guint i = 0; i < self->children->len; i++) {
    QuickGridChild *c =
        static_cast<QuickGridChild *> (g_ptr_array_index (self->children, i));
    if (keep_clear != NULL && c->widget != keep_clear)
      gtk_widget_add_css_class (c->widget, "quick-settings-dimmed");
    else
      gtk_widget_remove_css_class (c->widget, "quick-settings-dimmed");
  }
}

static void
on_drawer_child_revealed (GObject *object,
                          GParamSpec *pspec G_GNUC_UNUSED,
                          gpointer user_data)
{
  GtkRevealer *rev = GTK_REVEALER (object);
  WinomeQuickSettingsGrid *self = WINOME_QUICK_SETTINGS_GRID (user_data);
  if (!gtk_revealer_get_reveal_child (rev) &&
      !gtk_revealer_get_child_revealed (rev)) {
    gtk_revealer_set_child (rev, NULL);
    self->drawer_anchor = NULL;
    gtk_widget_queue_resize (GTK_WIDGET (self));
  }
}

// Pack visible grid children into 2-column rows and report the row index
// containing @drawer_anchor (or -1).
static GList **
compute_rows (WinomeQuickSettingsGrid *self, int n_columns,
              int *n_rows_out, int *anchor_row_out)
{
  GList **rows = g_new0 (GList *, self->children->len + 1);
  int n_rows = 0;
  int line_index = 0;
  int anchor_row = -1;
  gboolean cur_has_anchor = FALSE;
  GList *cur = NULL;

  for (guint i = 0; i < self->children->len; i++) {
    QuickGridChild *c =
        static_cast<QuickGridChild *> (g_ptr_array_index (self->children, i));
    if (!gtk_widget_get_visible (c->widget))
      continue;

    int span = CLAMP (c->col_span, 1, n_columns);
    c->col_span = span;

    if (line_index == 0 || line_index + span > n_columns) {
      if (cur != NULL) {
        if (cur_has_anchor)
          anchor_row = n_rows;
        rows[n_rows++] = cur;
      }
      cur = NULL;
      cur_has_anchor = FALSE;
      line_index = 0;
    }

    if (c->widget == self->drawer_anchor)
      cur_has_anchor = TRUE;
    cur = g_list_append (cur, c);
    line_index = (line_index + span) % n_columns;
  }

  if (cur != NULL) {
    if (cur_has_anchor)
      anchor_row = n_rows;
    rows[n_rows++] = cur;
  }
  rows[n_rows] = NULL;

  if (n_rows_out != NULL)
    *n_rows_out = n_rows;
  if (anchor_row_out != NULL)
    *anchor_row_out = anchor_row;

  return rows;
}

static int
max_child_height (GList *row, int child_width, int column_spacing)
{
  int h = 0;
  for (GList *l = row; l != NULL; l = l->next) {
    QuickGridChild *c = static_cast<QuickGridChild *>(l->data);
    int for_w = -1;
    if (child_width >= 0)
      for_w = child_width * c->col_span + column_spacing * (c->col_span - 1);
    int m, n;
    gtk_widget_measure (c->widget, GTK_ORIENTATION_VERTICAL, for_w, &m, &n,
                        NULL, NULL);
    h = MAX (h, n);
  }
  return h;
}

static void
winome_quick_settings_grid_measure (GtkWidget *widget,
                                    GtkOrientation orientation,
                                    int for_size,
                                    int *minimum, int *natural,
                                    int *minimum_baseline, int *natural_baseline)
{
  WinomeQuickSettingsGrid *self = WINOME_QUICK_SETTINGS_GRID (widget);
  const int n_columns = 2;

  if (minimum_baseline)
    *minimum_baseline = -1;
  if (natural_baseline)
    *natural_baseline = -1;

  if (orientation == GTK_ORIENTATION_HORIZONTAL) {
    int min_w = 0, nat_w = 0;
    for (guint i = 0; i < self->children->len; i++) {
      QuickGridChild *c =
          static_cast<QuickGridChild *> (g_ptr_array_index (self->children, i));
      if (!gtk_widget_get_visible (c->widget))
        continue;
      int m, n;
      gtk_widget_measure (c->widget, GTK_ORIENTATION_HORIZONTAL, -1,
                          &m, &n, NULL, NULL);
      int cm = m / c->col_span;
      int cn = n / c->col_span;
      if (c->col_span == 1) {
        cm = MIN (cm, self->toggle_max_width);
        cn = MIN (cn, self->toggle_max_width);
      }
      min_w = MAX (min_w, cm);
      nat_w = MAX (nat_w, cn);
    }
    *minimum = n_columns * min_w + self->column_spacing;
    *natural = n_columns * nat_w + self->column_spacing;
  } else {
    int n_rows = 0, anchor_row = -1;
    GList **rows = compute_rows (self, n_columns, &n_rows, &anchor_row);
    int child_width = for_size >= 0
                          ? (for_size - self->column_spacing) / n_columns
                          : -1;
    int total_h = 0;
    for (int r = 0; r < n_rows; r++) {
      total_h += max_child_height (rows[r], child_width, self->column_spacing);
      g_list_free (rows[r]);
    }
    g_free (rows);

    if (n_rows > 1)
      total_h += (n_rows - 1) * self->row_spacing;

    if (anchor_row >= 0) {
      int drawer_h = grid_measure_drawer (self, for_size);
      if (drawer_h > 0)
        total_h += drawer_h + MIN (self->row_spacing, drawer_h);
    }

    *minimum = total_h;
    *natural = total_h;
  }
}

static void
winome_quick_settings_grid_size_allocate (GtkWidget *widget,
                                          int width,
                                          int height G_GNUC_UNUSED,
                                          int baseline G_GNUC_UNUSED)
{
  WinomeQuickSettingsGrid *self = WINOME_QUICK_SETTINGS_GRID (widget);
  const int n_columns = 2;

  int n_rows = 0, anchor_row = -1;
  GList **rows = compute_rows (self, n_columns, &n_rows, &anchor_row);

  int child_width = (width - self->column_spacing) / n_columns;
  int y = 0;

  for (int r = 0; r < n_rows; r++) {
    GList *row = rows[r];
    int row_height = max_child_height (row, child_width, self->column_spacing);

    int line_index = 0;
    for (GList *l = row; l != NULL; l = l->next) {
      QuickGridChild *c = static_cast<QuickGridChild *>(l->data);
      int cw = child_width * c->col_span +
               self->column_spacing * (c->col_span - 1);
      int x = line_index * (child_width + self->column_spacing);

      GtkAllocation alloc = {x, y, cw, row_height};
      gtk_widget_size_allocate (c->widget, &alloc, -1);

      line_index = (line_index + c->col_span) % n_columns;
    }

    y += row_height;
    g_list_free (row);

    int drawer_h = (r == anchor_row) ? grid_measure_drawer (self, width) : 0;
    if (drawer_h > 0) {
      int top_gap = (r + 1 < n_rows) ? self->row_spacing
                                     : MIN (self->row_spacing, drawer_h);
      GtkAllocation drawer_alloc = {0, y + top_gap, width, drawer_h};
      gtk_widget_size_allocate (self->drawer_revealer, &drawer_alloc, -1);
      y += top_gap + drawer_h;
      if (r + 1 < n_rows)
        y += MIN (self->row_spacing, drawer_h);
    } else if (r + 1 < n_rows) {
      y += self->row_spacing;
    }
  }
  g_free (rows);
}

static void
winome_quick_settings_grid_init (WinomeQuickSettingsGrid *self)
{
  self->children = g_ptr_array_new_with_free_func (g_free);
  self->row_spacing = qs_grid_spacing ("spacing-rows", 12);
  self->column_spacing = qs_grid_spacing ("spacing-columns", 12);
  self->toggle_max_width = qs_toggle_max_width ();

  self->drawer_revealer = gtk_revealer_new ();
  gtk_revealer_set_transition_type (GTK_REVEALER (self->drawer_revealer),
                                    GTK_REVEALER_TRANSITION_TYPE_SLIDE_DOWN);
  gtk_revealer_set_transition_duration (
      GTK_REVEALER (self->drawer_revealer),
      winome::QUICK_SETTINGS_ANIMATION_TIME / 2);
  gtk_revealer_set_reveal_child (GTK_REVEALER (self->drawer_revealer), FALSE);
  gtk_widget_set_hexpand (self->drawer_revealer, TRUE);
  gtk_widget_set_parent (self->drawer_revealer, GTK_WIDGET (self));

  g_signal_connect (self->drawer_revealer, "notify::child-revealed",
                    G_CALLBACK (on_drawer_child_revealed), self);
}

static void
winome_quick_settings_grid_finalize (GObject *object)
{
  WinomeQuickSettingsGrid *self = WINOME_QUICK_SETTINGS_GRID (object);
  self->drawer_anchor = NULL;
  if (self->drawer_revealer != NULL) {
    gtk_widget_unparent (self->drawer_revealer);
    self->drawer_revealer = NULL;
  }
  for (guint i = 0; i < self->children->len; i++) {
    QuickGridChild *c =
        static_cast<QuickGridChild *> (g_ptr_array_index (self->children, i));
    gtk_widget_unparent (c->widget);
  }
  g_ptr_array_unref (self->children);
  G_OBJECT_CLASS (winome_quick_settings_grid_parent_class)->finalize (object);
}

static void
winome_quick_settings_grid_class_init (WinomeQuickSettingsGridClass *klass)
{
  GObjectClass *gobject_class = G_OBJECT_CLASS (klass);
  GtkWidgetClass *widget_class = GTK_WIDGET_CLASS (klass);

  gobject_class->finalize = winome_quick_settings_grid_finalize;
  widget_class->measure = winome_quick_settings_grid_measure;
  widget_class->size_allocate = winome_quick_settings_grid_size_allocate;
  gtk_widget_class_set_css_name (widget_class, "grid");
}

static GtkWidget *
winome_quick_settings_grid_new (void)
{
  return GTK_WIDGET (g_object_new (winome_quick_settings_grid_get_type (), NULL));
}

static void
winome_quick_settings_grid_attach (GtkWidget *grid, GtkWidget *child,
                                   int col_span)
{
  WinomeQuickSettingsGrid *self = WINOME_QUICK_SETTINGS_GRID (grid);
  QuickGridChild *c = g_new0 (QuickGridChild, 1);

  c->widget = child;
  c->col_span = col_span;
  g_ptr_array_add (self->children, c);
  gtk_widget_set_parent (child, grid);
}

static GtkWidget *
find_quick_settings_root (GtkWidget *widget)
{
  GtkWidget *cur = widget;
  while (cur != NULL) {
    if (g_object_get_data (G_OBJECT (cur), "qs-grid") != NULL)
      return cur;
    cur = gtk_widget_get_parent (cur);
  }
  return NULL;
}

static WinomeQuickSettingsGrid *
get_qs_grid (GtkWidget *quick_settings)
{
  if (quick_settings == NULL)
    return NULL;
  GtkWidget *grid = GTK_WIDGET (
      g_object_get_data (G_OBJECT (quick_settings), "qs-grid"));
  return grid != NULL ? WINOME_QUICK_SETTINGS_GRID (grid) : NULL;
}

static gboolean
quick_settings_is_menu_open_for (GtkWidget *quick_settings, GtkWidget *anchor)
{
  WinomeQuickSettingsGrid *grid = get_qs_grid (quick_settings);
  if (grid == NULL || anchor == NULL || grid->drawer_revealer == NULL)
    return FALSE;
  return grid->drawer_anchor == anchor &&
         gtk_revealer_get_reveal_child (GTK_REVEALER (grid->drawer_revealer));
}

static void
quick_settings_toggle_inline_menu (GtkWidget *quick_settings,
                                   GtkWidget *anchor, GtkWidget *menu)
{
  WinomeQuickSettingsGrid *grid = get_qs_grid (quick_settings);
  if (grid == NULL || anchor == NULL || menu == NULL)
    return;

  if (quick_settings_is_menu_open_for (quick_settings, anchor)) {
    winome_quick_settings_close_menu (quick_settings, TRUE);
    return;
  }

  GtkRevealer *rev = GTK_REVEALER (grid->drawer_revealer);
  grid->drawer_anchor = anchor;
  gtk_revealer_set_transition_duration (
      rev, winome::QUICK_SETTINGS_ANIMATION_TIME / 2);
  gtk_revealer_set_child (rev, menu);
  grid_set_dimmed (grid, anchor);
  gtk_revealer_set_reveal_child (rev, TRUE);
  gtk_widget_queue_resize (GTK_WIDGET (grid));
}

gboolean
winome_quick_settings_close_menu (GtkWidget *quick_settings, gboolean animate)
{
  WinomeQuickSettingsGrid *grid = get_qs_grid (quick_settings);
  if (grid == NULL || grid->drawer_revealer == NULL)
    return FALSE;

  GtkRevealer *rev = GTK_REVEALER (grid->drawer_revealer);
  gboolean open_now = gtk_revealer_get_reveal_child (rev) != FALSE;
  gboolean active_now = open_now || gtk_revealer_get_child_revealed (rev);
  if (!active_now && grid->drawer_anchor == NULL)
    return FALSE;

  grid_set_dimmed (grid, NULL);

  if (!animate) {
    gtk_revealer_set_transition_duration (rev, 0);
    gtk_revealer_set_reveal_child (rev, FALSE);
    gtk_revealer_set_child (rev, NULL);
    grid->drawer_anchor = NULL;
    gtk_widget_queue_resize (GTK_WIDGET (grid));
    return open_now;
  }

  if (!open_now)
    return FALSE;

  gtk_revealer_set_transition_duration (
      rev, winome::QUICK_SETTINGS_ANIMATION_TIME / 2);
  gtk_revealer_set_reveal_child (rev, FALSE);
  return TRUE;
}

static void
on_grid_capture_pressed (GtkGestureClick *gesture, int n_press G_GNUC_UNUSED,
                         double x, double y, gpointer user_data)
{
  GtkWidget *qs = GTK_WIDGET (user_data);
  WinomeQuickSettingsGrid *grid = get_qs_grid (qs);
  if (grid == NULL || grid->drawer_revealer == NULL ||
      !gtk_revealer_get_reveal_child (GTK_REVEALER (grid->drawer_revealer)))
    return;

  GtkWidget *picked =
      gtk_widget_pick (GTK_WIDGET (grid), x, y, GTK_PICK_DEFAULT);
  if (picked == grid->drawer_revealer ||
      (picked != NULL &&
       gtk_widget_is_ancestor (picked, grid->drawer_revealer)))
    return;

  if (winome_quick_settings_close_menu (qs, TRUE))
    gtk_gesture_set_state (GTK_GESTURE (gesture), GTK_EVENT_SEQUENCE_CLAIMED);
}

// --- Panel widget ------------------------------------------------------------

// Power menu (the GNOME power-off / end-session rows reached from the system
// item): Suspend, Restart, Power Off, separator, Log Out, Quit WINOME. These
// act immediately — GNOME's end-session items use the default 'user' session
// mode, so no confirmation dialog sits between the click and the action.
typedef enum {
  POWER_ACTION_SUSPEND,
  POWER_ACTION_LOGOUT,
  POWER_ACTION_RESTART,
  POWER_ACTION_SHUTDOWN,
  POWER_ACTION_QUIT_SHELL
} PowerAction;

static gboolean
idle_quit_winome_shell (gpointer G_GNUC_UNUSED data)
{
  GApplication *app = g_application_get_default ();
  if (app != NULL)
    g_application_quit (app);
  return G_SOURCE_REMOVE;
}

static void
on_power_action (gpointer user_data)
{
  switch (GPOINTER_TO_INT (user_data)) {
  case POWER_ACTION_SUSPEND:
    winome::suspend_system ();
    break;
  case POWER_ACTION_LOGOUT:
    winome::log_out ();
    break;
  case POWER_ACTION_RESTART:
    winome::restart_system ();
    break;
  case POWER_ACTION_SHUTDOWN:
    winome::shutdown_system ();
    break;
  case POWER_ACTION_QUIT_SHELL:
    g_idle_add (idle_quit_winome_shell, NULL);
    break;
  }
}

static GtkWidget *
build_power_menu (gpointer user_data)
{
  WinomeMenuEntry items[6] = {};

  (void)user_data;

  items[0].label = tr ("Suspend", "挂起");
  items[0].icon_name = "media-playback-pause-symbolic";
  items[0].checked = FALSE;
  items[0].callback = on_power_action;
  items[0].user_data = GINT_TO_POINTER (POWER_ACTION_SUSPEND);

  items[1].label = tr ("Restart", "重启");
  items[1].icon_name = "system-reboot-symbolic";
  items[1].checked = FALSE;
  items[1].callback = on_power_action;
  items[1].user_data = GINT_TO_POINTER (POWER_ACTION_RESTART);

  items[2].label = tr ("Power Off", "关机");
  items[2].icon_name = "system-shutdown-symbolic";
  items[2].checked = FALSE;
  items[2].callback = on_power_action;
  items[2].user_data = GINT_TO_POINTER (POWER_ACTION_SHUTDOWN);

  // Separate system power actions from session / shell exit actions, matching
  // GNOME's ShutdownItem separator before Log Out.
  items[3].label = NULL;

  items[4].label = tr ("Log Out", "注销");
  items[4].icon_name = "system-log-out-symbolic";
  items[4].checked = FALSE;
  items[4].callback = on_power_action;
  items[4].user_data = GINT_TO_POINTER (POWER_ACTION_LOGOUT);

  items[5].label = tr ("Quit WINOME", "退出 WINOME");
  items[5].icon_name = "application-exit-symbolic";
  items[5].checked = FALSE;
  items[5].callback = on_power_action;
  items[5].user_data = GINT_TO_POINTER (POWER_ACTION_QUIT_SHELL);

  // Header mirrors upstream's setHeader() calls: GNOME labels this menu
  // "Power Off Menu" with the shutdown glyph in the badge.
  return build_quick_menu_full (items, G_N_ELEMENTS (items),
                               "system-shutdown-symbolic",
                               tr ("Power Off Menu", "关机菜单"), NULL, FALSE);
}

// Open handler for the system power capsule and shutdown button: toggles
// the inline Power Off Menu directly below Row 0.
static void
on_power_item_clicked (GtkButton *button, gpointer user_data)
{
  GtkWidget *qs = find_quick_settings_root (GTK_WIDGET (button));
  GtkWidget *anchor = user_data != NULL
                          ? GTK_WIDGET (user_data)
                          : gtk_widget_get_parent (GTK_WIDGET (button));
  if (qs == NULL || anchor == NULL)
    return;

  if (quick_settings_is_menu_open_for (qs, anchor)) {
    winome_quick_settings_close_menu (qs, TRUE);
    return;
  }

  GtkWidget *menu = build_power_menu (NULL);
  if (menu != NULL)
    quick_settings_toggle_inline_menu (qs, anchor, menu);
}

// Each row needs its own target mode, so wrap it in a tiny heap struct kept
// alive for the lifetime of the panel (the menu is rebuilt per open).
typedef struct {
  WinomePowerMode mode;
  GtkWidget      *panel;
} PowerModeTarget;

static void
on_power_mode_activate (gpointer user_data)
{
  PowerModeTarget *t = static_cast<PowerModeTarget *> (user_data);
  if (t->panel != NULL && t->mode != WINOME_POWER_BALANCED &&
      t->mode < WINOME_POWER_MODE_COUNT) {
    g_object_set_data (G_OBJECT (t->panel), "last-power-mode",
                       GINT_TO_POINTER ((int)t->mode + 1));
  }
  if (winome_power_set_mode (t->mode))
    winome_quick_settings_refresh (t->panel);
}

static WinomePowerMode
preferred_non_balanced_mode (GtkWidget *qs)
{
  int stored = qs != NULL ? GPOINTER_TO_INT (
      g_object_get_data (G_OBJECT (qs), "last-power-mode")) : 0;
  if (stored > 0 && stored - 1 < WINOME_POWER_MODE_COUNT &&
      stored - 1 != WINOME_POWER_BALANCED &&
      winome_power_mode_available ((WinomePowerMode)(stored - 1))) {
    return (WinomePowerMode)(stored - 1);
  }
  if (winome_power_mode_available (WINOME_POWER_SAVER))
    return WINOME_POWER_SAVER;
  if (winome_power_mode_available (WINOME_POWER_PERFORMANCE))
    return WINOME_POWER_PERFORMANCE;
  return WINOME_POWER_BALANCED;
}

static void
on_power_mode_toggled (GtkToggleButton *button, gpointer user_data)
{
  GtkWidget *qs = GTK_WIDGET (user_data);
  WinomePowerMode target = gtk_toggle_button_get_active (button)
                               ? preferred_non_balanced_mode (qs)
                               : WINOME_POWER_BALANCED;
  winome_power_set_mode (target);
  if (qs != NULL)
    winome_quick_settings_refresh (qs);
}

// Wi-Fi secondary menu: the visible networks with the associated one checked.
// Clicking a row asks Windows to connect using its stored profile; we do not
// prompt for credentials, matching GNOME's behaviour for saved networks.
typedef struct {
  GtkWidget *panel;              // quick settings root, for refresh
  GPtrArray *networks;           // WinomeWifiNetwork*, owned
  GPtrArray *labels;             // char* row labels, owned
  GPtrArray *rows;               // WifiRowTarget*, owned (one per row)
} WifiMenu;

static void
wifi_menu_free (gpointer data)
{
  WifiMenu *m = static_cast<WifiMenu *>(data);
  if (m == nullptr)
    return;
  if (m->rows != NULL)
    g_ptr_array_free (m->rows, TRUE);
  if (m->labels != NULL)
    g_ptr_array_free (m->labels, TRUE);
  if (m->networks != NULL)
    g_ptr_array_free (m->networks, TRUE);
  g_free (m);
}

// Row activation: each row gets its own index into the menu's network array so
// the callback knows which SSID was clicked.
typedef struct {
  WifiMenu *menu;
  guint     index;
} WifiRowTarget;

// Clicking a row connects; the row target is recycled rather than freed so a
// menu row can be activated more than once (they are not: the popup is torn
// down on the next open, and the free func below releases whatever is left).
static void
run_wifi_task_async (GtkWidget *panel,
                     std::string ssid,
                     gboolean (*fn)(const char *, const char *, gboolean),
                     std::string password,
                     gboolean open_network,
                     bool is_disconnect,
                     bool has_saved_profile)
{
  if (panel != nullptr)
    g_object_ref (panel);

  std::thread ([panel, ssid = std::move (ssid), fn,
                password = std::move (password), open_network,
                is_disconnect, has_saved_profile] () {
    gboolean ok = FALSE;
    if (is_disconnect)
      ok = winome_wifi_disconnect ();
    else if (has_saved_profile)
      ok = winome_wifi_connect (ssid.c_str ());
    else if (fn != nullptr)
      ok = fn (ssid.c_str (), password.c_str (), open_network);

    struct DoneData {
      GtkWidget *panel;
      std::string ssid;
      bool failed;
    };
    auto *done = new DoneData {panel, ssid, !ok && !is_disconnect};
    g_idle_add ([] (gpointer ud) -> gboolean {
      auto *d = static_cast<DoneData *> (ud);
      if (d->panel != nullptr) {
        winome_quick_settings_refresh (d->panel);
        g_object_unref (d->panel);
      }
      if (d->failed) {
        winome::notify_shell (
            tr ("Wi-Fi Connection Failed", "Wi-Fi 连接失败"),
            d->ssid, "WINOME");
      }
      delete d;
      return G_SOURCE_REMOVE;
    }, done);
  }).detach ();
}

typedef struct {
  GtkWidget *dialog;
  GtkWidget *entry;
  GtkWidget *panel;
  char *ssid;
} WifiPasswordPrompt;

static void
wifi_password_prompt_free (gpointer data)
{
  WifiPasswordPrompt *p = static_cast<WifiPasswordPrompt *> (data);
  if (p == nullptr)
    return;
  if (p->panel != nullptr)
    g_object_unref (p->panel);
  g_free (p->ssid);
  g_free (p);
}

static void
on_wifi_pwd_cancel (GtkButton *b G_GNUC_UNUSED, gpointer user_data)
{
  GtkWidget *dlg = GTK_WIDGET (user_data);
  if (dlg != nullptr)
    gtk_window_destroy (GTK_WINDOW (dlg));
}

static void
on_wifi_pwd_submit (GtkWidget *w G_GNUC_UNUSED, gpointer user_data)
{
  WifiPasswordPrompt *p = static_cast<WifiPasswordPrompt *> (user_data);
  if (p == nullptr)
    return;
  const char *pwd = gtk_editable_get_text (GTK_EDITABLE (p->entry));
  std::string ssid = p->ssid ? p->ssid : "";
  std::string pass = pwd ? pwd : "";
  GtkWidget *panel = p->panel;
  GtkWidget *dlg = p->dialog;
  if (dlg != nullptr)
    gtk_window_destroy (GTK_WINDOW (dlg));
  run_wifi_task_async (panel, std::move (ssid),
                       winome_wifi_connect_with_password,
                       std::move (pass), FALSE, false, false);
}

static void
show_wifi_password_dialog (const char *ssid, GtkWidget *panel)
{
  GtkWidget *dlg = gtk_window_new ();
  gtk_window_set_decorated (GTK_WINDOW (dlg), FALSE);
  gtk_window_set_resizable (GTK_WINDOW (dlg), FALSE);
  gtk_window_set_title (GTK_WINDOW (dlg),
                        tr ("Connect to Wi-Fi", "连接到无线网络"));
  winome::install_bundled_fonts (dlg);

  GtkWidget *box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 12);
  gtk_widget_add_css_class (box, "modal-dialog");
  gtk_widget_add_css_class (box, "prompt-dialog");
  gtk_widget_set_size_request (box, 340, -1);

  GtkWidget *content = gtk_box_new (GTK_ORIENTATION_VERTICAL, 8);
  gtk_widget_add_css_class (content, "modal-dialog-content-box");

  gchar *heading = g_strdup_printf (
      tr ("Enter password for “%s”", "输入 “%s” 的密码"), ssid);
  GtkWidget *lbl = gtk_label_new (heading);
  g_free (heading);
  gtk_widget_add_css_class (lbl, "prompt-dialog-info-label");
  gtk_label_set_xalign (GTK_LABEL (lbl), 0.0f);
  gtk_label_set_wrap (GTK_LABEL (lbl), TRUE);
  gtk_box_append (GTK_BOX (content), lbl);

  GtkWidget *entry = gtk_password_entry_new ();
  gtk_password_entry_set_show_peek_icon (GTK_PASSWORD_ENTRY (entry), TRUE);
  gtk_box_append (GTK_BOX (content), entry);
  gtk_box_append (GTK_BOX (box), content);

  GtkWidget *btns = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 8);
  gtk_widget_add_css_class (btns, "modal-dialog-button-box");
  gtk_widget_set_halign (btns, GTK_ALIGN_END);

  GtkWidget *cancel = gtk_button_new_with_label (tr ("Cancel", "取消"));
  gtk_widget_add_css_class (cancel, "cancel-button");
  gtk_widget_add_css_class (cancel, "modal-dialog-linked-button");
  g_signal_connect (cancel, "clicked", G_CALLBACK (on_wifi_pwd_cancel), dlg);
  gtk_box_append (GTK_BOX (btns), cancel);

  GtkWidget *connect_btn = gtk_button_new_with_label (tr ("Connect", "连接"));
  gtk_widget_add_css_class (connect_btn, "default");
  gtk_widget_add_css_class (connect_btn, "modal-dialog-linked-button");
  gtk_box_append (GTK_BOX (btns), connect_btn);
  gtk_box_append (GTK_BOX (box), btns);

  WifiPasswordPrompt *p = g_new0 (WifiPasswordPrompt, 1);
  p->dialog = dlg;
  p->entry = entry;
  p->panel = panel ? GTK_WIDGET (g_object_ref (panel)) : nullptr;
  p->ssid = g_strdup (ssid);
  g_object_set_data_full (G_OBJECT (dlg), "wifi-prompt", p,
                          wifi_password_prompt_free);

  g_signal_connect (connect_btn, "clicked", G_CALLBACK (on_wifi_pwd_submit), p);
  g_signal_connect (entry, "activate", G_CALLBACK (on_wifi_pwd_submit), p);

  gtk_window_set_child (GTK_WINDOW (dlg), box);
  gtk_widget_realize (dlg);
  GdkSurface *surf = gtk_native_get_surface (GTK_NATIVE (dlg));
  if (surf != nullptr) {
    HWND hwnd = gdk_win32_surface_get_handle (surf);
    if (hwnd != nullptr) {
      LONG_PTR ex = GetWindowLongPtrW (hwnd, GWL_EXSTYLE);
      SetWindowLongPtrW (hwnd, GWL_EXSTYLE,
                         (ex | WS_EX_TOOLWINDOW | WS_EX_TOPMOST) & ~WS_EX_APPWINDOW);
      RECT wa = {};
      SystemParametersInfoW (SPI_GETWORKAREA, 0, &wa, 0);
      RECT wr = {};
      GetWindowRect (hwnd, &wr);
      int w = std::max<int> (wr.right - wr.left, 360);
      int h = std::max<int> (wr.bottom - wr.top, 160);
      int x = wa.left + ((wa.right - wa.left) - w) / 2;
      int y = wa.top + ((wa.bottom - wa.top) - h) / 2;
      SetWindowPos (hwnd, HWND_TOPMOST, x, y, 0, 0,
                    SWP_NOSIZE | SWP_SHOWWINDOW);
    }
  }
  gtk_widget_set_visible (dlg, TRUE);
  gtk_window_present (GTK_WINDOW (dlg));
  gtk_widget_grab_focus (entry);
}

static void
on_wifi_network_activate (gpointer user_data)
{
  WifiRowTarget *row = static_cast<WifiRowTarget *> (user_data);
  if (row == nullptr || row->menu == nullptr || row->menu->networks == nullptr)
    return;
  if (row->index >= row->menu->networks->len)
    return;

  const WinomeWifiNetwork *net = static_cast<const WinomeWifiNetwork *>
      (g_ptr_array_index (row->menu->networks, row->index));

  if (net->connected) {
    run_wifi_task_async (row->menu->panel, net->ssid, nullptr, "", FALSE,
                         true, false);
    return;
  }

  if (winome_wifi_has_profile (net->ssid)) {
    run_wifi_task_async (row->menu->panel, net->ssid, nullptr, "", FALSE,
                         false, true);
  } else if (!net->secured) {
    run_wifi_task_async (row->menu->panel, net->ssid,
                         winome_wifi_connect_with_password, "", TRUE,
                         false, false);
  } else {
    show_wifi_password_dialog (net->ssid, row->menu->panel);
  }
}

static GtkWidget *
build_wifi_menu (gpointer user_data)
{
  GtkWidget *panel = GTK_WIDGET (user_data);
  winome_wifi_trigger_scan_async ();
  WinomeWifiNetwork *list = nullptr;
  guint count = 0;

  if (!winome_wifi_scan (&list, &count) || list == nullptr)
    return NULL;

  if (count == 0) {
    winome_wifi_free_networks (list);
    return NULL;   // radio on, nothing in range: leave the arrow inert
  }

  WifiMenu *m = g_new0 (WifiMenu, 1);
  m->panel = panel;
  m->networks = g_ptr_array_new_with_free_func (g_free);
  m->labels = g_ptr_array_new_with_free_func (g_free);
  m->rows = g_ptr_array_new_with_free_func (g_free);

  for (guint i = 0; i < count; i++) {
    WinomeWifiNetwork *net = g_new0 (WinomeWifiNetwork, 1);
    *net = list[i];
    g_ptr_array_add (m->networks, net);

    // GNOME renders "SSID   72%" for network rows.
    g_ptr_array_add (m->labels,
                     g_strdup_printf ("%s   %d%%", list[i].ssid,
                                      list[i].signal_quality));
  }
  winome_wifi_free_networks (list);

  WinomeMenuEntry *items = g_new0 (WinomeMenuEntry, count);
  for (guint i = 0; i < count; i++) {
    const WinomeWifiNetwork *net =
        static_cast<const WinomeWifiNetwork *> (g_ptr_array_index (m->networks, i));
    WifiRowTarget *row = g_new0 (WifiRowTarget, 1);

    row->menu = m;
    row->index = i;
    g_ptr_array_add (m->rows, row);

    items[i].label = static_cast<const char *> (g_ptr_array_index (m->labels, i));
    items[i].icon_name = net->secured ? "network-wireless-encrypted-symbolic"
                                      : "network-wireless-symbolic";
    items[i].checked = net->connected;
    items[i].callback = on_wifi_network_activate;
    items[i].user_data = row;
  }

  // Upstream NetworkMenu calls
  //   menu.setHeader('network-wireless-symbolic', _('Wi-Fi'), subtitle)
  // where subtitle is "Connected" / "Not Connected" / the wired fallback. The
  // badge goes blue (.active) once associated.
  WinomeWifiStatus st;
  const gboolean associated = winome_wifi_get_status (&st) && st.connected;
  const char *subtitle = st.ssid[0] != '\0' ? st.ssid
                                            : tr ("Not Connected", "未连接");

  GtkWidget *menu = build_quick_menu_full (items, count,
                                           "network-wireless-symbolic",
                                           tr ("Wi-Fi", "无线网络"),
                                           subtitle, associated);
  g_object_set_data_full (G_OBJECT (menu), "wifi-menu", m, wifi_menu_free);
  g_free (items);
  return menu;
}

static GtkWidget *
build_power_mode_menu (gpointer user_data)
{
  GtkWidget *panel = GTK_WIDGET (user_data);
  WinomeMenuEntry items[WINOME_POWER_MODE_COUNT];
  PowerModeTarget *targets = g_new0 (PowerModeTarget, WINOME_POWER_MODE_COUNT);
  WinomePowerMode active = winome_power_get_active ();
  guint n = 0;
  int m;

  for (m = 0; m < WINOME_POWER_MODE_COUNT; m++) {
    WinomePowerMode mode = static_cast<WinomePowerMode> (m);
    if (!winome_power_mode_available (mode))
      continue;
    targets[n].mode = mode;
    targets[n].panel = panel;
    items[n].label = winome_power_mode_label (mode);
    items[n].icon_name = winome_power_mode_icon (mode);
    items[n].checked = (mode == active);
    items[n].callback = on_power_mode_activate;
    items[n].user_data = &targets[n];
    n++;
  }

  if (n == 0) {
    g_free (targets);
    return NULL;   // no stock plans installed: leave the arrow inert
  }

  // Header: "Power Mode" with the current plan's glyph in the badge, shown
  // active while a plan is selected. Falls back to Balanced when the scheme
  // cannot be read.
  WinomePowerMode shown =
      (active == WINOME_POWER_MODE_COUNT) ? WINOME_POWER_BALANCED : active;

  GtkWidget *menu = build_quick_menu_full (
      items, n, winome_power_mode_icon (shown),
      tr ("Power Mode", "电源模式"), winome_power_mode_label (shown),
      active != WINOME_POWER_MODE_COUNT);
  // Free the targets once the menu is destroyed.
  g_object_set_data_full (G_OBJECT (menu), "power-mode-targets", targets, g_free);

  return menu;
}

typedef struct {
  GPtrArray *labels;
} BluetoothMenu;

static void
bluetooth_menu_free (gpointer data)
{
  BluetoothMenu *m = static_cast<BluetoothMenu *> (data);
  if (m == nullptr)
    return;
  if (m->labels != nullptr)
    g_ptr_array_free (m->labels, TRUE);
  g_free (m);
}

static void
on_bluetooth_device_activate (gpointer G_GNUC_UNUSED user_data)
{
  ShellExecuteW (nullptr, L"open", L"ms-settings:bluetooth", nullptr, nullptr,
                 SW_SHOWNORMAL);
}

static GtkWidget *
build_bluetooth_menu (gpointer G_GNUC_UNUSED user_data)
{
  winome::BluetoothDevice devs[16] = {};
  int count = winome::get_bluetooth_devices (devs, 16);
  if (count < 0)
    count = 0;

  BluetoothMenu *m = g_new0 (BluetoothMenu, 1);
  m->labels = g_ptr_array_new_with_free_func (g_free);

  int total = count + 1;
  WinomeMenuEntry *items = g_new0 (WinomeMenuEntry, total);
  for (int i = 0; i < count; ++i) {
    char *copy = g_strdup (devs[i].name);
    g_ptr_array_add (m->labels, copy);
    items[i].label = copy;
    items[i].icon_name = "bluetooth-active-symbolic";
    items[i].checked = devs[i].connected ? TRUE : FALSE;
    items[i].callback = on_bluetooth_device_activate;
    items[i].user_data = NULL;
  }

  items[count].label = tr ("Bluetooth Settings", "蓝牙设置");
  items[count].icon_name = "preferences-system-symbolic";
  items[count].checked = FALSE;
  items[count].callback = on_bluetooth_device_activate;
  items[count].user_data = NULL;

  bool bt_present = false, bt_enabled = false;
  char bt_name[249] = "";
  winome::get_bluetooth_status (&bt_present, &bt_enabled, bt_name,
                                sizeof (bt_name));
  const char *subtitle = bt_name[0] != '\0'
                             ? bt_name
                             : (bt_enabled ? tr ("On", "已开启")
                                           : tr ("Off", "已关闭"));

  GtkWidget *menu = build_quick_menu_full (items, static_cast<guint> (total),
                                           "bluetooth-active-symbolic",
                                           tr ("Bluetooth", "蓝牙"),
                                           subtitle,
                                           bt_enabled ? TRUE : FALSE);
  g_object_set_data_full (G_OBJECT (menu), "bluetooth-menu", m,
                          bluetooth_menu_free);
  g_free (items);
  return menu;
}

GtkWidget *
winome_quick_settings_new (void)
{
  GtkWidget *root;
  GtkWidget *grid;
  GtkWidget *volume_slider, *volume_icon, *volume_button;
  GtkWidget *brightness_slider, *brightness_row = NULL;
  GtkWidget *wlan = NULL, *bluetooth = NULL, *powermode = NULL;
  GtkWidget *nightlight = NULL, *darkmode = NULL, *dnd = NULL;
  GtkWidget *keyboard = NULL, *airplane = NULL;
  GtkWidget *power_icon, *power_title, *power_toggle;

  root = gtk_box_new (GTK_ORIENTATION_VERTICAL, 12);
  gtk_widget_add_css_class (root, "quick-settings");

  grid = winome_quick_settings_grid_new ();
  gtk_widget_add_css_class (grid, "quick-settings-grid");
  g_object_set_data (G_OBJECT (root), "qs-grid", grid);
  gtk_box_append (GTK_BOX (root), grid);

  GtkGesture *grid_click = gtk_gesture_click_new ();
  gtk_gesture_single_set_button (GTK_GESTURE_SINGLE (grid_click), 0);
  gtk_event_controller_set_propagation_phase (GTK_EVENT_CONTROLLER (grid_click),
                                              GTK_PHASE_CAPTURE);
  g_signal_connect (grid_click, "pressed",
                    G_CALLBACK (on_grid_capture_pressed), root);
  gtk_widget_add_controller (grid, GTK_EVENT_CONTROLLER (grid_click));

  // --- Row 0: system item (battery + action buttons), full width ---
  {
    GtkWidget *row = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 12);
    gtk_widget_add_css_class (row, "quick-settings-system-item");

    // PowerToggle: non-toggling .quick-toggle.power-item with battery icon +
    // percentage as title (status/system.js); clicking opens the Power Off Menu
    // below Row 0.
    GtkWidget *power = create_toggle_capsule (false,
                                              "battery-level-100-symbolic",
                                              "100%", NULL,
                                              &power_icon, &power_title, NULL);
    power_toggle = power;
    gtk_widget_add_css_class (power, "power-item");
    gtk_widget_set_focus_on_click (power, FALSE);
    gtk_widget_set_hexpand (power, FALSE);
    gtk_widget_set_vexpand (power, FALSE);
    g_signal_connect (power, "clicked",
                      G_CALLBACK (on_power_item_clicked), row);
    gtk_box_append (GTK_BOX (row), power);

    // x_expand spacer: laptop-style layout when a battery is present, pushing
    // the action buttons to the right edge.
    GtkWidget *spacer = gtk_label_new ("");
    gtk_widget_set_hexpand (spacer, TRUE);
    gtk_box_append (GTK_BOX (row), spacer);

    GtkWidget *btn;
    btn = make_icon_button ("screenshooter-symbolic");
    g_signal_connect (btn, "clicked", G_CALLBACK (on_screenshot_clicked), NULL);
    gtk_box_append (GTK_BOX (row), btn);

    btn = make_icon_button ("preferences-system-symbolic");
    g_signal_connect (btn, "clicked", G_CALLBACK (on_settings_clicked), NULL);
    gtk_box_append (GTK_BOX (row), btn);

    btn = make_icon_button ("system-lock-screen-symbolic");
    g_signal_connect (btn, "clicked", G_CALLBACK (on_lock_clicked), NULL);
    gtk_box_append (GTK_BOX (row), btn);

    btn = make_icon_button ("system-shutdown-symbolic");
    g_signal_connect (btn, "clicked", G_CALLBACK (on_power_item_clicked), row);
    gtk_box_append (GTK_BOX (row), btn);

    winome_quick_settings_grid_attach (grid, row, 2);
  }

  // --- Row 1: volume slider, full width ---
  {
    GtkWidget *row = make_quick_slider ("audio-volume-high-symbolic",
                                        &volume_slider, &volume_icon,
                                        &volume_button);
    g_signal_connect (volume_button, "clicked",
                      G_CALLBACK (on_volume_mute_clicked), root);
    winome_quick_settings_grid_attach (grid, row, 2);
  }

  // --- Row 2: brightness slider, full width ---
  {
    GtkWidget *row = make_quick_slider ("display-brightness-symbolic",
                                        &brightness_slider, NULL, NULL);
    brightness_row = row;
    winome_quick_settings_grid_attach (grid, row, 2);
  }

  // --- Row 3+: toggles grid (2 columns) ---
  {
    // The subtitle mirrors the active plan; fall back to Balanced when the
    // scheme cannot be read (no PowrProf / only OEM plans).
    WinomePowerMode detected = winome_power_get_active ();
    WinomePowerMode active_power =
        (detected == WINOME_POWER_MODE_COUNT) ? WINOME_POWER_BALANCED
                                              : detected;

    // Order follows panel.js:381-405. The full-width rows (system item,
    // volume, brightness) come first, then the half-width toggles in the
    // sequence GNOME registers them. The order is what produces the panel's
    // visual grouping — network and bluetooth lead the toggles, the theme
    // pair sits together, and the keyboard-brightness / rfkill pair lands last
    // because upstream registers those after night light and DND.
    char ssid[97] = "";
    bool have_name = winome::get_network_name (ssid, sizeof (ssid));
    wlan = make_quick_menu_toggle ("network-wireless-symbolic",
                                   tr ("Wi-Fi", "无线网络"),
                                   have_name ? ssid : tr ("Not Connected", "未连接"),
                                   build_wifi_menu, root);
    g_signal_connect (quick_menu_toggle_button (wlan), "toggled",
                      G_CALLBACK (on_wlan_toggled), root);
    winome_quick_settings_grid_attach (grid, wlan, 1);

    bluetooth = make_quick_menu_toggle ("bluetooth-active-symbolic",
                                        tr ("Bluetooth", "蓝牙"),
                                        tr ("Off", "已关闭"),
                                        build_bluetooth_menu, root);
    g_signal_connect (quick_menu_toggle_button (bluetooth), "toggled",
                      G_CALLBACK (on_bluetooth_toggled), root);
    winome_quick_settings_grid_attach (grid, bluetooth, 1);

    powermode = make_quick_menu_toggle ("power-profile-balanced-symbolic",
                                        tr ("Power Mode", "电源模式"),
                                        winome_power_mode_label (active_power),
                                        build_power_mode_menu, root);
    g_signal_connect (quick_menu_toggle_button (powermode), "toggled",
                      G_CALLBACK (on_power_mode_toggled), root);
    winome_quick_settings_grid_attach (grid, powermode, 1);

    nightlight = make_quick_toggle ("night-light-symbolic",
                                    tr ("Night Light", "夜间模式"),
                                    NULL, NULL, NULL);
    g_signal_connect (nightlight, "toggled",
                      G_CALLBACK (on_night_light_toggled), NULL);
    winome_quick_settings_grid_attach (grid, nightlight, 1);

    darkmode = make_quick_toggle ("dark-mode-symbolic",
                                  tr ("Dark Style", "深色主题"),
                                  NULL, NULL, NULL);
    bool is_dark = false;
    winome::get_dark_mode (&is_dark);
    gtk_toggle_button_set_active (GTK_TOGGLE_BUTTON (darkmode), is_dark);
    g_signal_connect (darkmode, "toggled",
                      G_CALLBACK (on_dark_mode_toggled), NULL);
    winome_quick_settings_grid_attach (grid, darkmode, 1);

    dnd = make_quick_toggle ("notifications-disabled-symbolic",
                             tr ("Do Not Disturb", "勿扰模式"),
                             NULL, NULL, NULL);
    g_signal_connect (dnd, "toggled",
                      G_CALLBACK (on_dnd_toggled), NULL);
    winome_quick_settings_grid_attach (grid, dnd, 1);

    // keyboard_brightness and rfkill are the last two upstream registers.
    keyboard = make_quick_menu_toggle ("input-keyboard-symbolic",
                                       tr ("Keyboard", "键盘"),
                                       NULL,
                                       NULL, NULL);
    gtk_widget_set_visible (keyboard,
                            winome::has_keyboard_backlight () ? TRUE : FALSE);
    winome_quick_settings_grid_attach (grid, keyboard, 1);

    airplane = make_quick_toggle ("airplane-mode-symbolic",
                                  tr ("Airplane Mode", "飞行模式"),
                                  NULL, NULL, NULL);
    g_signal_connect (airplane, "toggled",
                      G_CALLBACK (on_airplane_toggled), root);
    winome_quick_settings_grid_attach (grid, airplane, 1);
  }

  // Volume and brightness sliders write to the system.
  quick_slider_connect_value_changed (volume_slider,
                                      G_CALLBACK (on_volume_changed), NULL);
  quick_slider_connect_value_changed (brightness_slider,
                                      G_CALLBACK (on_brightness_changed), NULL);

  g_object_set_data (G_OBJECT (root), "power-toggle", power_toggle);
  g_object_set_data (G_OBJECT (root), "power-icon", power_icon);
  g_object_set_data (G_OBJECT (root), "power-title", power_title);
  g_object_set_data (G_OBJECT (root), "volume-slider", volume_slider);
  g_object_set_data (G_OBJECT (root), "volume-icon", volume_icon);
  g_object_set_data (G_OBJECT (root), "brightness-slider", brightness_slider);
  g_object_set_data (G_OBJECT (root), "brightness-row", brightness_row);
  g_object_set_data (G_OBJECT (root), "wlan-toggle", wlan);
  g_object_set_data (G_OBJECT (root), "bluetooth-toggle", bluetooth);
  g_object_set_data (G_OBJECT (root), "powermode-toggle", powermode);
  g_object_set_data (G_OBJECT (root), "nightlight-toggle", nightlight);
  g_object_set_data (G_OBJECT (root), "darkmode-toggle", darkmode);
  g_object_set_data (G_OBJECT (root), "dnd-toggle", dnd);
  g_object_set_data (G_OBJECT (root), "airplane-toggle", airplane);

  winome_quick_settings_refresh (root);

  return root;
}

void
winome_quick_settings_refresh (GtkWidget *quick_settings)
{
  GtkWidget *power_icon =
      GTK_WIDGET (g_object_get_data (G_OBJECT (quick_settings), "power-icon"));
  GtkWidget *power_title =
      GTK_WIDGET (g_object_get_data (G_OBJECT (quick_settings), "power-title"));
  GtkWidget *power_toggle =
      GTK_WIDGET (g_object_get_data (G_OBJECT (quick_settings), "power-toggle"));
  GtkWidget *volume_slider =
      GTK_WIDGET (g_object_get_data (G_OBJECT (quick_settings), "volume-slider"));
  GtkWidget *volume_icon =
      GTK_WIDGET (g_object_get_data (G_OBJECT (quick_settings), "volume-icon"));
  GtkWidget *brightness_slider =
      GTK_WIDGET (g_object_get_data (G_OBJECT (quick_settings), "brightness-slider"));
  GtkWidget *brightness_row =
      GTK_WIDGET (g_object_get_data (G_OBJECT (quick_settings), "brightness-row"));
  GtkWidget *wlan_toggle =
      GTK_WIDGET (g_object_get_data (G_OBJECT (quick_settings), "wlan-toggle"));
  GtkWidget *bluetooth_toggle =
      GTK_WIDGET (g_object_get_data (G_OBJECT (quick_settings), "bluetooth-toggle"));
  GtkWidget *powermode_toggle =
      GTK_WIDGET (g_object_get_data (G_OBJECT (quick_settings), "powermode-toggle"));
  GtkWidget *nightlight_toggle =
      GTK_WIDGET (g_object_get_data (G_OBJECT (quick_settings), "nightlight-toggle"));
  GtkWidget *darkmode_toggle =
      GTK_WIDGET (g_object_get_data (G_OBJECT (quick_settings), "darkmode-toggle"));
  GtkWidget *dnd_toggle =
      GTK_WIDGET (g_object_get_data (G_OBJECT (quick_settings), "dnd-toggle"));
  GtkWidget *airplane_toggle =
      GTK_WIDGET (g_object_get_data (G_OBJECT (quick_settings), "airplane-toggle"));

  winome::BatteryStatus battery;
  if (power_toggle && power_icon && power_title
      && winome::get_battery_status (&battery)) {
    gchar text[16];
    snprintf (text, sizeof (text), "%d%%", battery.percentage);
    gtk_label_set_text (GTK_LABEL (power_title), text);

    gtk_image_set_from_icon_name (GTK_IMAGE (power_icon),
                                  battery_icon_name (battery));

    // Native keeps the PowerToggle hidden when there is no battery/UPS and
    // shows the action buttons pushed to the right edge instead.
    gtk_widget_set_visible (power_toggle, TRUE);
  } else if (power_toggle) {
    gtk_widget_set_visible (power_toggle, FALSE);
  }

  double volume = 0.0;
  bool muted = false;
  if (volume_slider && winome::get_volume (&volume)) {
    g_signal_handlers_block_by_func (volume_slider,
        (gpointer) on_volume_changed, NULL);
    quick_slider_set_value (volume_slider, volume);
    g_signal_handlers_unblock_by_func (volume_slider,
        (gpointer) on_volume_changed, NULL);

    if (volume_icon) {
      winome::get_mute (&muted);
      gtk_image_set_from_icon_name (GTK_IMAGE (volume_icon),
                                    volume_icon_name (volume, muted));
    }
  }

  double brightness = 0.5;
  const bool has_brightness = winome::get_brightness (&brightness);
  if (brightness_row)
    gtk_widget_set_visible (brightness_row, has_brightness ? TRUE : FALSE);
  if (brightness_slider && has_brightness) {
    g_signal_handlers_block_by_func (brightness_slider,
        (gpointer) on_brightness_changed, NULL);
    quick_slider_set_value (brightness_slider, brightness);
    g_signal_handlers_unblock_by_func (brightness_slider,
        (gpointer) on_brightness_changed, NULL);
  }

  if (wlan_toggle) {
    WinomeWifiStatus wifi_st = {};
    gboolean have_wifi = winome_wifi_get_status (&wifi_st);
    set_toggle_active_blocked (wlan_toggle,
                               have_wifi && wifi_st.enabled,
                               G_CALLBACK (on_wlan_toggled), quick_settings);

    MenuToggleParts *parts = static_cast<MenuToggleParts *> (
        g_object_get_data (G_OBJECT (wlan_toggle), "menu-toggle-parts"));
    if (parts && parts->subtitle_label) {
      const char *sub = (!have_wifi || !wifi_st.enabled)
                            ? tr ("Off", "已关闭")
                            : ((wifi_st.connected && wifi_st.ssid[0] != '\0')
                                   ? wifi_st.ssid
                                   : tr ("Not Connected", "未连接"));
      gtk_label_set_text (GTK_LABEL (parts->subtitle_label), sub);
    }
  }

  if (bluetooth_toggle) {
    bool bt_present = false, bt_enabled = false;
    char bt_name[249] = "";
    winome::get_bluetooth_status (&bt_present, &bt_enabled, bt_name,
                                  sizeof (bt_name));
    gtk_widget_set_visible (bluetooth_toggle, bt_present ? TRUE : FALSE);
    set_toggle_active_blocked (bluetooth_toggle,
                               bt_enabled ? TRUE : FALSE,
                               G_CALLBACK (on_bluetooth_toggled), quick_settings);

    MenuToggleParts *parts = static_cast<MenuToggleParts *> (
        g_object_get_data (G_OBJECT (bluetooth_toggle), "menu-toggle-parts"));
    if (parts && parts->subtitle_label) {
      const char *sub = bt_name[0] != '\0'
                            ? bt_name
                            : (bt_enabled ? tr ("On", "已开启")
                                          : tr ("Off", "已关闭"));
      gtk_label_set_text (GTK_LABEL (parts->subtitle_label), sub);
    }
  }

  if (powermode_toggle) {
    MenuToggleParts *parts = static_cast<MenuToggleParts *> (
        g_object_get_data (G_OBJECT (powermode_toggle), "menu-toggle-parts"));
    WinomePowerMode detected = winome_power_get_active ();
    WinomePowerMode active = (detected == WINOME_POWER_MODE_COUNT)
                                 ? WINOME_POWER_BALANCED
                                 : detected;
    if (active != WINOME_POWER_BALANCED) {
      g_object_set_data (G_OBJECT (quick_settings), "last-power-mode",
                         GINT_TO_POINTER ((int)active + 1));
    }
    set_toggle_active_blocked (powermode_toggle,
                               active != WINOME_POWER_BALANCED,
                               G_CALLBACK (on_power_mode_toggled), quick_settings);
    if (parts) {
      if (parts->subtitle_label) {
        gtk_label_set_text (GTK_LABEL (parts->subtitle_label),
                            winome_power_mode_label (active));
      }
      if (parts->icon) {
        gtk_image_set_from_icon_name (GTK_IMAGE (parts->icon),
                                      winome_power_mode_icon (active));
      }
    }
  }

  if (nightlight_toggle) {
    bool night_on = false;
    if (winome::get_night_light (&night_on)) {
      g_signal_handlers_block_by_func (nightlight_toggle,
          (gpointer) on_night_light_toggled, NULL);
      gtk_toggle_button_set_active (GTK_TOGGLE_BUTTON (nightlight_toggle),
                                    night_on ? TRUE : FALSE);
      g_signal_handlers_unblock_by_func (nightlight_toggle,
          (gpointer) on_night_light_toggled, NULL);
    }
  }

  if (darkmode_toggle) {
    bool is_dark = false;
    if (winome::get_dark_mode (&is_dark)) {
      g_signal_handlers_block_by_func (darkmode_toggle,
          (gpointer) on_dark_mode_toggled, NULL);
      gtk_toggle_button_set_active (GTK_TOGGLE_BUTTON (darkmode_toggle), is_dark);
      g_signal_handlers_unblock_by_func (darkmode_toggle,
          (gpointer) on_dark_mode_toggled, NULL);
    }
  }

  if (dnd_toggle) {
    bool dnd_on = false;
    if (winome::get_do_not_disturb (&dnd_on)) {
      g_signal_handlers_block_by_func (dnd_toggle,
          (gpointer) on_dnd_toggled, NULL);
      gtk_toggle_button_set_active (GTK_TOGGLE_BUTTON (dnd_toggle),
                                    dnd_on ? TRUE : FALSE);
      g_signal_handlers_unblock_by_func (dnd_toggle,
          (gpointer) on_dnd_toggled, NULL);
    }
  }

  if (airplane_toggle) {
    bool air_on = false;
    if (winome::get_airplane_mode (&air_on)) {
      g_signal_handlers_block_by_func (airplane_toggle,
          (gpointer) on_airplane_toggled, quick_settings);
      gtk_toggle_button_set_active (GTK_TOGGLE_BUTTON (airplane_toggle),
                                    air_on ? TRUE : FALSE);
      g_signal_handlers_unblock_by_func (airplane_toggle,
          (gpointer) on_airplane_toggled, quick_settings);
    }
  }
}
