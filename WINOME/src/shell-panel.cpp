// SPDX-License-Identifier: GPL-2.0-only
// Copyright (C) 2026 WINOME contributors
// GNOME-style top panel, faithful port of gnome-shell's js/ui/panel.js.
//
// Widget tree (panel.js):
//   #panel (window)
//     WinomePanelLayout        -> custom allocate (sideWidth clamp + centered clock)
//       #panelLeft   (box)     -> ActivitiesButton (.panel-button#panelActivities)
//       #panelCenter (box)     -> DateMenuButton (.panel-button.clock-display)
//       #panelRight  (box)     -> QuickSettings  (.panel-button)
//
// The layout replicates panel.js vfunc_allocate() exactly: the clock stays
// centered in the monitor work area, and each side box is clamped to its
// natural width (no stretching). GtkCenterBox cannot do that (it centers the
// middle widget in the leftover space between the sides, shifting it off the
// true center), so a dedicated widget is used.

#include "shell-panel.h"
#include "st-engine.h"
#include "quick-settings.h"
#include "system-status.h"
#include "overview-trigger.h"

#include <time.h>
#include <string.h>
#include <math.h>

#include <windows.h>

// --- GNOME theme constants, resolved through the St engine -----------------

static winome::StEngine *
panel_engine (void)
{
  return winome::StEngine::get ();
}

// #panel { height: 2.2em } -> 32px ("Sans 11" at 96dpi).
static int
panel_height (void)
{
  winome::StEngine *e = panel_engine ();
  if (e == nullptr)
    return 32;

  winome::StEngine::Node panel (*e, nullptr, "panel", "", "");
  double v = 0.0;
  if (panel.lookup_length ("height", &v))
    return (int)(v + 0.5);
  return 32;
}

// The GNOME status icon size (16px), resolved from the selector
// #panel .panel-button .system-status-icon { icon-size: 1.091em; }.
static int
status_icon_size (void)
{
  winome::StEngine *e = panel_engine ();
  if (e == nullptr)
    return 16;

  winome::StEngine::Node panel (*e, nullptr, "panel", "", "");
  winome::StEngine::Node button (*e, &panel, "", "panel-button", "");
  winome::StEngine::Node icon (*e, &button, "", "system-status-icon", "");

  double v = 0.0;
  if (icon.lookup_length ("icon-size", &v))
    return (int)(v + 0.5);
  return 16;
}

// Spacing between status indicators, from
// #panel .panel-button .panel-status-indicators-box { spacing: 4px; }.
static int
status_indicators_spacing (void)
{
  winome::StEngine *e = panel_engine ();
  if (e == nullptr)
    return 4;

  winome::StEngine::Node panel (*e, nullptr, "panel", "", "");
  winome::StEngine::Node button (*e, &panel, "", "panel-button", "");
  winome::StEngine::Node box (*e, &button, "",
                              "panel-status-indicators-box", "");

  double v = 0.0;
  if (box.lookup_length ("spacing", &v))
    return (int)(v + 0.5);
  return 4;
}

// The workspace dot base size, from
// #panel .panel-button#panelActivities .workspace-dot { min-width: 0.5455em; }
// -> 8px.
static int
workspace_dot_size (void)
{
  winome::StEngine *e = panel_engine ();
  if (e == nullptr)
    return 8;

  winome::StEngine::Node panel (*e, nullptr, "panel", "", "");
  winome::StEngine::Node button (*e, &panel, "panelActivities", "panel-button",
                                 "");
  winome::StEngine::Node dot (*e, &button, "", "workspace-dot", "");

  double v = 0.0;
  if (dot.lookup_length ("min-width", &v))
    return (int)(v + 0.5);
  return 8;
}

// --- Workspace dot ----------------------------------------------------------
//
// In gnome-shell a WorkspaceDot is a Clutter.Actor (no CSS node) wrapping a
// .workspace-dot St.Widget. The active workspace stretches the dot into a wide
// capsule (width = dot size * width-multiplier, 3.625 for <= 2 workspaces).
// GtkBox is deliberately not used here: the mapped selector
// "#panel .panel-button#panelActivities box" would otherwise apply its padding
// to the dot too. A plain GtkWidget subclass paints its CSS background (like
// St.Widget) and does not match element selector "box".

