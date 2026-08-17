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

// $scalable_icon_size from gnome-shell-sass (1.091em at "Sans 11" -> 16px).
#define QUICK_ICON_SIZE 16

// --- QuickToggle -------------------------------------------------------------
//
// Standalone toggle, matching QuickToggle in quickSettings.js:
//   .quick-toggle.button
//     box  (St.BoxLayout, spacing 9px)
//       .quick-toggle-icon   (16px)
//       titleBox             (x_expand, vertical)
//         .quick-toggle-title
//         .quick-toggle-subtitle   (hidden when subtitle == NULL)
//
// The ltr box padding (15px left / 12px right) is re-asserted in the GTK4 CSS
// overrides because the conversion drops the :ltr/:rtl rules.
static GtkWidget *
make_quick_toggle (const char *icon_name, const char *title,
                   const char *subtitle,
                   GtkWidget **icon_out, GtkWidget **title_out)
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
  gtk_widget_set_focus_on_click (button, FALSE);

  box = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 9);
  gtk_widget_set_valign (box, GTK_ALIGN_CENTER);
  gtk_widget_set_halign (box, GTK_ALIGN_START);
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
    gtk_widget_set_halign (subtitle_label, GTK_ALIGN_START);
    gtk_widget_set_valign (subtitle_label, GTK_ALIGN_CENTER);
    gtk_box_append (GTK_BOX (title_box), subtitle_label);
  }

  if (icon_out)
    *icon_out = icon;
  if (title_out)
    *title_out = title_label;

  return button;
}

// --- QuickMenuToggle ---------------------------------------------------------
//
// Toggle with a secondary-menu arrow, matching QuickMenuToggle in
// quickSettings.js:
//   .quick-toggle-has-menu (button)
//     box
//       .quick-toggle                (icon + title + subtitle)
//       .quick-toggle-separator      (1px vertical rule)
//       .quick-toggle-menu-button    (go-next-symbolic icon)
//
// The accent fill is keyed off `.quick-toggle:checked` and
// `.quick-toggle-menu-button:checked` in the stylesheet, so the outer button's
// active state is mirrored onto those two nodes (GTK4 has no state
// inheritance). The separator colors itself via the ancestor selector
// `.quick-toggle-has-menu:checked .quick-toggle-separator`.

typedef struct {
  GtkWidget *toggle_part;      // the .quick-toggle node
  GtkWidget *menu_button_part; // the .quick-toggle-menu-button node
} MenuToggleParts;

static void
sync_menu_toggle_checked (GtkToggleButton *button, gpointer user_data)
{
  MenuToggleParts *parts = static_cast<MenuToggleParts *>(user_data);
  if (gtk_toggle_button_get_active (button)) {
    gtk_widget_set_state_flags (parts->toggle_part,
                                GTK_STATE_FLAG_CHECKED, FALSE);
    gtk_widget_set_state_flags (parts->menu_button_part,
                                GTK_STATE_FLAG_CHECKED, FALSE);
  } else {
    gtk_widget_unset_state_flags (parts->toggle_part,
                                  GTK_STATE_FLAG_CHECKED);
    gtk_widget_unset_state_flags (parts->menu_button_part,
                                  GTK_STATE_FLAG_CHECKED);
  }
}

