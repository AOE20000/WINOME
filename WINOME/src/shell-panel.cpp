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
#include "config.h"
#include "easing.h"
#include "host-utils.h"
#include "i18n.h"
#include "st-engine.h"
#include "system-status.h"
#include "virtual-desktop.h"
#include "wifi-networks.h"
#include "quick-settings.h"
#include "notifications.h"
#include "systray.h"
#include "overview-trigger.h"
#include "overview.h"
#include "fonts.h"

#include <gdk/win32/gdkwin32.h>
#include <dwmapi.h>

#include <time.h>
#include <string.h>
#include <math.h>
#include <algorithm>
#include <memory>
#include <string>
#include <vector>

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
  gtk_widget_set_valign (dot, GTK_ALIGN_CENTER);

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
  winome::toggle_overview ("activities-button");
}

static GtkWidget *
create_activities_button (GtkWidget **out_indicators)
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

  if (out_indicators != nullptr)
    *out_indicators = indicators;

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
  const auto &cfg = winome::get_config ();
  if (cfg.clock_format == "24h")
    return TRUE;
  if (cfg.clock_format == "12h")
    return FALSE;

  wchar_t buf[8] = {};
  DWORD size = sizeof (buf);
  DWORD type = 0;
  HKEY key = NULL;

  if (RegOpenKeyExW (HKEY_CURRENT_USER,
                     L"Control Panel\\International", 0, KEY_READ, &key)
      == ERROR_SUCCESS) {
    // Windows stores iTime as REG_SZ ("0" = 12h, "1" = 24h), not DWORD.
    LSTATUS status = RegQueryValueExW (key, L"iTime", NULL, &type,
                                       reinterpret_cast<BYTE *> (buf), &size);
    RegCloseKey (key);
    if (status == ERROR_SUCCESS && type == REG_SZ)
      return buf[0] == L'1';
  }

  return FALSE;
}

static gboolean
update_clock (gpointer data)
{
  GtkLabel *clock = GTK_LABEL (data);
  GDateTime *now;
  gchar *text;

  now = g_date_time_new_now_local ();

  // Matches gnome-shell formatTime(): %H:%M (24h) or %l:%M %p (12h), with
  // optional seconds when enabled in WINOME config.
  // GDateTime.format() produces UTF-8 (unlike strftime, whose %p emits the
  // locale's GBK "上午/下午" on Chinese Windows and trips Pango).
  const auto &cfg = winome::get_config ();
  const char *fmt_24 = cfg.clock_show_seconds ? "%H:%M:%S" : "%H:%M";
  const char *fmt_12 = cfg.clock_show_seconds ? "%l:%M:%S %p" : "%l:%M %p";
  text = g_date_time_format (now, is_24h_clock () ? fmt_24 : fmt_12);

  gtk_label_set_text (clock, text);

  g_free (text);
  g_date_time_unref (now);

  return G_SOURCE_CONTINUE;
}

// --- Panel popovers ---------------------------------------------------------
//
// The quick-settings and calendar menus are custom borderless toplevels,
// positioned below the panel with a stable gap and kept clear of the screen
// edges. A low-level mouse hook closes them on clicks outside the popover
// (clicks on the anchor button itself are left to its toggle handler), and
// they are raised above the Activities overview.

#define POPOVER_TOP_GAP    8    // gap below the panel
#define POPOVER_SIDE_GAP   12   // gap from the right screen edge
#define POPOVER_SCREEN_GAP 8    // minimum gap from the other screen edges

typedef struct {
  GtkWidget *window;
  GtkWidget *button;
  GtkWidget *wrap;         // shadow-wrap around the card
  GtkWidget *arrow_area;   // BoxPointer upward triangle
  GtkWidget *card;
  GtkWidget *child;        // embedded content widget (quick settings / calendar / tray)
  gboolean open;
  gboolean centered;       // calendar: center on the button (quick settings right-aligns)
  RECT anchor_rect;        // the button's screen rect, for the close hook
  int win_x;               // last positioned left edge in physical px
  gint raise_attempts;     // post-map re-assert counter (GDK re-lates late)
  guint raise_source_id;   // pending g_idle/g_timeout id for popover_raise_idle
  uint64_t anim_gen;       // invalidates stale open/close fade callbacks
} PanelPopover;

static GList *g_open_popovers = NULL;
static HHOOK g_popover_mouse_hook = NULL;
static gboolean g_overview_hooked = FALSE;
// Monotonic time of the last popover dismissal by an outside click while the
// overview was open (see popover_mouse_hook_proc).
static gint64 g_popover_outside_close_us = 0;