typedef struct {
  GtkWidget parent_instance;
} WinomeDot;

typedef struct {
  GtkWidgetClass parent_class;
} WinomeDotClass;

#define WINOME_DOT(obj) (G_TYPE_CHECK_INSTANCE_CAST ((obj), \
                          WINOME_TYPE_DOT, WinomeDot))

G_DEFINE_TYPE (WinomeDot, winome_dot, GTK_TYPE_WIDGET)

static void
winome_dot_init (WinomeDot *self G_GNUC_UNUSED)
{
}

static void
winome_dot_class_init (WinomeDotClass *klass G_GNUC_UNUSED)
{
}

static GtkWidget *
create_workspace_dot (void)
{
  GtkWidget *dot = GTK_WIDGET (g_object_new (winome_dot_get_type (), NULL));
  gtk_widget_add_css_class (dot, "workspace-dot");

  int size = workspace_dot_size ();
  // width-multiplier for a single workspace (nIndicators <= 2 -> 3.625).
  int width = (int)(size * 3.625 + 0.5);
  gtk_widget_set_size_request (dot, width, size);

  return dot;
}

// --- Activities button ------------------------------------------------------

static void
on_activities_clicked (GtkButton *button G_GNUC_UNUSED,
                       gpointer user_data G_GNUC_UNUSED)
{
  winome::toggle_overview ();
}

static GtkWidget *
create_activities_button (void)
{
  GtkWidget *button;
  GtkWidget *indicators;
  GtkWidget *dot;

  button = gtk_button_new ();
  gtk_widget_add_css_class (button, "panel-button");
  gtk_widget_set_name (button, "panelActivities");
  gtk_widget_set_focus_on_click (button, FALSE);
  gtk_widget_set_vexpand (button, TRUE);

  // WorkspaceIndicators: a plain St.BoxLayout; the stylesheet's
  // "#panel .panel-button#panelActivities box" rule adds the em padding and
  // the spacing is set here (the St `spacing` property is dropped in the
  // GTK4 conversion). Like native, the box keeps its natural (8px) height and
  // is centered in the full-height button, so the dot is not stretched.
  indicators = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 5);
  gtk_widget_set_valign (indicators, GTK_ALIGN_CENTER);
  dot = create_workspace_dot ();
  gtk_box_append (GTK_BOX (indicators), dot);

  gtk_button_set_child (GTK_BUTTON (button), indicators);

  g_signal_connect (button, "clicked", G_CALLBACK (on_activities_clicked),
                    nullptr);

  return button;
}

// --- Panel layout -----------------------------------------------------------
//
// Port of Panel.vfunc_allocate()/vfunc_get_preferred_width() from panel.js.
// Three children (start/center/end boxes) are laid out with the center box
// placed exactly at ceil(sideWidth) and the sides clamped to their natural
// widths.

typedef struct {
  GtkWidget parent_instance;
  GtkWidget *start;
  GtkWidget *center;
  GtkWidget *end;
} WinomePanelLayout;

typedef struct {
  GtkWidgetClass parent_class;
} WinomePanelLayoutClass;

#define WINOME_PANEL_LAYOUT(obj) (G_TYPE_CHECK_INSTANCE_CAST ((obj), \
                                   WINOME_TYPE_PANEL_LAYOUT, WinomePanelLayout))

G_DEFINE_TYPE (WinomePanelLayout, winome_panel_layout, GTK_TYPE_WIDGET)