static GtkWidget *
make_quick_menu_toggle (const char *icon_name, const char *title,
                        const char *subtitle)
{
  GtkWidget *button;
  GtkWidget *box;
  GtkWidget *toggle_part;
  GtkWidget *content_box;
  GtkWidget *icon;
  GtkWidget *title_box;
  GtkWidget *title_label;
  GtkWidget *subtitle_label;
  GtkWidget *separator;
  GtkWidget *menu_button;
  GtkWidget *arrow;
  MenuToggleParts *parts;

  button = gtk_toggle_button_new ();
  gtk_widget_add_css_class (button, "quick-toggle-has-menu");
  gtk_widget_set_focus_on_click (button, FALSE);

  box = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);
  gtk_widget_set_valign (box, GTK_ALIGN_FILL);
  gtk_widget_set_halign (box, GTK_ALIGN_FILL);
  gtk_button_set_child (GTK_BUTTON (button), box);

  // The toggle part. A plain box (not a nested button) so clicks land on the
  // outer toggle button; it carries the .quick-toggle capsule background and
  // fills the button height like native (min-height 3.273em).
  toggle_part = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);
  gtk_widget_add_css_class (toggle_part, "quick-toggle");
  gtk_widget_add_css_class (toggle_part, "button");
  gtk_widget_set_hexpand (toggle_part, TRUE);
  gtk_widget_set_valign (toggle_part, GTK_ALIGN_FILL);
  gtk_box_append (GTK_BOX (box), toggle_part);

  // Content box: matches `.quick-toggle > box` in the stylesheet (the 9px
  // spacing + ltr padding live here). Vertically centered in the capsule.
  content_box = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 9);
  gtk_widget_set_valign (content_box, GTK_ALIGN_CENTER);
  gtk_widget_set_halign (content_box, GTK_ALIGN_START);
  gtk_box_append (GTK_BOX (toggle_part), content_box);

  icon = gtk_image_new_from_icon_name (icon_name);
  gtk_image_set_pixel_size (GTK_IMAGE (icon), QUICK_ICON_SIZE);
  gtk_widget_add_css_class (icon, "quick-toggle-icon");
  gtk_widget_set_valign (icon, GTK_ALIGN_CENTER);
  gtk_box_append (GTK_BOX (content_box), icon);

  title_box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  gtk_widget_set_hexpand (title_box, TRUE);
  gtk_widget_set_valign (title_box, GTK_ALIGN_CENTER);
  gtk_box_append (GTK_BOX (content_box), title_box);

  title_label = gtk_label_new (title);
  gtk_widget_add_css_class (title_label, "quick-toggle-title");
  gtk_label_set_ellipsize (GTK_LABEL (title_label), PANGO_ELLIPSIZE_END);
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

  // 1px vertical rule; its background-color comes from the stylesheet
  // (.quick-toggle-has-menu .quick-toggle-separator).
  separator = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  gtk_widget_add_css_class (separator, "quick-toggle-separator");
  gtk_widget_set_size_request (separator, 1, -1);
  gtk_widget_set_valign (separator, GTK_ALIGN_FILL);
  gtk_box_append (GTK_BOX (box), separator);

  // The ">" menu button area (.icon-button fill + go-next arrow), filling the
  // capsule height like native (y_expand).
  menu_button = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);
  gtk_widget_add_css_class (menu_button, "quick-toggle-menu-button");
  gtk_widget_add_css_class (menu_button, "icon-button");
  gtk_widget_set_valign (menu_button, GTK_ALIGN_FILL);
  arrow = gtk_image_new_from_icon_name ("go-next-symbolic");
  gtk_image_set_pixel_size (GTK_IMAGE (arrow), QUICK_ICON_SIZE);
  gtk_widget_set_valign (arrow, GTK_ALIGN_CENTER);
  gtk_widget_set_halign (arrow, GTK_ALIGN_CENTER);
  gtk_box_append (GTK_BOX (menu_button), arrow);
  gtk_box_append (GTK_BOX (box), menu_button);

  parts = g_new0 (MenuToggleParts, 1);
  parts->toggle_part = toggle_part;
  parts->menu_button_part = menu_button;
  g_object_set_data_full (G_OBJECT (button), "menu-toggle-parts",
                          parts, g_free);
  g_signal_connect (button, "toggled",
                    G_CALLBACK (sync_menu_toggle_checked), parts);
  sync_menu_toggle_checked (GTK_TOGGLE_BUTTON (button), parts);

  return button;
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

static void
winome_quick_slider_set_value_from_x (WinomeQuickSlider *self, double x)
{
  int width = gtk_widget_get_width (GTK_WIDGET (self));
  double min_x = SLIDER_HANDLE_RADIUS;
  double max_x = width - SLIDER_HANDLE_RADIUS;
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
  double w = gtk_widget_get_width (widget);
  double h = gtk_widget_get_height (widget);
  double cy = h / 2.0;

  // Handle center: value * (width - handle) + handle/2, clamped like native.
  double cx = SLIDER_HANDLE_RADIUS +
              CLAMP (self->value, 0.0, 1.0) *
                  (w - 2.0 * SLIDER_HANDLE_RADIUS);

  GdkRGBA trough_color = {1.0, 1.0, 1.0, 0.15};  // rgba(255,255,255,0.15)
  GdkRGBA highlight_color = {0.208, 0.518, 0.894, 1.0};  // accent #3584e4
  GdkRGBA handle_color = {1.0, 1.0, 1.0, 1.0};

  // Trough: full width, 4px, rounded.
  append_rounded_color (snapshot, &trough_color,
                        0, cy - SLIDER_TROUGH_HEIGHT / 2.0,
                        w, SLIDER_TROUGH_HEIGHT, 2.0);

  // Highlight: from the origin (0) to the handle.
  if (cx > 0)
    append_rounded_color (snapshot, &highlight_color,
                          0, cy - SLIDER_TROUGH_HEIGHT / 2.0,
                          cx, SLIDER_TROUGH_HEIGHT, 2.0);

  // Handle: 18px white circle.
  if (w > 0)
    append_rounded_color (snapshot, &handle_color,
                          cx - SLIDER_HANDLE_RADIUS,
                          cy - SLIDER_HANDLE_RADIUS,
                          2.0 * SLIDER_HANDLE_RADIUS,
                          2.0 * SLIDER_HANDLE_RADIUS,
                          SLIDER_HANDLE_RADIUS);
}

