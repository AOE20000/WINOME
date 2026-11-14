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

// --- QuickToggleMenu (secondary menu) ----------------------------------------
//
// The popover that opens from a `.quick-toggle-menu-button` arrow, matching
// QuickToggleMenu in quickSettings.js:
//
//   super(sourceActor, 'quick-toggle-menu')
//     .quick-toggle-menu-container   (reactive, x_expand, y_expand:false)
//       .header                     (optional title, hidden here)
//       .popup-menu-content         (6px padding, #36363a, 20px radius)
//         .popup-menu-box
//           .popup-menu-item         (12px radius, active = lighten(#424247,9%))
//
// Both the container and the item classes already exist in
// gnome-shell-dark.css, so the menu reuses the theme verbatim — no extra CSS.
//
// GNOME anchors the menu's top edge to the *bottom of the toggle capsule*
// (a Clutter.BindConstraint on Y with offset = sourceActor.height), so the
// menu hangs directly under the row that opened it.

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
  winome_shell_panel_close_menu ();

  if (cb != NULL)
    cb (ud);
}

static GtkWidget *
make_quick_menu_item (const WinomeMenuEntry *entry)
{
  GtkWidget *item;
  GtkWidget *box;
  GtkWidget *icon = NULL;
  GtkWidget *label;

  item = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 12);
  gtk_widget_add_css_class (item, "popup-menu-item");
  gtk_widget_set_focus_on_click (item, FALSE);

  box = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);
  gtk_widget_set_valign (box, GTK_ALIGN_CENTER);
  gtk_widget_set_halign (box, GTK_ALIGN_START);
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
  gtk_widget_add_css_class (label, "popup-menu-item-label");
  gtk_label_set_xalign (GTK_LABEL (label), 0.0);
  gtk_widget_set_valign (label, GTK_ALIGN_CENTER);
  gtk_widget_set_hexpand (label, TRUE);
  gtk_box_append (GTK_BOX (box), label);

  // Radio-style active marker, matching `.popup-menu-item:checked` upstream.
  if (entry->checked) {
    GtkWidget *ornament = gtk_image_new_from_icon_name ("object-select-symbolic");
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

// QuickToggleMenu's header (quickSettings.js:389-430): a grid holding a
// circular icon badge on the left, the title, and an optional subtitle below
// it. `.quick-toggle-menu .header` in the stylesheet supplies
// `padding-bottom: 12px`, the badge's `border-radius: 999px` and its
// `.active` accent fill, so only the structure is needed here.
//
//   .header
//     .header .icon[.active]   24px glyph in a round badge
//     .header .title            %title_3
//     .header .subtitle         %caption_heading
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

  // Icon and text sit on one row; the subtitle stacks under the title, so the
  // text column is itself a vertical box.
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
  gtk_label_set_xalign (GTK_LABEL (title_label), 0.0);
  gtk_box_append (GTK_BOX (text_col), title_label);

  if (subtitle != NULL && subtitle[0] != '\0') {
    subtitle_label = gtk_label_new (subtitle);
    gtk_widget_add_css_class (subtitle_label, "subtitle");
    gtk_label_set_xalign (GTK_LABEL (subtitle_label), 0.0);
    gtk_box_append (GTK_BOX (text_col), subtitle_label);
  }

  gtk_box_append (GTK_BOX (row), text_col);
  gtk_box_append (GTK_BOX (header), row);

  return header;
}