static void
winome_panel_layout_measure (GtkWidget *widget,
                             GtkOrientation orientation,
                             int for_size,
                             int *minimum, int *natural,
                             int *minimum_baseline, int *natural_baseline)
{
  WinomePanelLayout *self = WINOME_PANEL_LAYOUT (widget);
  GtkWidget *children[] = {self->start, self->center, self->end};

  int min_sum = 0, nat_sum = 0;
  int min_max = 0, nat_max = 0;

  for (int i = 0; i < 3; i++) {
    if (children[i] == nullptr)
      continue;
    int cmin = 0, cnat = 0;
    gtk_widget_measure (children[i], orientation, for_size,
                        &cmin, &cnat, nullptr, nullptr);
    if (orientation == GTK_ORIENTATION_HORIZONTAL) {
      min_sum += cmin;
      nat_sum += cnat;
    } else {
      min_max = MAX (min_max, cmin);
      nat_max = MAX (nat_max, cnat);
    }
  }

  if (orientation == GTK_ORIENTATION_HORIZONTAL) {
    *minimum = min_sum;
    *natural = nat_sum;
  } else {
    *minimum = min_max;
    *natural = nat_max;
  }

  if (minimum_baseline)
    *minimum_baseline = -1;
  if (natural_baseline)
    *natural_baseline = -1;
}

static void
winome_panel_layout_size_allocate (GtkWidget *widget,
                                   int width, int height, int baseline)
{
  WinomePanelLayout *self = WINOME_PANEL_LAYOUT (widget);
  GtkWidget *left = self->start;
  GtkWidget *center = self->center;
  GtkWidget *right = self->end;

  int left_nat = 0, center_nat = 0, right_nat = 0;
  if (left)
    gtk_widget_measure (left, GTK_ORIENTATION_HORIZONTAL, -1,
                        nullptr, &left_nat, nullptr, nullptr);
  if (center)
    gtk_widget_measure (center, GTK_ORIENTATION_HORIZONTAL, -1,
                        nullptr, &center_nat, nullptr, nullptr);
  if (right)
    gtk_widget_measure (right, GTK_ORIENTATION_HORIZONTAL, -1,
                        nullptr, &right_nat, nullptr, nullptr);

  // The panel spans the full primary monitor work area, so the center offset
  // from panel.js (work-area relative to the monitor) is always 0.
  int side = MAX (0, (width - center_nat) / 2);
  gboolean rtl = gtk_widget_get_direction (widget) == GTK_TEXT_DIR_RTL;

  GtkAllocation alloc;
  alloc.y = 0;
  alloc.height = height;

  if (left) {
    alloc.width = MIN (side, left_nat);
    alloc.x = rtl ? MAX (width - alloc.width, 0) : 0;
    gtk_widget_size_allocate (left, &alloc, baseline);
  }

  if (center) {
    alloc.width = center_nat;
    alloc.x = rtl ? (width - (int)ceil ((double)side) - center_nat)
                  : (int)ceil ((double)side);
    gtk_widget_size_allocate (center, &alloc, baseline);
  }

  if (right) {
    alloc.width = MIN (side, right_nat);
    alloc.x = rtl ? 0 : MAX (width - alloc.width, 0);
    gtk_widget_size_allocate (right, &alloc, baseline);
  }
}

static void
winome_panel_layout_init (WinomePanelLayout *self G_GNUC_UNUSED)
{
}

static void
winome_panel_layout_class_init (WinomePanelLayoutClass *klass)
{
  GtkWidgetClass *widget_class = GTK_WIDGET_CLASS (klass);

  widget_class->measure = winome_panel_layout_measure;
  widget_class->size_allocate = winome_panel_layout_size_allocate;
}

static void
winome_panel_layout_set (GtkWidget *layout,
                         GtkWidget *start, GtkWidget *center, GtkWidget *end)
{
  WinomePanelLayout *self = WINOME_PANEL_LAYOUT (layout);
  self->start = start;
  self->center = center;
  self->end = end;

  gtk_widget_set_parent (start, layout);
  gtk_widget_set_parent (center, layout);
  gtk_widget_set_parent (end, layout);
}

// --- Clock ------------------------------------------------------------------