static void
winome_quick_slider_measure (GtkWidget *widget G_GNUC_UNUSED,
                             GtkOrientation orientation,
                             int for_size G_GNUC_UNUSED,
                             int *minimum, int *natural,
                             int *minimum_baseline, int *natural_baseline)
{
  if (orientation == GTK_ORIENTATION_HORIZONTAL) {
    *minimum = 0;
    *natural = 150;
  } else {
    *minimum = 2 * SLIDER_HANDLE_RADIUS + 4;
    *natural = 2 * SLIDER_HANDLE_RADIUS + 4;
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
                   GtkWidget **icon_out)
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
on_lock_clicked (GtkButton *button G_GNUC_UNUSED, gpointer G_GNUC_UNUSED user_data)
{
  winome::lock_screen ();
}

// --- QuickSettingsGrid -------------------------------------------------------
//
// Port of the native QuickSettingsLayout (quickSettings.js) into a custom
// widget. GtkGrid would work, but it distributes surplus height to rows that
// report expand and misaligns the toggle capsules; the native layout sizes
// each row to its natural height and gives each child exactly its share.
//
// Layout (nColumns=2, row/column spacing 12px):
//   - A child with column-span 2 takes a whole row (system item, sliders).
//   - A child with column-span 1 is packed two per row (the toggles).
//   - childWidth = floor((width - columnSpacing) / 2); a spanned child gets
//     childWidth * span + columnSpacing * (span - 1).
//   - Row height = max natural height of the children in the row.

typedef struct {
  GtkWidget *widget;
  int col_span;
} QuickGridChild;

typedef struct {
  GtkWidget parent_instance;
  GPtrArray *children;
  int row_spacing;
  int column_spacing;
} WinomeQuickSettingsGrid;

typedef struct {
  GtkWidgetClass parent_class;
} WinomeQuickSettingsGridClass;

#define WINOME_TYPE_QUICK_SETTINGS_GRID (winome_quick_settings_grid_get_type ())
#define WINOME_QUICK_SETTINGS_GRID(obj) (G_TYPE_CHECK_INSTANCE_CAST ((obj), \
    WINOME_TYPE_QUICK_SETTINGS_GRID, WinomeQuickSettingsGrid))

G_DEFINE_TYPE (WinomeQuickSettingsGrid, winome_quick_settings_grid, GTK_TYPE_WIDGET)

// The rows: each entry is a GList of QuickGridChild* (in layout order).
static GList **
compute_rows (WinomeQuickSettingsGrid *self, int n_columns)
{
  GList **rows = g_new0 (GList *, self->children->len + 1);
  int n_rows = 0;
  int line_index = 0;
  GList *cur = NULL;

  for (guint i = 0; i < self->children->len; i++) {
    QuickGridChild *c = static_cast<QuickGridChild *> (g_ptr_array_index (self->children, i));
    if (c->col_span > n_columns)
      c->col_span = n_columns;

    if (line_index == 0 || line_index + c->col_span > n_columns) {
      if (cur != NULL)
        rows[n_rows++] = cur;
      cur = NULL;
      line_index = 0;
    }
    cur = g_list_append (cur, c);
    line_index = (line_index + c->col_span) % n_columns;
  }
  if (cur != NULL)
    rows[n_rows++] = cur;
  rows[n_rows] = NULL;

  return rows;
}

static int
max_child_height (GList *row)
{
  int h = 0;
  for (GList *l = row; l != NULL; l = l->next) {
    QuickGridChild *c = static_cast<QuickGridChild *>(l->data);
    int m, n;
    gtk_widget_measure (c->widget, GTK_ORIENTATION_VERTICAL, -1, &m, &n,
                        NULL, NULL);
    h = MAX (h, n);
  }
  return h;
}