// Build the popup content for a secondary menu.
//
// Structure mirrors QuickToggleMenu (quickSettings.js:363): the actor is a
// `.quick-toggle-menu-container` wrapping `this.box`, which is the popup menu
// box itself — `.quick-toggle-menu` is the *style class of the box*, and it
// carries the card background (`@extend %card`, `border-radius:
// $base_border_radius * 3`). Nesting a `.popup-menu-content` inside it paints
// a second rounded background and reads as a washed-out double border.
static GtkWidget *
build_quick_menu_full (const WinomeMenuEntry *entries, guint n_entries,
                       const char *header_icon, const char *header_title,
                       const char *header_subtitle, gboolean header_active)
{
  GtkWidget *menu;
  GtkWidget *header;
  guint i;

  // Structure mirrors QuickToggleMenu (quickSettings.js:363). PopupMenuBase
  // builds `this.box` as a vertical St.BoxLayout with x_expand: true and sets its
  // style class to the one passed to super() — 'quick-toggle-menu' here
  // (popupMenu.js:640-647). So the box carries exactly one class and expands
  // rather than hugging its content.
  //
  // Note there is no `.popup-menu-box` rule in the stylesheet — a class by that
  // name would style nothing.
  menu = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  gtk_widget_add_css_class (menu, "quick-toggle-menu");
  gtk_widget_set_hexpand (menu, TRUE);

  header = build_quick_menu_header (header_icon, header_title,
                                    header_subtitle, header_active);
  if (header != NULL)
    gtk_box_append (GTK_BOX (menu), header);

  for (i = 0; i < n_entries; i++) {
    GtkWidget *item = make_quick_menu_item (&entries[i]);
    gtk_box_append (GTK_BOX (menu), item);
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

// Called with the freshly built menu popup and the arrow widget to anchor it
// under. The popup is unref'd by the caller once the handler returns.
typedef void (*QuickMenuOpenFunc) (GtkWidget *arrow, GtkWidget *menu,
                                   gpointer user_data);
// Builds the popup for a toggle's arrow. Returns a new widget (caller frees) or
// NULL for a toggle with no menu.
typedef GtkWidget *(*QuickMenuBuilder) (gpointer user_data);

typedef struct {
  GtkWidget *toggle_part;      // the .quick-toggle node
  GtkWidget *menu_button_part; // the .quick-toggle-menu-button node
  GtkWidget *icon;             // icon image inside toggle_part
  GtkWidget *subtitle_label;   // optional subtitle label, may be NULL
  QuickMenuBuilder    menu_builder;      // builds the popup, may be NULL
  gpointer            menu_builder_data;
  QuickMenuOpenFunc   menu_open_func;    // anchors the popup, may be NULL
  gpointer            menu_open_data;
} MenuToggleParts;

// --- Menu toggle content layout ----------------------------------------------
//
// The QuickMenuToggle's inner box. GtkBox over-allocates the menu button by
// 2px vertically (a GtkBoxLayout allocation quirk), leaving a 1px step at the
// join; this custom widget lays the three parts out at exactly the box height.

typedef struct {
  GtkWidget parent_instance;
  GtkWidget *toggle_part;
  GtkWidget *separator;
  GtkWidget *menu_button;
} WinomeMenuToggleContent;

typedef struct {
  GtkWidgetClass parent_class;
} WinomeMenuToggleContentClass;

#define WINOME_TYPE_MENU_TOGGLE_CONTENT (winome_menu_toggle_content_get_type ())
#define WINOME_MENU_TOGGLE_CONTENT(obj) (G_TYPE_CHECK_INSTANCE_CAST ((obj), \
    WINOME_TYPE_MENU_TOGGLE_CONTENT, WinomeMenuToggleContent))

G_DEFINE_TYPE (WinomeMenuToggleContent, winome_menu_toggle_content, GTK_TYPE_WIDGET)