static gboolean
is_24h_clock (void)
{
  DWORD iTime = 0;
  DWORD size = sizeof (iTime);
  HKEY key = NULL;

  if (RegOpenKeyExW (HKEY_CURRENT_USER,
                     L"Control Panel\\International", 0, KEY_READ, &key)
      == ERROR_SUCCESS) {
    RegQueryValueExW (key, L"iTime", NULL, NULL,
                      reinterpret_cast<BYTE *> (&iTime), &size);
    RegCloseKey (key);
  }

  return iTime == 1;
}

static gboolean
update_clock (gpointer data)
{
  GtkLabel *clock = GTK_LABEL (data);
  GDateTime *now;
  gchar *text;

  now = g_date_time_new_now_local ();

  // Matches gnome-shell formatTime(): %H:%M (24h) or %l:%M %p (12h).
  // GDateTime.format() produces UTF-8 (unlike strftime, whose %p emits the
  // locale's GBK "上午/下午" on Chinese Windows and trips Pango).
  if (is_24h_clock ())
    text = g_date_time_format (now, "%H:%M");
  else
    text = g_date_time_format (now, "%l:%M %p");

  gtk_label_set_text (clock, text);

  g_free (text);
  g_date_time_unref (now);

  return G_SOURCE_CONTINUE;
}

static void
on_click_popover (GtkButton *button G_GNUC_UNUSED, gpointer popover)
{
  // Toggle, like PanelMenu.Button._clickGesture in gnome-shell.
  if (gtk_widget_get_visible (GTK_WIDGET (popover)))
    gtk_popover_popdown (GTK_POPOVER (popover));
  else
    gtk_popover_popup (GTK_POPOVER (popover));
}

// GNOME keeps a panel button highlighted (:active) while its menu is open;
// GTK4 clears :active when the mouse is released. The stylesheet defines an
// "open" class with the same fill; toggle it with the popover.
static void
on_popover_show (GtkWidget *popover G_GNUC_UNUSED, gpointer button)
{
  gtk_widget_add_css_class (GTK_WIDGET (button), "open");
}

// GTK4's popover grab window on Windows leaves the anchor button's :hover
// (PRELIGHT) state stale: after the popover closes, the button keeps its
// highlight even when the mouse leaves, because the backend's leave tracking
// is not re-armed. Track the real pointer ourselves for a short while after
// the popover closes and clear the stale hover when it leaves the button.
static gboolean
clear_stale_hover (gpointer data)
{
  GtkWidget *b = GTK_WIDGET (data);
  GtkNative *native = gtk_widget_get_native (b);
  if (native == nullptr)
    return G_SOURCE_REMOVE;
  GdkSurface *surface = gtk_native_get_surface (native);
  if (surface == nullptr)
    return G_SOURCE_REMOVE;

  GdkDisplay *display = gdk_display_get_default ();
  GdkSeat *seat = gdk_display_get_default_seat (display);
  GdkDevice *device = gdk_seat_get_pointer (seat);
  if (device == nullptr)
    return G_SOURCE_REMOVE;

  double px = 0, py = 0;
  GdkSurface *under = gdk_device_get_surface_at_position (device, &px, &py);

  GtkWidget *root = GTK_WIDGET (gtk_widget_get_root (b));
  if (root == nullptr)
    return G_SOURCE_REMOVE;

  gboolean inside = FALSE;
  if (under == surface) {
    graphene_rect_t bounds;
    if (gtk_widget_compute_bounds (b, root, &bounds)) {
      inside = px >= bounds.origin.x &&
               px <= bounds.origin.x + bounds.size.width &&
               py >= bounds.origin.y &&
               py <= bounds.origin.y + bounds.size.height;
    }
  }

  if (!inside) {
    gtk_widget_unset_state_flags (b, GTK_STATE_FLAG_PRELIGHT);
    return G_SOURCE_REMOVE;
  }
  return G_SOURCE_CONTINUE;
}