static void
panel_popover_cancel_raise (PanelPopover *pp)
{
  if (pp->raise_source_id != 0) {
    g_source_remove (pp->raise_source_id);
    pp->raise_source_id = 0;
  }
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

static HWND
panel_popover_hwnd (PanelPopover *pp)
{
  GdkSurface *surface = gtk_native_get_surface (GTK_NATIVE (pp->window));
  if (surface == nullptr)
    return nullptr;
  return static_cast<HWND> (gdk_win32_surface_get_handle (surface));
}

// The anchor's screen rect. For a secondary menu the anchor is the arrow box
// inside the capsule, but the capsule is what the user considers "the button":
// widen to the whole QuickMenuToggle so clicking the toggle row does not
// dismiss a menu the arrow just opened.
static void
panel_popover_anchor_rect (PanelPopover *pp)
{
  GtkWidget *anchor = pp->button;
  GtkWidget *root = GTK_WIDGET (gtk_widget_get_root (anchor));
  if (root == nullptr)
    return;
  GdkSurface *surface = gtk_native_get_surface (GTK_NATIVE (root));
  if (surface == nullptr)
    return;
  HWND panel_hwnd = static_cast<HWND> (gdk_win32_surface_get_handle (surface));
  if (panel_hwnd == nullptr)
    return;

  RECT pr;
  GetWindowRect (panel_hwnd, &pr);

  graphene_rect_t b;
  if (gtk_widget_compute_bounds (anchor, root, &b)) {
    pp->anchor_rect.left = pr.left + (LONG) b.origin.x;
    pp->anchor_rect.top = pr.top + (LONG) b.origin.y;
    pp->anchor_rect.right = pp->anchor_rect.left + (LONG) b.size.width;
    pp->anchor_rect.bottom = pp->anchor_rect.top + (LONG) b.size.height;
  }
}

// BoxPointer metrics, from the theme (popovers.scss / _popovers.scss):
//   -boxpointer-gap 4px, -arrow-rise 10px,
//   -arrow-border-radius 22px, -arrow-border-width 1px, -arrow-base 20px
// which is how BoxPointer._reposition() derives them:
//
//   margin  = 4 * arrow_border_radius + arrow_border_width + arrow_base = 109
//   gap     = boxpointer_gap                                          = 4
//   padding = arrow_rise                                              = 10
//
//   resX = sourceCenterX - (margin/2 + (natWidth - margin) * alignment)
//   resX = clamp(resX, workarea.x + padding,
//                     workarea.x + workarea.width - (padding + natWidth))
//   resY = sourceBottomRight.y + gap
//
// alignment 0.5 (the default _arrowAlignment) centres the menu's arrow slot on
// the anchor's centre; the rest is the room the arrow + shadow needs, which is
// why right-aligning to the anchor's edge lands ~margin/2 off-screen.
#define BOXPOINTER_MARGIN  109
#define BOXPOINTER_GAP     4
#define BOXPOINTER_PADDING 10

struct BoxPointerTheme {
  double arrow_rise = 6.0;
  double arrow_base = 20.0;
  double arrow_border_radius = 22.0;
  double arrow_border_width = 1.0;
  double gap = BOXPOINTER_GAP;
  GdkRGBA panel_bg = {0x36 / 255.0f, 0x36 / 255.0f, 0x3a / 255.0f, 1.0f};
  GdkRGBA border_color = {0x42 / 255.0f, 0x42 / 255.0f, 0x47 / 255.0f, 1.0f};
  bool resolved = false;
};

static const BoxPointerTheme &
boxpointer_theme (void)
{
  static BoxPointerTheme t;
  if (t.resolved)
    return t;
  t.resolved = true;

  winome::StEngine *e = panel_engine ();
  if (e == nullptr)
    return t;

  winome::StEngine::Node bp (*e, nullptr, "", "popup-menu-boxpointer", "");
  winome::StEngine::Node sub (*e, nullptr, "", "keyboard-subkeys-boxpointer", "");
  auto lookup_bp = [&] (const char *prop, double *out) {
    double v = 0.0;
    if (bp.lookup_length (prop, &v) || sub.lookup_length (prop, &v))
      *out = v;
  };
  lookup_bp ("-arrow-rise", &t.arrow_rise);
  if (t.arrow_rise <= 0.0)
    t.arrow_rise = 6.0;
  lookup_bp ("-arrow-base", &t.arrow_base);
  lookup_bp ("-arrow-border-radius", &t.arrow_border_radius);
  lookup_bp ("-arrow-border-width", &t.arrow_border_width);
  lookup_bp ("-boxpointer-gap", &t.gap);

  std::string hex;
  GdkRGBA rgba{};
  winome::StEngine::Node content (*e, nullptr, "", "popup-menu-content", "");
  if (content.lookup_color ("background-color", &hex) &&
      gdk_rgba_parse (&rgba, hex.c_str ()))
    t.panel_bg = rgba;
  if (content.lookup_color ("border-color", &hex) &&
      gdk_rgba_parse (&rgba, hex.c_str ()))
    t.border_color = rgba;

  return t;
}

static void
draw_popover_arrow (GtkDrawingArea *area, cairo_t *cr, int width, int height,
                    gpointer data)
{
  PanelPopover *pp = static_cast<PanelPopover *> (data);
  if (pp == nullptr || !pp->open || width <= 0 || height <= 0)
    return;

  const BoxPointerTheme &t = boxpointer_theme ();
  int scale = gtk_widget_get_scale_factor (GTK_WIDGET (area));
  if (scale <= 0)
    scale = 1;

  // .quick-settings-popover-shadow-wrap adds 6px left padding around the area.
  double wrap_pad = 6.0;
  double anchor_cx =
      ((pp->anchor_rect.left + pp->anchor_rect.right) * 0.5 - pp->win_x) /
          scale -
      wrap_pad;
  double corner_r = pp->centered ? 30.0 : 36.0;
  double half_base = t.arrow_base * 0.5;
  double min_cx = corner_r + half_base;
  double max_cx = (double)width - corner_r - half_base;
  double cx = (max_cx >= min_cx) ? CLAMP (anchor_cx, min_cx, max_cx)
                                 : width * 0.5;

  double x_left = cx - half_base;
  double x_right = cx + half_base;
  double tip_y = MAX (1.0, (double)height - t.arrow_rise + 1.0);
  double base_y = (double)height;

  const GdkRGBA &fill = t.panel_bg;
  cairo_move_to (cr, x_left, base_y);
  cairo_line_to (cr, cx, tip_y);
  cairo_line_to (cr, x_right, base_y);
  cairo_close_path (cr);
  cairo_set_source_rgba (cr, fill.red, fill.green, fill.blue, fill.alpha);
  cairo_fill (cr);

  // Panel cards draw a 1px border (#424247); stroke the slanted triangle legs
  // to meet it cleanly.
  cairo_move_to (cr, x_left + 0.5, base_y - 0.5);
  cairo_line_to (cr, cx, tip_y + 0.5);
  cairo_line_to (cr, x_right - 0.5, base_y - 0.5);
  cairo_set_source_rgba (cr, t.border_color.red, t.border_color.green,
                         t.border_color.blue, t.border_color.alpha);
  cairo_set_line_width (cr, t.arrow_border_width);
  cairo_stroke (cr);
}

// Compute the popover's top-left corner (screen coordinates, physical px).
// Returns FALSE if the geometry could not be determined.
static gboolean
panel_popover_geometry (PanelPopover *pp, int width, int height,
                        int *out_x, int *out_y)
{
  GtkWidget *root = GTK_WIDGET (gtk_widget_get_root (pp->button));
  if (root == nullptr)
    return FALSE;
  GdkSurface *ps = gtk_native_get_surface (GTK_NATIVE (root));
  RECT pr = {};
  if (ps == nullptr ||
      !GetWindowRect (static_cast<HWND> (gdk_win32_surface_get_handle (ps)),
                      &pr))
    return FALSE;

  HWND hwnd = panel_popover_hwnd (pp);
  HMONITOR mon = MonitorFromWindow (hwnd, MONITOR_DEFAULTTONEAREST);
  MONITORINFO mi = {};
  mi.cbSize = sizeof (mi);
  if (!GetMonitorInfoW (mon, &mi))
    return FALSE;
  RECT wa = mi.rcMonitor;

  // Right-align for the quick settings; center on the clock for the calendar.
  int x = pp->centered
      ? (pp->anchor_rect.left + pp->anchor_rect.right) / 2 - width / 2
      : wa.right - width - POPOVER_SIDE_GAP;

  // A stable gap below the panel (the panel spans the monitor top).
  int y = pr.bottom + POPOVER_TOP_GAP;

  // Clamp inside the monitor.
  int x_min = wa.left + POPOVER_SCREEN_GAP;
  int x_max = MAX (x_min, wa.right - width - POPOVER_SCREEN_GAP);
  int y_min = pr.bottom + POPOVER_TOP_GAP;
  int y_max = MAX (y_min, wa.bottom - height - POPOVER_SCREEN_GAP);
  *out_x = CLAMP (x, x_min, x_max);
  *out_y = CLAMP (y, y_min, y_max);
  return TRUE;
}

static void
panel_popover_hide_now (PanelPopover *pp)
{
  gtk_widget_set_visible (pp->window, FALSE);
  HWND hwnd = panel_popover_hwnd (pp);
  if (hwnd != nullptr)
    SetWindowPos (hwnd, HWND_NOTOPMOST, 0, 0, 0, 0,
                  SWP_NOMOVE | SWP_NOSIZE);
}

static void
panel_popover_close_ex (PanelPopover *pp, const char *reason, gboolean immediate)
{
  if (!pp->open) {
    if (immediate) {
      ++pp->anim_gen;
      if (pp->window != nullptr && gtk_widget_get_visible (pp->window))
        panel_popover_hide_now (pp);
    }
    return;
  }

  g_print ("[pop] close (%s)\n", reason);
  pp->open = FALSE;
  const uint64_t gen = ++pp->anim_gen;
  panel_popover_cancel_raise (pp);
  gtk_widget_remove_css_class (pp->button, "open");

  if (pp->child != NULL)
    winome_quick_settings_close_menu (pp->child, FALSE);

  g_open_popovers = g_list_remove (g_open_popovers, pp);

  // Clear the stale :hover GTK leaves on the anchor button on Windows.
  gtk_widget_unset_state_flags (pp->button, GTK_STATE_FLAG_PRELIGHT);
  GtkWidget *p = gtk_widget_get_parent (pp->button);
  while (p) {
    gtk_widget_unset_state_flags (p, GTK_STATE_FLAG_PRELIGHT);
    p = gtk_widget_get_parent (p);
  }
  g_timeout_add_full (G_PRIORITY_DEFAULT, 200, clear_stale_hover,
                      g_object_ref (pp->button), g_object_unref);

  const double cur_op = pp->wrap != nullptr ? gtk_widget_get_opacity (pp->wrap) : 0.0;
  if (immediate || pp->wrap == nullptr || cur_op <= 0.01) {
    panel_popover_hide_now (pp);
    return;
  }

  winome::tween_opacity (
      pp->wrap, cur_op, 0.0, winome::BOXPOPOVER_ANIMATION_TIME,
      winome::Ease::ease_out_quad, [pp, gen] () {
        if (pp->open || pp->anim_gen != gen)
          return;
        panel_popover_hide_now (pp);
      });
}

static void
panel_popover_close (PanelPopover *pp, const char *reason)
{
  panel_popover_close_ex (pp, reason, FALSE);
}

// After GTK maps the popover, re-assert the shell-chrome ex styles, the
// topmost z-order AND the anchor-derived position. GTK4's deferred map can
// both demote the window out of the topmost band (hiding it below the
// topmost Activities overview) and reposition the window to GTK's own
// cached geometry — so the current rectangle must never be trusted, only
// the anchor geometry. Re-runs for a short while because GDK may still
// reconfigure right after the map.
static gboolean
popover_raise_idle (gpointer data)
{
  PanelPopover *pp = static_cast<PanelPopover *> (data);
  pp->raise_source_id = 0;
  if (!pp->open)
    return G_SOURCE_REMOVE;

  HWND hwnd = panel_popover_hwnd (pp);
  if (hwnd != nullptr) {
    LONG_PTR ex = GetWindowLongPtrW (hwnd, GWL_EXSTYLE);
    LONG_PTR want = (ex & ~WS_EX_APPWINDOW) | WS_EX_TOOLWINDOW | WS_EX_TOPMOST;
    if (want != ex)
      SetWindowLongPtrW (hwnd, GWL_EXSTYLE, want);

    RECT before = {0, 0, 0, 0};
    GetWindowRect (hwnd, &before);
    BOOL cloaked = FALSE;
    DwmGetWindowAttribute (hwnd, DWMWA_CLOAKED, &cloaked, sizeof (cloaked));

    if (pp->raise_attempts == 0) {
      // Recompute the position from the anchor; keep the current size (GTK
      // has negotiated it by now).
      int w = before.right - before.left;
      int h = before.bottom - before.top;
      int x = before.left, y = before.top;
      g_print ("[pop] raise#%d rect=%ld,%ld %dx%d vis=%d cloak=%d\n",
               pp->raise_attempts, before.left, before.top, w, h,
               (int)IsWindowVisible (hwnd), (int)cloaked);
      if (w > 0 && h > 0 && panel_popover_geometry (pp, w, h, &x, &y)) {
        pp->win_x = x;
        if (pp->arrow_area != nullptr)
          gtk_widget_queue_draw (pp->arrow_area);
        SetWindowPos (hwnd, HWND_TOPMOST, x, y, w, h,
                      SWP_NOACTIVATE | SWP_SHOWWINDOW | SWP_FRAMECHANGED);
      }
    } else {
      // Subsequent follow-up passes only re-assert HWND_TOPMOST without
      // touching size/frame so inline drawer animations resize smoothly.
      SetWindowPos (hwnd, HWND_TOPMOST, 0, 0, 0, 0,
                    SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    }
  }

  // Keep the full shell order (this popover > panel > overview) once right
  // after the map; fullscreen-watcher re-asserts it every 400ms thereafter, so
  // there is no need to churn SWP_FRAMECHANGED on the panel/overview every
  // 100ms for a full second (that churn made the popover flicker against the
  // overview while opening).
  if (pp->raise_attempts == 0)
    winome::overview_restack ();

  if (++pp->raise_attempts < 10) {
    pp->raise_source_id = g_timeout_add (100, popover_raise_idle, pp);
    return G_SOURCE_REMOVE;
  }
  return G_SOURCE_REMOVE;
}

// The popover must be raised AFTER GDK's map sequence completes: GTK4 defers
// the map to the main loop, and GDK's first-map positioning runs after any
// SetWindowPos issued from panel_popover_open(), which otherwise leaves the
// window below the topmost Activities overview.
static void
on_popover_map (GtkWidget *widget, gpointer data)
{
  (void)widget;
  PanelPopover *pp = static_cast<PanelPopover *> (data);
  if (!pp->open)
    return;
  panel_popover_cancel_raise (pp);
  pp->raise_source_id = g_idle_add (popover_raise_idle, pp);
}

static void
panel_popover_open (PanelPopover *pp)
{
  if (pp->open)
    return;

  // Match upstream PanelMenu: opening one panel menu closes any sibling menu.
  GList *siblings = g_list_copy (g_open_popovers);
  for (GList *l = siblings; l != nullptr; l = l->next) {
    PanelPopover *other = static_cast<PanelPopover *> (l->data);
    if (other != pp && g_list_find (g_open_popovers, other) != nullptr)
      panel_popover_close_ex (other, "switch", TRUE);
  }
  g_list_free (siblings);

  g_print ("[pop] open\n");
  pp->open = TRUE;
  ++pp->anim_gen;
  pp->raise_attempts = 0;
  gtk_widget_add_css_class (pp->button, "open");

  if (pp->child != nullptr) {
    if (pp->centered)
      winome_calendar_refresh (pp->child);
    else if (g_object_get_data (G_OBJECT (pp->child), "qs-grid") != nullptr)
      winome_quick_settings_refresh (pp->child);
  }

  panel_popover_anchor_rect (pp);

  // Measure natural size (card + shadow margin) for initial Win32 placement,
  // while leaving default_size (-1, -1) on this non-resizable window so GTK4
  // automatically grows and shrinks it when an inline QuickToggleMenu reveals.
  int nat_w, nat_h;
  gtk_widget_measure (pp->wrap, GTK_ORIENTATION_HORIZONTAL, -1,
                      nullptr, &nat_w, nullptr, nullptr);
  gtk_widget_measure (pp->wrap, GTK_ORIENTATION_VERTICAL, -1,
                      nullptr, &nat_h, nullptr, nullptr);
  gtk_window_set_default_size (GTK_WINDOW (pp->window), -1, -1);

  // Realize so the HWND exists, then place it at its final rect before
  // showing, so it never appears at GTK's default placement.
  gtk_widget_realize (pp->window);
  GdkSurface *surface = gtk_native_get_surface (GTK_NATIVE (pp->window));
  HWND hwnd = surface != nullptr
      ? static_cast<HWND> (gdk_win32_surface_get_handle (surface))
      : nullptr;
  if (hwnd != nullptr) {
    // Shell chrome: keep the popover out of Alt-Tab and the taskbar.
    LONG_PTR ex = GetWindowLongPtrW (hwnd, GWL_EXSTYLE);
    ex &= ~WS_EX_APPWINDOW;
    ex |= WS_EX_TOOLWINDOW;
    SetWindowLongPtrW (hwnd, GWL_EXSTYLE, ex);

    int scale = gdk_surface_get_scale_factor (surface);
    int px_w = nat_w * scale;
    int px_h = nat_h * scale;
    int x = 0, y = 0;
    if (panel_popover_geometry (pp, px_w, px_h, &x, &y)) {
      pp->win_x = x;
      if (pp->arrow_area != nullptr)
        gtk_widget_queue_draw (pp->arrow_area);
      // Show + raise + activate so the popover is interactive (Esc, clicks).
      // SWP_FRAMECHANGED so the ex-style changes above take effect on the
      // banding immediately.
      SetWindowPos (hwnd, HWND_TOPMOST, x, y, px_w, px_h,
                    SWP_SHOWWINDOW | SWP_FRAMECHANGED);
    }
  }

  if (pp->wrap != nullptr) {
    const double start_op =
        gtk_widget_get_visible (pp->window) ? gtk_widget_get_opacity (pp->wrap) : 0.0;
    gtk_widget_set_opacity (pp->wrap, start_op);
    gtk_widget_set_visible (pp->window, TRUE);
    winome::tween_opacity (pp->wrap, start_op, 1.0,
                           winome::BOXPOPOVER_ANIMATION_TIME,
                           winome::Ease::ease_out_quad);
  } else {
    gtk_widget_set_visible (pp->window, TRUE);
  }

  // Re-assert the topmost placement after GTK's map completes (see
  // popover_raise_idle): the first map demotes the window out of the
  // topmost band, hiding it below the overview.
  panel_popover_cancel_raise (pp);
  pp->raise_source_id = g_idle_add (popover_raise_idle, pp);

  g_open_popovers = g_list_append (g_open_popovers, pp);
}

static void
on_click_popover (GtkButton *button G_GNUC_UNUSED, gpointer data)
{
  PanelPopover *pp = static_cast<PanelPopover *> (data);

  g_print ("[pop] on_click open=%d overview=%d\n", (int)pp->open,
           (int)winome::overview_active ());

  if (pp->open) {
    panel_popover_close (pp, "toggle");
    return;
  }

  // Native GNOME: panel menus opened during the Activities overview float
  // ABOVE the overview (popover > panel > overview); the overview stays
  // visible behind them. popover_raise_idle + overview_restack() keep the
  // layering stable, so no overview close here.
  panel_popover_open (pp);
}

static gboolean
on_popover_key (GtkEventControllerKey *controller G_GNUC_UNUSED,
                guint keyval,
                guint keycode G_GNUC_UNUSED,
                GdkModifierType state G_GNUC_UNUSED,
                gpointer data)
{
  PanelPopover *pp = static_cast<PanelPopover *> (data);
  if (keyval == GDK_KEY_Escape) {
    if (pp->child != nullptr &&
        winome_quick_settings_close_menu (pp->child, TRUE))
      return GDK_EVENT_STOP;
    panel_popover_close (pp, "escape");
    return GDK_EVENT_STOP;
  }
  return GDK_EVENT_PROPAGATE;
}

static gboolean
close_outside_popovers_idle (gpointer data)
{
  POINT *pt = static_cast<POINT *>(data);
  if (pt == nullptr)
    return G_SOURCE_REMOVE;

  GList *snapshot = g_list_copy (g_open_popovers);
  for (GList *l = snapshot; l != nullptr; l = l->next) {
    PanelPopover *pp = static_cast<PanelPopover *>(l->data);
    if (g_list_find (g_open_popovers, pp) == nullptr || !pp->open)
      continue;

    HWND hwnd = panel_popover_hwnd (pp);
    RECT r;
    gboolean inside = FALSE;
    if (hwnd != nullptr && GetWindowRect (hwnd, &r)) {
      inside = PtInRect (&r, *pt) != FALSE;
      if (!inside && PtInRect (&pp->anchor_rect, *pt))
        inside = TRUE;
    }
    if (!inside)
      panel_popover_close (pp, "outside-click");
  }
  g_list_free (snapshot);
  g_free (pt);
  return G_SOURCE_REMOVE;
}

// Low-level mouse hook (installed on the main thread, where the GTK Win32
// backend pumps messages): check popover bounds synchronously and dispatch
// dismissal via idle so the hook returns immediately without blocking Windows.
static LRESULT CALLBACK
popover_mouse_hook_proc (int nCode, WPARAM wParam, LPARAM lParam)
{
  if (nCode == HC_ACTION &&
      (wParam == WM_LBUTTONDOWN || wParam == WM_RBUTTONDOWN ||
       wParam == WM_MBUTTONDOWN) &&
      g_open_popovers != nullptr) {
    MSLLHOOKSTRUCT *ms = reinterpret_cast<MSLLHOOKSTRUCT *>(lParam);
    POINT pt = ms->pt;
    gboolean any_outside = FALSE;

    for (GList *l = g_open_popovers; l != nullptr; l = l->next) {
      PanelPopover *pp = static_cast<PanelPopover *>(l->data);
      if (!pp->open)
        continue;

      HWND hwnd = panel_popover_hwnd (pp);
      RECT r;
      gboolean inside = FALSE;
      if (hwnd != nullptr && GetWindowRect (hwnd, &r)) {
        inside = PtInRect (&r, pt) != FALSE;
        // Clicks on the anchor button are handled by its toggle handler.
        if (!inside && PtInRect (&pp->anchor_rect, pt))
          inside = TRUE;
      }
      if (!inside) {
        any_outside = TRUE;
        break;
      }
    }

    if (any_outside) {
      // GNOME's menu grab consumes an outside click: it dismisses the menu
      // only. When the overview is open underneath, mark the dismissal so
      // the overview ignores the matching mouse-up instead of closing.
      if (winome::overview_active ())
        g_popover_outside_close_us = g_get_monotonic_time ();
      POINT *pt_copy = g_new (POINT, 1);
      *pt_copy = pt;
      g_idle_add (close_outside_popovers_idle, pt_copy);
    }
  }
  return CallNextHookEx (nullptr, nCode, wParam, lParam);
}

static void
ensure_popover_mouse_hook (void)
{
  if (g_popover_mouse_hook == nullptr)
    g_popover_mouse_hook = SetWindowsHookExW (WH_MOUSE_LL,
                                              popover_mouse_hook_proc,
                                              nullptr, 0);
}

// When the Activities overview opens it takes over the screen: make all panels
// transparent (GNOME style) and close any open popover so it does not float
// over the overview.
static std::vector<GtkWidget *> g_registered_panels;

static void
on_overview_state_changed (bool active, void *user_data G_GNUC_UNUSED)
{
  g_print ("[panel] overview state -> %d\n", (int)active);
  if (active)
    winome_shell_panel_close_popovers ();
  for (GtkWidget *panel : g_registered_panels) {
    if (active)
      gtk_widget_add_css_class (panel, "overview");
    else
      gtk_widget_remove_css_class (panel, "overview");
  }
}

// Register a panel window in the shared overview CSS state tracker.
static void
connect_overview_state (GtkWidget *panel)
{
  if (std::find (g_registered_panels.begin (), g_registered_panels.end (),
                 panel) == g_registered_panels.end ()) {
    g_registered_panels.push_back (panel);
  }
  if (!g_overview_hooked) {
    g_overview_hooked = TRUE;
    winome::set_overview_change_callback (on_overview_state_changed, nullptr);
  }
  if (winome::overview_active ())
    gtk_widget_add_css_class (panel, "overview");
}

// A popover anchored to the given panel button: a borderless toplevel card
// (the GNOME dark card background from the GTK4 stylesheet). @centered centers
// the card on the button (calendar); otherwise it right-aligns near the status
// area (quick settings).
static GtkWidget *
make_panel_popover (GtkWidget *button, GtkWidget *child, gboolean centered)
{
  PanelPopover *pp = g_new0 (PanelPopover, 1);
  pp->button = button;
  pp->centered = centered;

  pp->window = gtk_window_new ();
  gtk_window_set_decorated (GTK_WINDOW (pp->window), FALSE);
  gtk_window_set_resizable (GTK_WINDOW (pp->window), FALSE);
  gtk_widget_add_css_class (pp->window, "quick-settings-popover-window");

  // Shadow-wrap + card: the transparent wrapper leaves room for the card's
  // drop shadow, the card carries the GNOME dark background.
  GtkWidget *wrap = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  gtk_widget_add_css_class (wrap, "quick-settings-popover-shadow-wrap");
  pp->wrap = wrap;
  pp->arrow_area = gtk_drawing_area_new ();
  gtk_drawing_area_set_content_height (
      GTK_DRAWING_AREA (pp->arrow_area),
      (int)(boxpointer_theme ().arrow_rise + 0.5));
  gtk_drawing_area_set_draw_func (GTK_DRAWING_AREA (pp->arrow_area),
                                  draw_popover_arrow, pp, nullptr);
  gtk_box_append (GTK_BOX (wrap), pp->arrow_area);

  pp->card = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  gtk_widget_add_css_class (pp->card, "quick-settings-popover-card");
  if (centered)
    gtk_widget_add_css_class (pp->card, "datemenu-card");
  else if (child != nullptr &&
           g_object_get_data (G_OBJECT (child), "tray-popover") != nullptr)
    gtk_widget_add_css_class (pp->card, "tray-popover-card");
  pp->child = child;
  gtk_box_append (GTK_BOX (pp->card), child);
  gtk_box_append (GTK_BOX (wrap), pp->card);
  gtk_window_set_child (GTK_WINDOW (pp->window), wrap);

  // The popover is a separate toplevel window: give it the same Cantarell font
  // map as the panel so the em-based stylesheet values resolve identically.
  gtk_widget_set_font_map (pp->window, winome::bundled_font_map ());

  g_object_set_data (G_OBJECT (button), "popover", pp);
  g_signal_connect (button, "clicked", G_CALLBACK (on_click_popover), pp);

  // Raise above the (topmost) overview only after GDK's map completes.
  g_signal_connect (pp->window, "map", G_CALLBACK (on_popover_map), pp);

  GtkEventController *keys = gtk_event_controller_key_new ();
  g_signal_connect (keys, "key-pressed", G_CALLBACK (on_popover_key), pp);
  gtk_widget_add_controller (pp->window, keys);

  ensure_popover_mouse_hook ();

  return pp->window;
}

static void
close_all_open_popovers (gboolean immediate)
{
  while (g_open_popovers != nullptr) {
    PanelPopover *pp = static_cast<PanelPopover *> (g_open_popovers->data);
    panel_popover_close_ex (pp, "dismiss", immediate);
  }
}

void
winome_shell_panel_close_popovers (void)
{
  close_all_open_popovers (FALSE);
}

void
winome_shell_panel_close_popovers_immediate (void)
{
  close_all_open_popovers (TRUE);
}
typedef struct PanelStatus {
  bool is_primary;
  guint status_timer_id;
  guint clock_timer_id;
  GtkWidget *activities_indicators;
  int desktop_count;
  int active_desktop;
  GtkWidget *clock_label;
  GtkWidget *messages_indicator;
  GtkWidget *messages_indicator_pad;
  GtkWidget *network_icon;
  GtkWidget *bluetooth_icon;
  GtkWidget *volume_icon;
  GtkWidget *battery_box;
  GtkWidget *battery_icon;
  GtkWidget *battery_label;
  GtkWidget *quick_settings;
  GtkWidget *date_menu;
  GtkWidget *tray_inline_box;
  GtkWidget *plugin_box;
  GtkWidget *tray_menu;
  GtkWidget *tray_list_box;
  GtkWidget *tray_empty_label;
} PanelStatus;

// Sync Activities workspace dots with active Windows virtual desktops
// (upstream panel.js WorkspaceIndicators).
static void
apply_activities_dots (PanelStatus *st, int count, int active)
{
  if (st == nullptr || st->activities_indicators == nullptr)
    return;

  if (count < 1)
    count = 1;
  if (active < 0 || active >= count)
    active = 0;

  if (st->desktop_count == count && st->active_desktop == active)
    return;

  const bool count_changed = st->desktop_count != count;
  st->desktop_count = count;
  st->active_desktop = active;

  if (count_changed) {
    GtkWidget *child = gtk_widget_get_first_child (st->activities_indicators);
    while (child != nullptr) {
      GtkWidget *next = gtk_widget_get_next_sibling (child);
      gtk_box_remove (GTK_BOX (st->activities_indicators), child);
      child = next;
    }
    for (int i = 0; i < count; ++i)
      gtk_box_append (GTK_BOX (st->activities_indicators), create_workspace_dot ());
  }

  const int size = workspace_dot_size ();
  const double mult = (count <= 2) ? 3.625 : (count <= 5) ? 3.25 : 2.75;
  int idx = 0;
  for (GtkWidget *dot = gtk_widget_get_first_child (st->activities_indicators);
       dot != nullptr;
       dot = gtk_widget_get_next_sibling (dot), ++idx) {
    const bool is_active = (idx == active);
    if (is_active) {
      gtk_widget_remove_css_class (dot, "inactive");
      gtk_widget_set_size_request (dot, (int)(size * mult + 0.5), size);
    } else {
      gtk_widget_add_css_class (dot, "inactive");
      gtk_widget_set_size_request (dot, size, size);
    }
  }
}

static void
sync_activities_dots (PanelStatus *st)
{
  const auto snap = winome::vd::cached_snapshot ();
  apply_activities_dots (st, snap.valid ? snap.count : 1,
                         snap.valid ? snap.active_index : 0);
}

static void
on_panel_desktop_changed (int count, int active_index, void *user_data)
{
  apply_activities_dots (static_cast<PanelStatus *> (user_data), count,
                         active_index);
}

// In GNOME the hover/active highlight lives on the .clock child (an St.Widget).
// GtkLabel does not paint its CSS background in GTK4, so the .clock "label" is
// a GtkBox (which does paint it) wrapping the actual text label.
static void
update_messages_indicator (PanelStatus *st)
{
  if (st == nullptr || st->messages_indicator == nullptr)
    return;
  bool dnd_on = false;
  winome::get_do_not_disturb (&dnd_on);
  const bool show = !dnd_on && !winome::notifications_get_all ().empty ();
  gtk_widget_set_visible (st->messages_indicator, show ? TRUE : FALSE);
  if (st->messages_indicator_pad != nullptr)
    gtk_widget_set_visible (st->messages_indicator_pad, show ? TRUE : FALSE);
}

static GtkWidget *
create_clock_button (PanelStatus *st, bool with_popover)
{
  GtkWidget *button;
  GtkWidget *display_box;
  GtkWidget *indicator_pad;
  GtkWidget *clock_box;
  GtkWidget *clock_label;
  GtkWidget *indicator;

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

  // Empty mirror widget sized to match .messages-indicator so showing the dot
  // keeps the clock text centered in the monitor work area.
  indicator_pad = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);
  gtk_widget_set_valign (indicator_pad, GTK_ALIGN_CENTER);
  gtk_widget_set_visible (indicator_pad, FALSE);
  gtk_box_append (GTK_BOX (display_box), indicator_pad);

  clock_box = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);
  gtk_widget_add_css_class (clock_box, "clock");
  gtk_widget_set_valign (clock_box, GTK_ALIGN_CENTER);

  clock_label = gtk_label_new ("00:00");
  gtk_box_append (GTK_BOX (clock_box), clock_label);
  gtk_box_append (GTK_BOX (display_box), clock_box);

  indicator = gtk_image_new_from_icon_name ("message-indicator-symbolic");
  gtk_image_set_pixel_size (GTK_IMAGE (indicator), status_icon_size ());
  gtk_widget_add_css_class (indicator, "messages-indicator");
  gtk_widget_set_valign (indicator, GTK_ALIGN_CENTER);
  gtk_widget_set_visible (indicator, FALSE);
  gtk_box_append (GTK_BOX (display_box), indicator);

  GtkSizeGroup *indicator_sg = gtk_size_group_new (GTK_SIZE_GROUP_HORIZONTAL);
  gtk_size_group_add_widget (indicator_sg, indicator_pad);
  gtk_size_group_add_widget (indicator_sg, indicator);
  g_object_set_data_full (G_OBJECT (display_box), "indicator-size-group",
                          indicator_sg, g_object_unref);

  gtk_button_set_child (GTK_BUTTON (button), display_box);

  st->messages_indicator_pad = indicator_pad;
  st->messages_indicator = indicator;
  st->clock_label = clock_label;
  st->clock_timer_id = g_timeout_add_seconds (1, update_clock, clock_label);
  update_clock (clock_label);
  update_messages_indicator (st);

  if (with_popover) {
    // The clock opens the date/calendar popover (like DateMenuButton), centered
    // below the clock.
    st->date_menu = winome_calendar_new ();
    make_panel_popover (button, st->date_menu, TRUE);
  }

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

// gtk_image_set_from_icon_name invalidates the render node even when the
// name did not change; the 2s status poll would otherwise queue needless
// panel redraws for every tick.
static void
set_icon_name_if_changed (GtkImage *image, const char *name)
{
  if (g_strcmp0 (gtk_image_get_icon_name (image), name) == 0)
    return;
  gtk_image_set_from_icon_name (image, name);
}

static void
update_network_icon (GtkWidget *icon)
{
  bool airplane = false;
  if (winome::get_airplane_mode (&airplane) && airplane) {
    set_icon_name_if_changed (GTK_IMAGE (icon), "airplane-mode-symbolic");
    return;
  }

  WinomeWifiStatus wifi = {};
  if (winome_wifi_get_status (&wifi) && wifi.enabled && wifi.connected) {
    const char *name;
    if (wifi.signal_quality >= 80)
      name = "network-wireless-signal-excellent-symbolic";
    else if (wifi.signal_quality >= 55)
      name = "network-wireless-signal-good-symbolic";
    else if (wifi.signal_quality >= 30)
      name = "network-wireless-signal-ok-symbolic";
    else if (wifi.signal_quality > 0)
      name = "network-wireless-signal-weak-symbolic";
    else
      name = "network-wireless-signal-none-symbolic";
    set_icon_name_if_changed (GTK_IMAGE (icon), name);
    return;
  }

  bool connected = false;
  if (winome::get_network_connected (&connected) && connected) {
    set_icon_name_if_changed (GTK_IMAGE (icon), "network-wired-symbolic");
    return;
  }

  set_icon_name_if_changed (GTK_IMAGE (icon), "network-wireless-offline-symbolic");
}

static void
update_bluetooth_icon (GtkWidget *icon)
{
  if (icon == nullptr)
    return;

  bool present = false, enabled = false;
  winome::get_bluetooth_status (&present, &enabled, nullptr, 0);
  GtkWidget *parent = gtk_widget_get_parent (icon);
  if (parent != nullptr)
    gtk_widget_set_visible (parent, present && enabled);
  set_icon_name_if_changed (GTK_IMAGE (icon), "bluetooth-active-symbolic");
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

  set_icon_name_if_changed (GTK_IMAGE (icon), name);
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

    set_icon_name_if_changed (GTK_IMAGE (st->battery_icon), name);

    if (winome::get_config ().show_battery_percentage) {
      char pct[16];
      snprintf (pct, sizeof (pct), "%d%%", battery.percentage);
      gtk_label_set_text (GTK_LABEL (st->battery_label), pct);
      gtk_widget_set_visible (st->battery_label, TRUE);
    } else {
      gtk_widget_set_visible (st->battery_label, FALSE);
    }
  } else {
    // No battery/UPS: like status/system.js, show the power icon instead.
    gtk_widget_set_visible (st->battery_box, TRUE);
    set_icon_name_if_changed (GTK_IMAGE (st->battery_icon),
                               "system-shutdown-symbolic");
    gtk_widget_set_visible (st->battery_label, FALSE);
  }
}