static void
winome_quick_settings_grid_measure (GtkWidget *widget,
                                    GtkOrientation orientation,
                                    int for_size G_GNUC_UNUSED,
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
      QuickGridChild *c = static_cast<QuickGridChild *> (g_ptr_array_index (self->children, i));
      int m, n;
      gtk_widget_measure (c->widget, GTK_ORIENTATION_HORIZONTAL, -1,
                          &m, &n, NULL, NULL);
      min_w = MAX (min_w, m / c->col_span);
      nat_w = MAX (nat_w, n / c->col_span);
    }
    *minimum = n_columns * min_w + self->column_spacing;
    *natural = n_columns * nat_w + self->column_spacing;
  } else {
    GList **rows = compute_rows (self, n_columns);
    int min_h = 0, nat_h = 0;
    int n_rows = 0;
    for (int r = 0; rows[r] != NULL; r++) {
      int h = max_child_height (rows[r]);
      min_h += h;
      nat_h += h;
      n_rows++;
    }
    if (n_rows > 0) {
      min_h += (n_rows - 1) * self->row_spacing;
      nat_h += (n_rows - 1) * self->row_spacing;
    }
    *minimum = min_h;
    *natural = nat_h;
    for (int r = 0; rows[r] != NULL; r++)
      g_list_free (rows[r]);
    g_free (rows);
  }
}

static void
winome_quick_settings_grid_size_allocate (GtkWidget *widget,
                                          int width, int height, int baseline)
{
  WinomeQuickSettingsGrid *self = WINOME_QUICK_SETTINGS_GRID (widget);
  const int n_columns = 2;

  GList **rows = compute_rows (self, n_columns);

  int child_width = (width - self->column_spacing) / n_columns;
  int y = 0;

  for (int r = 0; rows[r] != NULL; r++) {
    GList *row = rows[r];
    int row_height = max_child_height (row);

    int line_index = 0;
    for (GList *l = row; l != NULL; l = l->next) {
      QuickGridChild *c = static_cast<QuickGridChild *>(l->data);
      int cw = child_width * c->col_span +
               self->column_spacing * (c->col_span - 1);
      int x = line_index * (child_width + self->column_spacing);

      GtkAllocation alloc;
      alloc.x = x;
      alloc.y = y;
      alloc.width = cw;
      alloc.height = row_height;
      gtk_widget_size_allocate (c->widget, &alloc, -1);

      line_index = (line_index + c->col_span) % n_columns;
    }

    y += row_height + self->row_spacing;
    g_list_free (row);
  }
  g_free (rows);
}

static void
winome_quick_settings_grid_init (WinomeQuickSettingsGrid *self)
{
  self->children = g_ptr_array_new_with_free_func (g_free);
  self->row_spacing = 12;
  self->column_spacing = 12;
}