static void
winome_menu_toggle_content_measure (GtkWidget *widget,
                                    GtkOrientation orientation,
                                    int for_size G_GNUC_UNUSED,
                                    int *minimum, int *natural,
                                    int *minimum_baseline, int *natural_baseline)
{
  WinomeMenuToggleContent *self = WINOME_MENU_TOGGLE_CONTENT (widget);

  if (minimum_baseline)
    *minimum_baseline = -1;
  if (natural_baseline)
    *natural_baseline = -1;

  if (orientation == GTK_ORIENTATION_HORIZONTAL) {
    int min = 0, nat = 0;
    int m, n;
    gtk_widget_measure (self->toggle_part, GTK_ORIENTATION_HORIZONTAL, -1,
                        &m, &n, NULL, NULL);
    min += m; nat += n;
    gtk_widget_measure (self->separator, GTK_ORIENTATION_HORIZONTAL, -1,
                        &m, &n, NULL, NULL);
    min += m; nat += n;
    gtk_widget_measure (self->menu_button, GTK_ORIENTATION_HORIZONTAL, -1,
                        &m, &n, NULL, NULL);
    min += m; nat += n;
    *minimum = min;
    *natural = nat;
  } else {
    int m1, n1, m2, n2, m3, n3;
    gtk_widget_measure (self->toggle_part, GTK_ORIENTATION_VERTICAL, -1,
                        &m1, &n1, NULL, NULL);
    gtk_widget_measure (self->separator, GTK_ORIENTATION_VERTICAL, -1,
                        &m2, &n2, NULL, NULL);
    gtk_widget_measure (self->menu_button, GTK_ORIENTATION_VERTICAL, -1,
                        &m3, &n3, NULL, NULL);
    *minimum = MAX (MAX (m1, m2), m3);
    *natural = MAX (MAX (n1, n2), n3);
  }
}

static void
winome_menu_toggle_content_size_allocate (GtkWidget *widget,
                                          int width, int height,
                                          int baseline G_GNUC_UNUSED)
{
  WinomeMenuToggleContent *self = WINOME_MENU_TOGGLE_CONTENT (widget);
  int sep_w, menu_w;

  gtk_widget_measure (self->separator, GTK_ORIENTATION_HORIZONTAL, -1,
                      NULL, &sep_w, NULL, NULL);
  gtk_widget_measure (self->menu_button, GTK_ORIENTATION_HORIZONTAL, -1,
                      NULL, &menu_w, NULL, NULL);

  GtkAllocation alloc;
  alloc.y = 0;
  alloc.height = height;

  alloc.x = 0;
  alloc.width = MAX (0, width - sep_w - menu_w);
  gtk_widget_size_allocate (self->toggle_part, &alloc, -1);

  alloc.x = alloc.width;
  alloc.width = sep_w;
  gtk_widget_size_allocate (self->separator, &alloc, -1);

  alloc.x += sep_w;
  alloc.width = menu_w;
  gtk_widget_size_allocate (self->menu_button, &alloc, -1);
}

static void
winome_menu_toggle_content_init (WinomeMenuToggleContent *self G_GNUC_UNUSED)
{
}

static void
winome_menu_toggle_content_class_init (WinomeMenuToggleContentClass *klass)
{
  GtkWidgetClass *widget_class = GTK_WIDGET_CLASS (klass);

  widget_class->measure = winome_menu_toggle_content_measure;
  widget_class->size_allocate = winome_menu_toggle_content_size_allocate;
  gtk_widget_class_set_css_name (widget_class, "box");
}

static GtkWidget *
menu_toggle_content_new (GtkWidget *toggle_part, GtkWidget *separator,
                         GtkWidget *menu_button)
{
  WinomeMenuToggleContent *self =
      static_cast<WinomeMenuToggleContent *> (g_object_new (
          winome_menu_toggle_content_get_type (), NULL));

  self->toggle_part = toggle_part;
  self->separator = separator;
  self->menu_button = menu_button;

  gtk_widget_set_parent (toggle_part, GTK_WIDGET (self));
  gtk_widget_set_parent (separator, GTK_WIDGET (self));
  gtk_widget_set_parent (menu_button, GTK_WIDGET (self));

  return GTK_WIDGET (self);
}

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