static gboolean
refresh_status (gpointer data)
{
  PanelStatus *st = static_cast<PanelStatus *>(data);
  if (st == nullptr)
    return G_SOURCE_REMOVE;

  if (st->clock_label != nullptr)
    update_clock (st->clock_label);
  sync_activities_dots (st);
  update_messages_indicator (st);
  if (st->network_icon != nullptr)
    update_network_icon (st->network_icon);
  if (st->bluetooth_icon != nullptr)
    update_bluetooth_icon (st->bluetooth_icon);
  if (st->volume_icon != nullptr)
    update_volume_icon (st->volume_icon);
  if (st->battery_box != nullptr)
    update_battery_icon (st);
  if (st->quick_settings != nullptr)
    winome_quick_settings_refresh (st->quick_settings);
  if (st->date_menu != nullptr)
    winome_calendar_refresh (st->date_menu);

  return G_SOURCE_CONTINUE;
}

// --- System Tray (compatible toolbar / Win11 UIA overflow) ----------------

typedef struct TrayRowPayload {
  uint64_t id;
} TrayRowPayload;

static void
clear_box_children (GtkWidget *box)
{
  if (box == nullptr)
    return;
  GtkWidget *child = gtk_widget_get_first_child (box);
  while (child != nullptr) {
    GtkWidget *next = gtk_widget_get_next_sibling (child);
    gtk_box_remove (GTK_BOX (box), child);
    child = next;
  }
}