static void
on_popover_hide (GtkWidget *popover G_GNUC_UNUSED, gpointer button)
{
  GtkWidget *b = GTK_WIDGET (button);
  gtk_widget_remove_css_class (b, "open");

  // GTK4's popover grab on Windows leaves the anchor button's :hover
  // (PRELIGHT) state stale after the popover closes. Clear it immediately and
  // keep monitoring the real pointer until it leaves the button.
  gtk_widget_unset_state_flags (b, GTK_STATE_FLAG_PRELIGHT);
  GtkWidget *p = gtk_widget_get_parent (b);
  while (p) {
    gtk_widget_unset_state_flags (p, GTK_STATE_FLAG_PRELIGHT);
    p = gtk_widget_get_parent (p);
  }

  g_timeout_add (200, clear_stale_hover, b);
}

static void
connect_popover_open_state (GtkWidget *popover, GtkWidget *button)
{
  g_signal_connect (popover, "show", G_CALLBACK (on_popover_show), button);
  g_signal_connect (popover, "hide", G_CALLBACK (on_popover_hide), button);
}

// DateMenuButton: .panel-button.clock-display > .clock-display-box > .clock.
// In GNOME the hover/active highlight lives on the .clock child (an St.Widget).
// GtkLabel does not paint its CSS background in GTK4, so the .clock "label" is
// a GtkBox (which does paint it) wrapping the actual text label.
static GtkWidget *
create_clock_button (void)
{
  GtkWidget *button;
  GtkWidget *display_box;
  GtkWidget *clock_box;
  GtkWidget *clock_label;
  GtkWidget *popover;
  GtkWidget *calendar;

  button = gtk_button_new ();
  gtk_widget_add_css_class (button, "panel-button");
  gtk_widget_add_css_class (button, "clock-display");
  gtk_widget_set_focus_on_click (button, FALSE);
  gtk_widget_set_vexpand (button, TRUE);

  display_box = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 2);
  gtk_widget_add_css_class (display_box, "clock-display-box");
  // Keep the clock at its natural size, centered in the full-height button,
  // so the hover highlight is a capsule around the text (like native, where
  // the .clock label is y-aligned CENTER).
  gtk_widget_set_valign (display_box, GTK_ALIGN_CENTER);

  clock_box = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);
  gtk_widget_add_css_class (clock_box, "clock");
  gtk_widget_set_valign (clock_box, GTK_ALIGN_CENTER);

  clock_label = gtk_label_new ("00:00");
  gtk_box_append (GTK_BOX (clock_box), clock_label);
  gtk_box_append (GTK_BOX (display_box), clock_box);

  gtk_button_set_child (GTK_BUTTON (button), display_box);

  g_timeout_add_seconds (1, update_clock, clock_label);
  update_clock (clock_label);

  popover = gtk_popover_new ();
  calendar = winome_calendar_new ();
  gtk_popover_set_child (GTK_POPOVER (popover), calendar);
  gtk_widget_add_css_class (popover, "quick-settings-popover");
  gtk_popover_set_has_arrow (GTK_POPOVER (popover), FALSE);
  gtk_popover_set_position (GTK_POPOVER (popover), GTK_POS_BOTTOM);
  gtk_popover_set_offset (GTK_POPOVER (popover), -8, -8);
  gtk_widget_set_parent (popover, button);

  g_object_set_data (G_OBJECT (button), "popover", popover);
  g_signal_connect (button, "clicked", G_CALLBACK (on_click_popover),
                    popover);
  connect_popover_open_state (popover, button);

  return button;
}

// --- Quick settings ---------------------------------------------------------

static GtkWidget *
create_status_icon (const char *icon_name)
{
  GtkWidget *image;

  image = gtk_image_new_from_icon_name (icon_name);
  gtk_image_set_pixel_size (GTK_IMAGE (image), status_icon_size ());
  gtk_widget_add_css_class (image, "system-status-icon");

  return image;
}