// Emitted when the arrow area is clicked. The menu_button is a GtkBox inside the
// outer GtkToggleButton; the panel anchors the menu there.
//
// The click controller is on the arrow and runs before the outer toggle sees
// the event, so the toggle does not flip — matching GNOME, where `clicked` is
// bound to the inner St.Button and never reaches the QuickMenuToggle.
static void
on_menu_button_pressed (GtkGestureClick *gesture, int n_press,
                        double x, double y, gpointer user_data)
{
  MenuToggleParts *parts = static_cast<MenuToggleParts *>(user_data);
  QuickMenuBuilder builder =
      static_cast<QuickMenuBuilder> (parts->menu_builder);

  (void)n_press;
  (void)x;
  (void)y;

  // Consume the press so the enclosing toggle never sees it.
  gtk_gesture_set_state (GTK_GESTURE (gesture), GTK_EVENT_SEQUENCE_CLAIMED);

  if (winome_shell_panel_close_menu_for (parts->menu_button_part))
    return;

  if (builder == NULL)
    return;

  GtkWidget *menu = builder (parts->menu_builder_data);
  if (menu == NULL)
    return;

  // The panel takes ownership and anchors the popup under the arrow.
  QuickMenuOpenFunc open_func =
      static_cast<QuickMenuOpenFunc> (parts->menu_open_func);
  // The panel takes ownership of the popup (it parents it into the menu
  // window), so drop our reference here.
  if (open_func != NULL)
    open_func (parts->menu_button_part, menu, parts->menu_open_data);
  else
    g_object_unref (menu);
}

// The panel owns menu windows: it reuses the popover chrome, screen
// positioning, outside-click dismissal and topmost reassert, so a menu behaves
// like the quick settings and calendar popovers.
static void
default_quick_menu_open (GtkWidget *arrow, GtkWidget *menu, gpointer user_data)
{
  (void)user_data;

  winome_shell_panel_open_menu (arrow, menu);
}