static GtkWidget *
make_tray_image (const winome::TrayIconEntry &item, int px)
{
  GtkWidget *img = nullptr;
  GdkTexture *tex = winome::systray_create_texture (item);
  if (tex != nullptr) {
    img = gtk_image_new_from_paintable (GDK_PAINTABLE (tex));
    g_object_unref (tex);
  } else {
    img = gtk_image_new_from_icon_name (
        winome::tray_fallback_icon_name (item.title, item.app_hint));
  }
  gtk_image_set_pixel_size (GTK_IMAGE (img), px);
  return img;
}

static void
on_tray_item_clicked (GtkButton *button, gpointer G_GNUC_UNUSED data)
{
  TrayRowPayload *payload = static_cast<TrayRowPayload *> (
      g_object_get_data (G_OBJECT (button), "tray-payload"));
  if (payload == nullptr)
    return;
  winome_shell_panel_close_popovers_immediate ();
  AllowSetForegroundWindow (ASFW_ANY);
  POINT pt{};
  GetCursorPos (&pt);
  winome::systray_activate_item (payload->id, false, pt.x, pt.y);
}

static void
on_tray_item_right_click (GtkGestureClick *gesture G_GNUC_UNUSED,
                          int n_press G_GNUC_UNUSED,
                          double x G_GNUC_UNUSED,
                          double y G_GNUC_UNUSED,
                          gpointer user_data)
{
  GtkWidget *button = GTK_WIDGET (user_data);
  TrayRowPayload *payload = static_cast<TrayRowPayload *> (
      g_object_get_data (G_OBJECT (button), "tray-payload"));
  if (payload == nullptr)
    return;
  winome_shell_panel_close_popovers_immediate ();
  AllowSetForegroundWindow (ASFW_ANY);
  POINT pt{};
  GetCursorPos (&pt);
  winome::systray_activate_item (payload->id, true, pt.x, pt.y);
}