// A SystemIndicator: a .panel-status-indicators-box holding one
// .system-status-icon (see SystemIndicator in quickSettings.js).
static GtkWidget *
make_indicator (GtkWidget **icon_out, const char *icon_name)
{
  GtkWidget *box;
  GtkWidget *icon;

  box = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 2);
  gtk_widget_add_css_class (box, "panel-status-indicators-box");

  icon = create_status_icon (icon_name);
  gtk_box_append (GTK_BOX (box), icon);

  if (icon_out)
    *icon_out = icon;
  return box;
}

// --- Status data ------------------------------------------------------------

typedef struct {
  GtkWidget *network_icon;
  GtkWidget *bluetooth_icon;
  GtkWidget *volume_icon;
  GtkWidget *battery_box;
  GtkWidget *battery_icon;
  GtkWidget *battery_label;
} PanelStatus;

static void
update_network_icon (GtkWidget *icon)
{
  bool connected = false;
  winome::get_network_connected (&connected);

  gtk_image_set_from_icon_name (
      GTK_IMAGE (icon),
      connected ? "network-wireless-signal-good-symbolic"
                : "network-wireless-offline-symbolic");
}

static void
update_volume_icon (GtkWidget *icon)
{
  double volume = 0.0;
  bool muted = false;
  winome::get_volume (&volume);
  winome::get_mute (&muted);

  const char *name;
  if (muted || volume <= 0.0)
    name = "audio-volume-muted-symbolic";
  else if (volume <= 1.0 / 3.0)
    name = "audio-volume-low-symbolic";
  else if (volume <= 2.0 / 3.0)
    name = "audio-volume-medium-symbolic";
  else
    name = "audio-volume-high-symbolic";

  gtk_image_set_from_icon_name (GTK_IMAGE (icon), name);
}

static void
update_battery_icon (PanelStatus *st)
{
  winome::BatteryStatus battery;
  char name[64];

  if (winome::get_battery_status (&battery)) {
    gtk_widget_set_visible (st->battery_box, TRUE);

    int fill = 10 * (battery.percentage / 10);
    if (battery.percentage >= 100)
      snprintf (name, sizeof (name), "battery-level-100-charged-symbolic");
    else
      snprintf (name, sizeof (name), "battery-level-%d%s-symbolic",
                fill, battery.charging ? "-charging" : "");

    gtk_image_set_from_icon_name (GTK_IMAGE (st->battery_icon), name);
  } else {
    // No battery/UPS: like status/system.js, show the power icon instead.
    gtk_widget_set_visible (st->battery_box, TRUE);
    gtk_image_set_from_icon_name (GTK_IMAGE (st->battery_icon),
                                  "system-shutdown-symbolic");
  }

  // Percentage label: only shown when org.gnome.desktop.interface
  // show-battery-percentage is enabled (off by default).
  gtk_widget_set_visible (st->battery_label, FALSE);
}

static gboolean
refresh_status (gpointer data)
{
  PanelStatus *st = static_cast<PanelStatus *>(data);

  update_network_icon (st->network_icon);
  update_volume_icon (st->volume_icon);
  update_battery_icon (st);

  return G_SOURCE_CONTINUE;
}