static GtkWidget *
make_quick_menu_toggle (const char *icon_name, const char *title,
                        const char *subtitle,
                        QuickMenuBuilder menu_builder,
                        gpointer menu_builder_data)
{
  GtkWidget *button;
  GtkWidget *content;
  GtkWidget *toggle_part;
  GtkWidget *content_box;
  GtkWidget *icon;
  GtkWidget *title_box;
  GtkWidget *title_label;
  GtkWidget *subtitle_label = NULL;
  GtkWidget *separator;
  GtkWidget *menu_button;
  GtkWidget *arrow;
  MenuToggleParts *parts;

  button = gtk_toggle_button_new ();
  gtk_widget_add_css_class (button, "quick-toggle-has-menu");
  gtk_widget_set_focus_on_click (button, FALSE);
  // Clip the inner parts to the capsule so the menu button's small allocation
  // overflow (a GtkBox quirk) cannot poke out above/below the capsule.
  gtk_widget_set_overflow (button, GTK_OVERFLOW_HIDDEN);

  // The toggle part. A plain box (not a nested button) so clicks land on the
  // outer toggle button; it carries the .quick-toggle capsule background and
  // fills the button height like native (min-height 3.273em).
  toggle_part = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);
  gtk_widget_add_css_class (toggle_part, "quick-toggle");
  gtk_widget_add_css_class (toggle_part, "button");
  gtk_widget_set_valign (toggle_part, GTK_ALIGN_FILL);

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

  // The ">" menu button area (.icon-button fill + go-next arrow), filling the
  // capsule height like native (y_expand).
  menu_button = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);
  gtk_widget_add_css_class (menu_button, "quick-toggle-menu-button");
  gtk_widget_add_css_class (menu_button, "icon-button");
  arrow = gtk_image_new_from_icon_name ("go-next-symbolic");
  gtk_image_set_pixel_size (GTK_IMAGE (arrow), QUICK_ICON_SIZE);
  gtk_widget_set_valign (arrow, GTK_ALIGN_CENTER);
  gtk_widget_set_halign (arrow, GTK_ALIGN_CENTER);
  gtk_box_append (GTK_BOX (menu_button), arrow);

  // Custom content layout: lays the three parts out at exactly the allocated
  // height (GtkBox over-allocates the menu button by 2px).
  content = menu_toggle_content_new (toggle_part, separator, menu_button);
  gtk_widget_set_valign (content, GTK_ALIGN_FILL);
  gtk_widget_set_halign (content, GTK_ALIGN_FILL);
  gtk_button_set_child (GTK_BUTTON (button), content);

  parts = g_new0 (MenuToggleParts, 1);
  parts->toggle_part = toggle_part;
  parts->menu_button_part = menu_button;
  parts->icon = icon;
  parts->subtitle_label = subtitle_label;
  parts->menu_builder = menu_builder;
  parts->menu_builder_data = menu_builder_data;
  // Until the panel installs its anchor handler, fall back to storing the popup
  // on the arrow so the plumbing is exercised end to end.
  parts->menu_open_func = default_quick_menu_open;
  g_object_set_data_full (G_OBJECT (button), "menu-toggle-parts",
                          parts, g_free);
  g_signal_connect (button, "toggled",
                    G_CALLBACK (sync_menu_toggle_checked), parts);
  sync_menu_toggle_checked (GTK_TOGGLE_BUTTON (button), parts);

  // Secondary menu. The arrow area gets its own click controller: clicking it
  // must open the menu, not flip the outer toggle (GNOME binds `clicked` on
  // the inner St.Button, which does not propagate to the toggle).
  if (menu_builder != NULL) {
    GtkGesture *click = gtk_gesture_click_new ();

    gtk_gesture_single_set_button (GTK_GESTURE_SINGLE (click), 0);
    // CAPTURE, so the arrow's press is consumed before the enclosing
    // GtkToggleButton can act on it: clicking ">" must open the menu without
    // flipping the capsule state.
    gtk_event_controller_set_propagation_phase (GTK_EVENT_CONTROLLER (click),
                                                GTK_PHASE_CAPTURE);
    g_signal_connect (click, "pressed",
                      G_CALLBACK (on_menu_button_pressed), parts);
    gtk_widget_add_controller (menu_button, GTK_EVENT_CONTROLLER (click));
  }

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
                                          int width,
                                          int height G_GNUC_UNUSED,
                                          int baseline G_GNUC_UNUSED)
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
  self->row_spacing = qs_grid_spacing ("spacing-rows", 12);
  self->column_spacing = qs_grid_spacing ("spacing-columns", 12);
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

// Power menu (the GNOME power-off / end-session rows reached from the system
// item): Suspend, Log Out, Restart, Power Off. These act immediately — GNOME's
// end-session items use the default 'user' session mode, so no confirmation
// dialog sits between the click and the action.
typedef enum {
  POWER_ACTION_SUSPEND,
  POWER_ACTION_LOGOUT,
  POWER_ACTION_RESTART,
  POWER_ACTION_SHUTDOWN
} PowerAction;

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
  }
}

static GtkWidget *
build_power_menu (gpointer user_data)
{
  WinomeMenuEntry items[4];

  (void)user_data;

  items[0].label = tr ("Suspend", "挂起");
  items[0].icon_name = "media-playback-pause-symbolic";
  items[0].checked = FALSE;
  items[0].callback = on_power_action;
  items[0].user_data = GINT_TO_POINTER (POWER_ACTION_SUSPEND);

  items[1].label = tr ("Log Out", "注销");
  items[1].icon_name = "system-log-out-symbolic";
  items[1].checked = FALSE;
  items[1].callback = on_power_action;
  items[1].user_data = GINT_TO_POINTER (POWER_ACTION_LOGOUT);

  items[2].label = tr ("Restart", "重启");
  items[2].icon_name = "system-reboot-symbolic";
  items[2].checked = FALSE;
  items[2].callback = on_power_action;
  items[2].user_data = GINT_TO_POINTER (POWER_ACTION_RESTART);

  items[3].label = tr ("Power Off", "关机");
  items[3].icon_name = "system-shutdown-symbolic";
  items[3].checked = FALSE;
  items[3].callback = on_power_action;
  items[3].user_data = GINT_TO_POINTER (POWER_ACTION_SHUTDOWN);

  // Header mirrors upstream's setHeader() calls: GNOME labels this menu
  // "Power Off Menu" with the shutdown glyph in the badge.
  return build_quick_menu_full (items, G_N_ELEMENTS (items),
                               "system-shutdown-symbolic",
                               tr ("Power Off Menu", "关机菜单"), NULL, FALSE);
}