static void
rebuild_tray_views (PanelStatus *st)
{
  if (st == nullptr)
    return;

  std::vector<winome::TrayIconEntry> items = winome::systray_get_items ();

  if (st->tray_inline_box != nullptr) {
    clear_box_children (st->tray_inline_box);
    const size_t limit = std::min<size_t> (items.size (), 4);
    for (size_t i = 0; i < limit; ++i) {
      GtkWidget *img = make_tray_image (items[i], 14);
      gtk_widget_add_css_class (img, "system-status-icon");
      gtk_widget_set_tooltip_text (img, items[i].title.c_str ());
      gtk_box_append (GTK_BOX (st->tray_inline_box), img);
    }
    GtkWidget *chevron = create_status_icon ("pan-down-symbolic");
    gtk_image_set_pixel_size (GTK_IMAGE (chevron), 13);
    gtk_box_append (GTK_BOX (st->tray_inline_box), chevron);
  }

  if (st->tray_list_box != nullptr) {
    clear_box_children (st->tray_list_box);
    const bool empty = items.empty ();
    if (st->tray_empty_label != nullptr)
      gtk_widget_set_visible (st->tray_empty_label, empty ? TRUE : FALSE);

    for (const auto &item : items) {
      GtkWidget *row_btn = gtk_button_new ();
      gtk_widget_add_css_class (row_btn, "popup-menu-item");
      gtk_widget_set_focus_on_click (row_btn, FALSE);
      gtk_widget_set_hexpand (row_btn, TRUE);

      GtkWidget *row_box = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 10);
      GtkWidget *icon = make_tray_image (item, 16);
      gtk_widget_add_css_class (icon, "popup-menu-icon");
      GtkWidget *lbl = gtk_label_new (item.title.c_str ());
      gtk_label_set_xalign (GTK_LABEL (lbl), 0.0f);
      gtk_label_set_ellipsize (GTK_LABEL (lbl), PANGO_ELLIPSIZE_END);
      gtk_label_set_max_width_chars (GTK_LABEL (lbl), 28);
      gtk_widget_set_hexpand (lbl, TRUE);

      gtk_box_append (GTK_BOX (row_box), icon);
      gtk_box_append (GTK_BOX (row_box), lbl);
      gtk_button_set_child (GTK_BUTTON (row_btn), row_box);
      gtk_widget_set_tooltip_text (row_btn, item.title.c_str ());

      TrayRowPayload *payload = g_new (TrayRowPayload, 1);
      payload->id = item.id;
      g_object_set_data_full (G_OBJECT (row_btn), "tray-payload", payload, g_free);
      g_signal_connect (row_btn, "clicked",
                        G_CALLBACK (on_tray_item_clicked), nullptr);

      GtkGesture *right_click = gtk_gesture_click_new ();
      gtk_gesture_single_set_button (GTK_GESTURE_SINGLE (right_click),
                                     GDK_BUTTON_SECONDARY);
      g_signal_connect (right_click, "pressed",
                        G_CALLBACK (on_tray_item_right_click), row_btn);
      gtk_widget_add_controller (row_btn, GTK_EVENT_CONTROLLER (right_click));

      gtk_box_append (GTK_BOX (st->tray_list_box), row_btn);
    }
  }
}