static GtkWidget *
create_quick_settings (PanelStatus *st)
{
  GtkWidget *button;
  GtkWidget *status_box;
  GtkWidget *popover;
  GtkWidget *quick_settings;

  button = gtk_button_new ();
  gtk_widget_add_css_class (button, "panel-button");
  gtk_widget_set_focus_on_click (button, FALSE);
  gtk_widget_set_vexpand (button, TRUE);

  status_box = gtk_box_new (GTK_ORIENTATION_HORIZONTAL,
                            status_indicators_spacing ());
  gtk_widget_add_css_class (status_box, "panel-status-indicators-box");
  gtk_widget_set_valign (status_box, GTK_ALIGN_CENTER);

  // Order matches QuickSettings._setupIndicators(): network, bluetooth,
  // volume, system(battery) left to right (rightmost = battery).
  gtk_box_append (GTK_BOX (status_box),
                  make_indicator (&st->network_icon,
                                  "network-wireless-signal-good-symbolic"));
  gtk_box_append (GTK_BOX (status_box),
                  make_indicator (&st->bluetooth_icon,
                                  "bluetooth-active-symbolic"));
  gtk_box_append (GTK_BOX (status_box),
                  make_indicator (&st->volume_icon,
                                  "audio-volume-high-symbolic"));

  st->battery_box = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);
  gtk_widget_add_css_class (st->battery_box, "panel-status-indicators-box");
  gtk_widget_add_css_class (st->battery_box, "power-status");
  st->battery_icon = create_status_icon ("battery-level-100-symbolic");
  gtk_box_append (GTK_BOX (st->battery_box), st->battery_icon);
  st->battery_label = gtk_label_new ("");
  gtk_widget_set_visible (st->battery_label, FALSE);
  gtk_box_append (GTK_BOX (st->battery_box), st->battery_label);
  gtk_box_append (GTK_BOX (status_box), st->battery_box);

  gtk_button_set_child (GTK_BUTTON (button), status_box);

  popover = gtk_popover_new ();
  quick_settings = winome_quick_settings_new ();
  gtk_popover_set_child (GTK_POPOVER (popover), quick_settings);
  gtk_widget_add_css_class (popover, "quick-settings-popover");
  gtk_popover_set_has_arrow (GTK_POPOVER (popover), FALSE);
  gtk_popover_set_position (GTK_POPOVER (popover), GTK_POS_BOTTOM);
  gtk_popover_set_offset (GTK_POPOVER (popover), -8, -8);
  gtk_widget_set_parent (popover, button);

  g_object_set_data (G_OBJECT (button), "popover", popover);
  g_signal_connect (button, "clicked", G_CALLBACK (on_click_popover),
                    popover);
  connect_popover_open_state (popover, button);

  return button;
}

// --- CSS --------------------------------------------------------------------

static void
load_css (GtkWidget *panel G_GNUC_UNUSED)
{
  GtkCssProvider *provider = gtk_css_provider_new ();

  gtk_css_provider_load_from_resource (provider,
                                       "/org/winome/theme/gnome-shell-gtk4.css");

  // Load at display scope so descendant selectors (#panel .panel-button,
  // #panel .panel-button.clock-display .clock, etc.) match all child widgets.
  gtk_style_context_add_provider_for_display (
    gdk_display_get_default (),
    GTK_STYLE_PROVIDER (provider),
    GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);

  g_object_unref (provider);
}

// --- Panel entry point ------------------------------------------------------

GtkWidget *
winome_shell_panel_new (void)
{
  GtkWidget *panel;
  GtkWidget *layout;
  GtkWidget *left_box;
  GtkWidget *center_box;
  GtkWidget *right_box;
  PanelStatus *status;

  panel = gtk_window_new ();
  gtk_window_set_title (GTK_WINDOW (panel), "WINOME Shell");
  gtk_window_set_decorated (GTK_WINDOW (panel), FALSE);
  gtk_window_set_default_size (GTK_WINDOW (panel), -1, panel_height ());

  gtk_widget_set_name (panel, "panel");

  layout = GTK_WIDGET (g_object_new (winome_panel_layout_get_type (), NULL));
  gtk_window_set_child (GTK_WINDOW (panel), layout);

  left_box = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);
  gtk_widget_set_name (left_box, "panelLeft");
  center_box = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);
  gtk_widget_set_name (center_box, "panelCenter");
  right_box = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);
  gtk_widget_set_name (right_box, "panelRight");

  gtk_box_append (GTK_BOX (left_box), create_activities_button ());
  gtk_box_append (GTK_BOX (center_box), create_clock_button ());

  status = g_new0 (PanelStatus, 1);
  gtk_box_append (GTK_BOX (right_box), create_quick_settings (status));

  winome_panel_layout_set (layout, left_box, center_box, right_box);

  // Refresh network / volume / battery icons periodically.
  g_timeout_add_seconds (2, refresh_status, status);

  load_css (panel);

  return panel;
}

int
winome_shell_panel_height (void)
{
  return panel_height ();
}