// Open handler for the battery capsule: same default anchor as the arrow.
static void
on_power_item_clicked (GtkButton *button, gpointer user_data)
{
  (void)user_data;

  if (winome_shell_panel_close_menu_for (GTK_WIDGET (button)))
    return;

  GtkWidget *menu = build_power_menu (NULL);
  if (menu == NULL)
    return;
  default_quick_menu_open (GTK_WIDGET (button), menu, NULL);
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
  if (winome_power_set_mode (t->mode))
    winome_quick_settings_refresh (t->panel);
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
    items[i].icon_name = net->secured ? "network-wireless-secured-symbolic"
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
    // Clicking the battery item opens the power menu (Suspend / Log Out /
    // Restart / Power Off). GNOME keeps the `.power-item` capsule look, so we
    // attach a plain click controller instead of using the arrow layout.
    g_signal_connect (power, "clicked",
                      G_CALLBACK (on_power_item_clicked), NULL);
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

    btn = make_icon_button ("applications-system-symbolic");
    g_signal_connect (btn, "clicked", G_CALLBACK (on_settings_clicked), NULL);
    gtk_box_append (GTK_BOX (row), btn);

    btn = make_icon_button ("system-lock-screen-symbolic");
    g_signal_connect (btn, "clicked", G_CALLBACK (on_lock_clicked), NULL);
    gtk_box_append (GTK_BOX (row), btn);

    btn = make_icon_button ("system-shutdown-symbolic");
    g_signal_connect (btn, "clicked", G_CALLBACK (on_power_item_clicked), NULL);
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
    g_signal_connect (wlan, "toggled",
                      G_CALLBACK (on_wlan_toggled), root);
    winome_quick_settings_grid_attach (grid, wlan, 1);

    bluetooth = make_quick_menu_toggle ("bluetooth-active-symbolic",
                                        tr ("Bluetooth", "蓝牙"),
                                        tr ("Off", "已关闭"),
                                        build_bluetooth_menu, root);
    g_signal_connect (bluetooth, "toggled",
                      G_CALLBACK (on_bluetooth_toggled), root);
    winome_quick_settings_grid_attach (grid, bluetooth, 1);

    powermode = make_quick_menu_toggle ("power-profile-balanced-symbolic",
                                        tr ("Power Mode", "电源模式"),
                                        winome_power_mode_label (active_power),
                                        build_power_mode_menu, root);
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
    g_signal_handlers_block_by_func (wlan_toggle,
        (gpointer) on_wlan_toggled, quick_settings);
    gtk_toggle_button_set_active (GTK_TOGGLE_BUTTON (wlan_toggle),
                                  have_wifi && wifi_st.enabled);
    g_signal_handlers_unblock_by_func (wlan_toggle,
        (gpointer) on_wlan_toggled, quick_settings);

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
    g_signal_handlers_block_by_func (bluetooth_toggle,
        (gpointer) on_bluetooth_toggled, quick_settings);
    gtk_toggle_button_set_active (GTK_TOGGLE_BUTTON (bluetooth_toggle),
                                  bt_enabled ? TRUE : FALSE);
    g_signal_handlers_unblock_by_func (bluetooth_toggle,
        (gpointer) on_bluetooth_toggled, quick_settings);

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
    if (parts) {
      WinomePowerMode detected = winome_power_get_active ();
      WinomePowerMode active = (detected == WINOME_POWER_MODE_COUNT)
                                   ? WINOME_POWER_BALANCED
                                   : detected;
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