static void
on_systray_changed (void *data)
{
  rebuild_tray_views (static_cast<PanelStatus *> (data));
}

static void
on_tray_refresh_clicked (GtkButton *button G_GNUC_UNUSED,
                         gpointer G_GNUC_UNUSED data)
{
  winome::systray_refresh_async (true);
}

static void
on_tray_native_overflow_clicked (GtkButton *button G_GNUC_UNUSED,
                                 gpointer G_GNUC_UNUSED data)
{
  winome_shell_panel_close_popovers_immediate ();
  AllowSetForegroundWindow (ASFW_ANY);
  winome::systray_toggle_native_overflow ();
}

static void
on_tray_popover_map (GtkWidget *win G_GNUC_UNUSED, gpointer data)
{
  PanelStatus *st = static_cast<PanelStatus *> (data);
  winome::systray_refresh_async (false);
  rebuild_tray_views (st);
}

static GtkWidget *
create_tray_button (PanelStatus *st)
{
  GtkWidget *button = gtk_button_new ();
  gtk_widget_add_css_class (button, "panel-button");
  gtk_widget_set_focus_on_click (button, FALSE);
  gtk_widget_set_vexpand (button, TRUE);
  gtk_widget_set_tooltip_text (button, winome::tr ("System Tray", "系统托盘"));

  st->tray_inline_box = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 6);
  gtk_widget_add_css_class (st->tray_inline_box, "panel-status-indicators-box");
  gtk_widget_set_valign (st->tray_inline_box, GTK_ALIGN_CENTER);
  gtk_button_set_child (GTK_BUTTON (button), st->tray_inline_box);

  st->tray_menu = gtk_box_new (GTK_ORIENTATION_VERTICAL, 8);
  gtk_widget_add_css_class (st->tray_menu, "tray-menu-box");
  g_object_set_data (G_OBJECT (st->tray_menu), "tray-popover",
                     GINT_TO_POINTER (1));
  gtk_widget_set_size_request (st->tray_menu, 280, -1);

  GtkWidget *header = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 6);
  GtkWidget *title = gtk_label_new (winome::tr ("System Tray", "系统托盘"));
  gtk_widget_add_css_class (title, "tray-menu-title");
  gtk_label_set_xalign (GTK_LABEL (title), 0.0f);
  gtk_widget_set_hexpand (title, TRUE);

  GtkWidget *refresh_btn = gtk_button_new_from_icon_name ("view-refresh-symbolic");
  gtk_widget_add_css_class (refresh_btn, "tray-header-button");
  gtk_widget_add_css_class (refresh_btn, "icon-button");
  gtk_widget_set_focus_on_click (refresh_btn, FALSE);
  gtk_widget_set_tooltip_text (refresh_btn, winome::tr ("Refresh", "刷新"));
  g_signal_connect (refresh_btn, "clicked",
                    G_CALLBACK (on_tray_refresh_clicked), nullptr);

  GtkWidget *overflow_btn =
      gtk_button_new_with_label (winome::tr ("Windows Tray", "原生托盘"));
  gtk_widget_add_css_class (overflow_btn, "tray-header-button");
  gtk_widget_set_focus_on_click (overflow_btn, FALSE);
  gtk_widget_set_tooltip_text (
      overflow_btn, winome::tr ("Open Windows system tray overflow", "打开 Windows 系统托盘溢出区"));
  g_signal_connect (overflow_btn, "clicked",
                    G_CALLBACK (on_tray_native_overflow_clicked), nullptr);

  gtk_box_append (GTK_BOX (header), title);
  gtk_box_append (GTK_BOX (header), refresh_btn);
  gtk_box_append (GTK_BOX (header), overflow_btn);
  gtk_box_append (GTK_BOX (st->tray_menu), header);

  GtkWidget *sep = gtk_separator_new (GTK_ORIENTATION_HORIZONTAL);
  gtk_widget_add_css_class (sep, "popup-separator-menu-item");
  gtk_box_append (GTK_BOX (st->tray_menu), sep);

  st->tray_empty_label = gtk_label_new (
      winome::tr ("No third-party tray icons detected.\nClick \"Windows Tray\" above to open native overflow.",
                  "暂未发现第三方托盘图标。\n可点击右上角“原生托盘”打开 Windows 系统托盘。"));
  gtk_label_set_wrap (GTK_LABEL (st->tray_empty_label), TRUE);
  gtk_label_set_xalign (GTK_LABEL (st->tray_empty_label), 0.0f);
  gtk_widget_set_margin_top (st->tray_empty_label, 6);
  gtk_widget_set_margin_bottom (st->tray_empty_label, 6);
  gtk_box_append (GTK_BOX (st->tray_menu), st->tray_empty_label);

  GtkWidget *scroll = gtk_scrolled_window_new ();
  gtk_scrolled_window_set_policy (GTK_SCROLLED_WINDOW (scroll),
                                  GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
  gtk_scrolled_window_set_propagate_natural_height (GTK_SCROLLED_WINDOW (scroll),
                                                    TRUE);
  gtk_scrolled_window_set_max_content_height (GTK_SCROLLED_WINDOW (scroll), 320);

  st->tray_list_box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 2);
  gtk_scrolled_window_set_child (GTK_SCROLLED_WINDOW (scroll), st->tray_list_box);
  gtk_box_append (GTK_BOX (st->tray_menu), scroll);

  GtkWidget *pop_win = make_panel_popover (button, st->tray_menu, FALSE);
  g_signal_connect (pop_win, "map", G_CALLBACK (on_tray_popover_map), st);

  rebuild_tray_views (st);
  return button;
}