static void
winome_quick_settings_grid_finalize (GObject *object)
{
  WinomeQuickSettingsGrid *self = WINOME_QUICK_SETTINGS_GRID (object);
  for (guint i = 0; i < self->children->len; i++) {
    QuickGridChild *c = static_cast<QuickGridChild *> (g_ptr_array_index (self->children, i));
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

// --- Panel widget ------------------------------------------------------------

GtkWidget *
winome_quick_settings_new (void)
{
  GtkWidget *root;
  GtkWidget *grid;
  GtkWidget *volume_slider, *volume_icon;
  GtkWidget *brightness_slider;
  GtkWidget *power_icon, *power_title, *power_toggle;

  root = gtk_box_new (GTK_ORIENTATION_VERTICAL, 12);
  gtk_widget_add_css_class (root, "quick-settings");

  grid = winome_quick_settings_grid_new ();
  gtk_widget_add_css_class (grid, "quick-settings-grid");
  gtk_box_append (GTK_BOX (root), grid);

  // --- Row 0: system item (battery + action buttons), full width ---
  {
    GtkWidget *row = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 12);
    gtk_widget_add_css_class (row, "quick-settings-system-item");

    // PowerToggle: .quick-toggle.power-item with the battery icon + percentage
    // as its title (status/system.js). .power-item resets the 12em min-size so
    // the capsule hugs the icon + text; keep it non-expanding so the spacer
    // pushes the action buttons to the right edge like native.
    GtkWidget *power = make_quick_toggle ("battery-level-100-symbolic",
                                          "100%", NULL,
                                          &power_icon, &power_title);
    power_toggle = power;
    gtk_widget_add_css_class (power, "power-item");
    gtk_widget_set_focus_on_click (power, FALSE);
    gtk_widget_set_hexpand (power, FALSE);
    gtk_widget_set_vexpand (power, FALSE);
    gtk_box_append (GTK_BOX (row), power);

    // x_expand spacer: laptop-style layout when a battery is present, pushing
    // the action buttons to the right edge.
    GtkWidget *spacer = gtk_label_new ("");
    gtk_widget_set_hexpand (spacer, TRUE);
    gtk_box_append (GTK_BOX (row), spacer);

    GtkWidget *btn;
    btn = make_icon_button ("screenshooter-symbolic");
    gtk_box_append (GTK_BOX (row), btn);

    btn = make_icon_button ("applications-system-symbolic");
    gtk_box_append (GTK_BOX (row), btn);

    btn = make_icon_button ("system-lock-screen-symbolic");
    g_signal_connect (btn, "clicked", G_CALLBACK (on_lock_clicked), NULL);
    gtk_box_append (GTK_BOX (row), btn);

    btn = make_icon_button ("system-shutdown-symbolic");
    gtk_box_append (GTK_BOX (row), btn);

    winome_quick_settings_grid_attach (grid, row, 2);
  }

  // --- Row 1: volume slider, full width ---
  {
    GtkWidget *row = make_quick_slider ("audio-volume-high-symbolic",
                                        &volume_slider, &volume_icon);
    winome_quick_settings_grid_attach (grid, row, 2);
  }

  // --- Row 2: brightness slider, full width ---
  {
    GtkWidget *row = make_quick_slider ("display-brightness-symbolic",
                                        &brightness_slider, NULL);
    winome_quick_settings_grid_attach (grid, row, 2);
  }

  // --- Row 3+: toggles grid (2 columns) ---
  {
    GtkWidget *wlan, *bluetooth, *powermode, *nightlight;
    GtkWidget *darkmode, *dnd, *keyboard, *airplane;
    int row = 3;

    wlan = make_quick_menu_toggle ("network-wireless-symbolic",
                                   tr ("Wi-Fi", "无线网络"),
                                   tr ("Connected", "已连接"));
    gtk_toggle_button_set_active (GTK_TOGGLE_BUTTON (wlan), TRUE);
    winome_quick_settings_grid_attach (grid, wlan, 1);

    bluetooth = make_quick_menu_toggle ("bluetooth-active-symbolic",
                                        tr ("Bluetooth", "蓝牙"),
                                        NULL);
    winome_quick_settings_grid_attach (grid, bluetooth, 1);
    row++;

    powermode = make_quick_menu_toggle ("power-profile-balanced-symbolic",
                                        tr ("Power Mode", "电源模式"),
                                        tr ("Balanced", "均衡"));
    winome_quick_settings_grid_attach (grid, powermode, 1);

    nightlight = make_quick_toggle ("night-light-symbolic",
                                    tr ("Night Light", "夜间模式"),
                                    NULL, NULL, NULL);
    winome_quick_settings_grid_attach (grid, nightlight, 1);
    row++;

    darkmode = make_quick_toggle ("dark-mode-symbolic",
                                  tr ("Dark Style", "深色主题"),
                                  NULL, NULL, NULL);
    gtk_toggle_button_set_active (GTK_TOGGLE_BUTTON (darkmode), TRUE);
    winome_quick_settings_grid_attach (grid, darkmode, 1);

    dnd = make_quick_toggle ("notifications-disabled-symbolic",
                             tr ("Do Not Disturb", "勿扰模式"),
                             NULL, NULL, NULL);
    winome_quick_settings_grid_attach (grid, dnd, 1);
    row++;

    keyboard = make_quick_menu_toggle ("input-keyboard-symbolic",
                                       tr ("Keyboard", "键盘"),
                                       NULL);
    gtk_toggle_button_set_active (GTK_TOGGLE_BUTTON (keyboard), TRUE);
    winome_quick_settings_grid_attach (grid, keyboard, 1);

    airplane = make_quick_toggle ("airplane-mode-symbolic",
                                  tr ("Airplane Mode", "飞行模式"),
                                  NULL, NULL, NULL);
    winome_quick_settings_grid_attach (grid, airplane, 1);
  }

  // Volume slider writes to the system.
  quick_slider_connect_value_changed (volume_slider,
                                      G_CALLBACK (on_volume_changed), NULL);

  g_object_set_data (G_OBJECT (root), "power-toggle", power_toggle);
  g_object_set_data (G_OBJECT (root), "power-icon", power_icon);
  g_object_set_data (G_OBJECT (root), "power-title", power_title);
  g_object_set_data (G_OBJECT (root), "volume-slider", volume_slider);
  g_object_set_data (G_OBJECT (root), "volume-icon", volume_icon);

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
}