static GtkWidget *
create_quick_settings (PanelStatus *st)
{
  GtkWidget *button;
  GtkWidget *status_box;

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

  // The status area opens the quick settings popover, right-aligned near the
  // status area.
  st->quick_settings = winome_quick_settings_new ();
  make_panel_popover (button, st->quick_settings, FALSE);

  return button;
}

// --- CSS --------------------------------------------------------------------

static GtkCssProvider *g_theme_provider = nullptr;
static GtkCssProvider *g_dev_css_provider = nullptr;

static void
load_optional_dev_css (GtkCssProvider *provider)
{
  if (provider == nullptr)
    return;
  if (!winome::get_config ().developer_mode || winome::is_safe_mode ()) {
    gtk_css_provider_load_from_string (provider, "");
    return;
  }
  const std::wstring dir = winome::app_data_dir ();
  if (dir.empty ()) {
    gtk_css_provider_load_from_string (provider, "");
    return;
  }
  const std::wstring css_path = dir + L"\\user-debug.css";
  HANDLE f = CreateFileW (css_path.c_str (), GENERIC_READ, FILE_SHARE_READ,
                          nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (f == INVALID_HANDLE_VALUE) {
    gtk_css_provider_load_from_string (provider, "");
    return;
  }
  LARGE_INTEGER sz {};
  std::string content;
  if (GetFileSizeEx (f, &sz) && sz.QuadPart > 0 && sz.QuadPart <= 1048576) {
    content.resize (static_cast<size_t> (sz.QuadPart));
    DWORD got = 0;
    if (ReadFile (f, content.data (), static_cast<DWORD> (sz.QuadPart),
                  &got, nullptr)) {
      content.resize (got);
    } else {
      content.clear ();
    }
  }
  CloseHandle (f);
  gtk_css_provider_load_from_string (provider, content.c_str ());
}

static void
load_css (GtkWidget *panel G_GNUC_UNUSED)
{
  GdkDisplay *display = gdk_display_get_default ();

  if (g_theme_provider == nullptr) {
    g_theme_provider = gtk_css_provider_new ();
    gtk_css_provider_load_from_resource (g_theme_provider,
                                         "/org/winome/theme/gnome-shell-gtk4.css");

    // Register custom GNOME Shell symbolic icons bundled in GResource alongside CSS.
    gtk_icon_theme_add_resource_path (gtk_icon_theme_get_for_display (display),
                                      "/org/winome/theme/icons");

    // Load at display scope so descendant selectors (#panel .panel-button,
    // #panel .panel-button.clock-display .clock, etc.) match all child widgets.
    gtk_style_context_add_provider_for_display (
      display,
      GTK_STYLE_PROVIDER (g_theme_provider),
      GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
  }

  if (g_dev_css_provider == nullptr) {
    g_dev_css_provider = gtk_css_provider_new ();
    load_optional_dev_css (g_dev_css_provider);
    gtk_style_context_add_provider_for_display (
      display,
      GTK_STYLE_PROVIDER (g_dev_css_provider),
      GTK_STYLE_PROVIDER_PRIORITY_USER);
  }
}

bool
winome_shell_reload_theme (void)
{
  if (g_theme_provider != nullptr) {
    gtk_css_provider_load_from_resource (g_theme_provider,
                                         "/org/winome/theme/gnome-shell-gtk4.css");
  }
  if (g_dev_css_provider != nullptr)
    load_optional_dev_css (g_dev_css_provider);
  bool st_ok = true;
  if (winome::StEngine *eng = winome::StEngine::get ())
    st_ok = eng->load_stylesheet_resource ("resource:///org/winome/theme/gnome-shell-dark.css");
  g_print ("[theme] theme reloaded (st_ok=%d)\n", st_ok ? 1 : 0);
  return st_ok;
}

// HWND of the topmost open popover, or NULL. The list is prepended on open,
// so the LAST element is the most recently opened (topmost) popover.
void *
winome_shell_panel_top_popover_hwnd (void)
{
  if (g_open_popovers == nullptr)
    return nullptr;
  GList *last = g_list_last (g_open_popovers);
  PanelPopover *pp = static_cast<PanelPopover *> (last->data);
  return panel_popover_hwnd (pp);
}

// See shell-panel.h: TRUE for ~350ms after a popover was dismissed by an
// outside click while the overview was open — the overview must swallow the
// matching click (GNOME's menu grab consumes it).
int
winome_shell_panel_popover_just_closed (void)
{
  if (g_popover_outside_close_us == 0)
    return FALSE;
  return (g_get_monotonic_time () - g_popover_outside_close_us) < 350 * 1000;
}

// --- Panel entry point ------------------------------------------------------

static void
on_panel_config_changed (void *data)
{
  refresh_status (data);
}

static void
on_panel_status_event (void *data)
{
  refresh_status (data);
}

struct PanelPluginSlotSpec {
  std::string key;
  std::string plugin_id;
  std::string slot;
  std::string icon_name;
  std::string text;
  std::string tooltip;
};

struct PanelPluginClickData {
  std::string plugin_id;
  std::string slot;
};

static std::vector<PanelPluginSlotSpec> g_panel_plugin_slots;
static GtkWidget *g_primary_plugin_box = nullptr;
static WinomePluginTopBarClickCb g_panel_plugin_click_cb = nullptr;

static void
on_panel_plugin_btn_clicked (GtkButton *button, gpointer G_GNUC_UNUSED data)
{
  auto *payload = static_cast<PanelPluginClickData *> (
      g_object_get_data (G_OBJECT (button), "winome-plugin-click"));
  if (payload != nullptr && g_panel_plugin_click_cb != nullptr)
    g_panel_plugin_click_cb (payload->plugin_id.c_str (), payload->slot.c_str ());
}

static void
rebuild_panel_plugin_slots (void)
{
  if (g_primary_plugin_box == nullptr)
    return;
  clear_box_children (g_primary_plugin_box);
  gtk_widget_set_visible (g_primary_plugin_box,
                          g_panel_plugin_slots.empty () ? FALSE : TRUE);

  for (const auto &s : g_panel_plugin_slots) {
    GtkWidget *btn = gtk_button_new ();
    gtk_widget_add_css_class (btn, "panel-button");
    gtk_widget_set_focus_on_click (btn, FALSE);
    gtk_widget_set_vexpand (btn, TRUE);
    if (!s.tooltip.empty ())
      gtk_widget_set_tooltip_text (btn, s.tooltip.c_str ());

    GtkWidget *box = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 4);
    gtk_widget_add_css_class (box, "panel-status-indicators-box");
    gtk_widget_set_valign (box, GTK_ALIGN_CENTER);

    if (!s.icon_name.empty ())
      gtk_box_append (GTK_BOX (box), create_status_icon (s.icon_name.c_str ()));
    if (!s.text.empty ()) {
      GtkWidget *lbl = gtk_label_new (s.text.c_str ());
      gtk_label_set_single_line_mode (GTK_LABEL (lbl), TRUE);
      gtk_box_append (GTK_BOX (box), lbl);
    }

    gtk_button_set_child (GTK_BUTTON (btn), box);

    auto *click_data = new PanelPluginClickData{s.plugin_id, s.slot};
    g_object_set_data_full (
        G_OBJECT (btn), "winome-plugin-click", click_data,
        +[](gpointer p) { delete static_cast<PanelPluginClickData *>(p); });
    g_signal_connect (btn, "clicked",
                      G_CALLBACK (on_panel_plugin_btn_clicked), nullptr);
    gtk_box_append (GTK_BOX (g_primary_plugin_box), btn);
  }
}

void
winome_shell_panel_set_plugin_click_cb (WinomePluginTopBarClickCb cb)
{
  g_panel_plugin_click_cb = cb;
}

void
winome_shell_panel_plugin_upsert (const char *key,
                                  const char *plugin_id,
                                  const char *slot,
                                  const char *icon_name,
                                  const char *text,
                                  const char *tooltip)
{
  if (key == nullptr || plugin_id == nullptr)
    return;
  PanelPluginSlotSpec spec;
  spec.key = key;
  spec.plugin_id = plugin_id;
  spec.slot = slot ? slot : "";
  spec.icon_name = icon_name ? icon_name : "";
  spec.text = text ? text : "";
  spec.tooltip = tooltip ? tooltip : "";

  bool updated = false;
  for (auto &existing : g_panel_plugin_slots) {
    if (existing.key == spec.key) {
      existing = spec;
      updated = true;
      break;
    }
  }
  if (!updated)
    g_panel_plugin_slots.push_back (std::move (spec));

  rebuild_panel_plugin_slots ();
}

void
winome_shell_panel_plugin_remove (const char *key)
{
  if (key == nullptr)
    return;
  const size_t before = g_panel_plugin_slots.size ();
  g_panel_plugin_slots.erase (
      std::remove_if (g_panel_plugin_slots.begin (), g_panel_plugin_slots.end (),
                      [key](const PanelPluginSlotSpec &s) { return s.key == key; }),
      g_panel_plugin_slots.end ());
  if (g_panel_plugin_slots.size () != before)
    rebuild_panel_plugin_slots ();
}

static void
on_panel_destroy (GtkWidget *panel, gpointer user_data)
{
  PanelStatus *st = static_cast<PanelStatus *> (user_data);
  g_registered_panels.erase (
      std::remove (g_registered_panels.begin (), g_registered_panels.end (),
                   panel),
      g_registered_panels.end ());

  if (st != nullptr) {
    if (st->plugin_box != nullptr && st->plugin_box == g_primary_plugin_box)
      g_primary_plugin_box = nullptr;
    if (st->status_timer_id != 0)
      g_source_remove (st->status_timer_id);
    if (st->clock_timer_id != 0)
      g_source_remove (st->clock_timer_id);
    winome::vd::remove_change_listener (on_panel_desktop_changed, st);
    winome::remove_config_listener (on_panel_config_changed, st);
    winome::notifications_remove_listener (on_panel_status_event, st);
    if (st->is_primary) {
      winome::systray_remove_listener (on_systray_changed, st);
      winome::remove_volume_listener (on_panel_status_event, st);
      winome_wifi_remove_listener (on_panel_status_event, st);
    }
    g_free (st);
  }
}

static GtkWidget *
build_shell_panel (bool primary)
{
  GtkWidget *panel;
  GtkWidget *layout;
  GtkWidget *left_box;
  GtkWidget *center_box;
  GtkWidget *right_box;
  PanelStatus *status;

  panel = gtk_window_new ();
  gtk_window_set_title (GTK_WINDOW (panel),
                        primary ? "WINOME Shell" : "WINOME Secondary Panel");
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

  status = g_new0 (PanelStatus, 1);
  status->is_primary = primary;
  status->desktop_count = -1;
  status->active_desktop = -1;
  gtk_box_append (GTK_BOX (left_box),
                  create_activities_button (&status->activities_indicators));
  sync_activities_dots (status);
  winome::vd::add_change_listener (on_panel_desktop_changed, status);
  gtk_box_append (GTK_BOX (center_box), create_clock_button (status, primary));
  if (primary) {
    status->plugin_box = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 2);
    gtk_widget_set_name (status->plugin_box, "panelPlugins");
    gtk_widget_set_visible (status->plugin_box, FALSE);
    gtk_box_append (GTK_BOX (right_box), status->plugin_box);
    g_primary_plugin_box = status->plugin_box;
    rebuild_panel_plugin_slots ();

    gtk_box_append (GTK_BOX (right_box), create_tray_button (status));
    gtk_box_append (GTK_BOX (right_box), create_quick_settings (status));
  }

  winome_panel_layout_set (layout, left_box, center_box, right_box);

  if (primary) {
    refresh_status (status);
    // Slow fallback poll for battery percentage / Bluetooth; volume and WLAN
    // push instant callbacks via WASAPI and WlanRegisterNotification.
    status->status_timer_id = g_timeout_add_seconds (5, refresh_status, status);
    winome::systray_add_listener (on_systray_changed, status);
    winome::add_volume_listener (on_panel_status_event, status);
    winome_wifi_add_listener (on_panel_status_event, status);
  }
  winome::add_config_listener (on_panel_config_changed, status);
  winome::notifications_add_listener (on_panel_status_event, status);
  g_signal_connect (panel, "destroy", G_CALLBACK (on_panel_destroy), status);

  // Give the panel its own font map so the bundled Cantarell font is used.
  winome::install_bundled_fonts (panel);

  if (primary)
    load_css (panel);

  // Make the panel transparent (and close popovers) while the overview is open.
  connect_overview_state (panel);

  return panel;
}

GtkWidget *
winome_shell_panel_new (void)
{
  return build_shell_panel (true);
}

GtkWidget *
winome_shell_panel_new_secondary (void)
{
  return build_shell_panel (false);
}

int
winome_shell_panel_height (void)
{
  return panel_height ();
}
