// SPDX-License-Identifier: GPL-2.0-only
// Copyright (C) 2026 WINOME contributors
//
// Geometry and look mirror workspaceThumbnail.js ThumbnailsBox:
//   .workspace-thumbnails { spacing: 6px; padding: 6px }
//   .workspace-thumbnails .workspace-thumbnail { background-color: #46464e;
//                                                 border-radius: 4px }
//   .workspace-thumbnail-indicator { border: 3px solid -st-accent-color;
//                                     border-radius: 8px }
// The vScale/hScale pill sizing follows vfunc_allocate; the indicator is
// allocated around the active thumbnail inflated by its border widths, so
// the 3px frame lands outside the pill exactly like upstream.

#include "overview-thumbs.h"
#include "overview-layout.h"
#include "st-engine.h"

#include <algorithm>
#include <cmath>
#include <new>
#include <vector>

namespace winome {

namespace {

// upstream MAX_THUMBNAIL_SCALE (also enforced by the caller's box_h).
constexpr double kMaxThumbnailScale = 0.08;
constexpr double kSpacingFallback = 12.0;   // .workspace-thumbnails spacing
constexpr double kPaddingFallback = 6.0;    // .workspace-thumbnails padding
constexpr double kThumbRadiusFallback = 6.0;
// .workspace-thumbnail-indicator: border shorthand values are not directly
// queryable through the St engine; the accent color matches the fixed
// -st-accent-color default used elsewhere in WINOME.
constexpr double kIndicatorWidthFallback = 3.0;
constexpr double kIndicatorRadiusFallback = 9.0;

struct ThumbRect {
  double x, y, w, h;
};

struct WinomeThumbs {
  GtkWidget parent_instance;

  std::vector<ThumbRect> pills;
  bool has_add_pill = false;
  ThumbRect add_pill{};
  bool can_manage = false;
  int active = -1;
  int hover_index = -1;     // 0..pills.size()-1, or pills.size() for '+'
  bool hover_close = false; // cursor inside top-right close badge
  int drop_target = -1;     // -1 none, 0..pills.size()-1, or pills.size() for '+'
  double scale = 0;         // pill px per porthole px

  // Continuous indicator position (workspacesView.js _scrollAdjustment.value
  // while a workspace switch animates): -1 = snap to `active`.
  double indicator_value = -1.0;

  // GPU transform during WINDOW_PICKER <-> APP_GRID (overviewControls.js
  // _getThumbnailsBoxParams: pivot (0.5, 0.5), scale 1->0.5, ty 0->h/2).
  double anim_scale = 1.0;
  double anim_translate_y = 0.0;
  double panel_top_ratio = 0.0;

  GdkTexture *wallpaper = nullptr;

  double spacing = kSpacingFallback;
  double padding = kPaddingFallback;
  double thumb_radius = kThumbRadiusFallback;
  double indicator_width = kIndicatorWidthFallback;
  double indicator_radius = kIndicatorRadiusFallback;
  GdkRGBA pill_color;
  GdkRGBA indicator_color;
  bool theme_resolved = false;

  void (*click_cb) (int index, void *data) = nullptr;
  void *click_data = nullptr;
  void (*add_cb) (void *data) = nullptr;
  void (*remove_cb) (int index, void *data) = nullptr;
  void *manage_data = nullptr;
};

struct WinomeThumbsClass {
  GtkWidgetClass parent_class;
};

#define WINOME_THUMBS(obj) \
  (G_TYPE_CHECK_INSTANCE_CAST ((obj), winome_thumbs_get_type (), WinomeThumbs))

G_DEFINE_TYPE (WinomeThumbs, winome_thumbs, GTK_TYPE_WIDGET)

// --- theme lookups (StEngine, with compiled-CSS fallbacks) -------------------

static void
resolve_theme (WinomeThumbs *self)
{
  if (self->theme_resolved)
    return;
  self->theme_resolved = true;

  self->pill_color = (GdkRGBA){0.275f, 0.275f, 0.306f, 1.0f};  // #46464e
  self->indicator_color = (GdkRGBA){0.208f, 0.518f, 0.894f, 1.0f};  // #3584e4

  StEngine *engine = StEngine::get ();
  if (engine == nullptr)
    return;

  StEngine::Node box (*engine, nullptr, "", "workspace-thumbnails", "");
  double v = 0;
  if (box.lookup_length ("spacing", &v) && v > 0)
    self->spacing = std::max (v, 10.0);
  if (box.lookup_length ("padding", &v) && v >= 0)
    self->padding = std::max (v, 6.0);

  StEngine::Node thumb (*engine, &box, "", "workspace-thumbnail", "");
  std::string color;
  if (thumb.lookup_color ("background-color", &color))
    gdk_rgba_parse (&self->pill_color, color.c_str ());
  if (thumb.lookup_length ("border-radius", &v) && v > 0)
    self->thumb_radius = std::max (v, 5.0);
}

// --- snapshot ------------------------------------------------------------------

static void
winome_thumbs_snapshot (GtkWidget *widget, GtkSnapshot *snapshot)
{
  WinomeThumbs *self = WINOME_THUMBS (widget);
  resolve_theme (self);

  if (self->pills.empty ())
    return;

  const bool has_transform =
      (std::abs (self->anim_scale - 1.0) > 0.002 ||
       std::abs (self->anim_translate_y) > 0.25);
  if (has_transform) {
    const float cx = static_cast<float> (gtk_widget_get_width (widget)) * 0.5f;
    const float cy = static_cast<float> (gtk_widget_get_height (widget)) * 0.5f;
    const float sc = static_cast<float> (std::clamp (self->anim_scale, 0.05, 2.0));
    graphene_point_t p_to =
        GRAPHENE_POINT_INIT (cx, cy + static_cast<float> (self->anim_translate_y));
    graphene_point_t p_back = GRAPHENE_POINT_INIT (-cx, -cy);
    gtk_snapshot_save (snapshot);
    gtk_snapshot_translate (snapshot, &p_to);
    gtk_snapshot_scale (snapshot, sc, sc);
    gtk_snapshot_translate (snapshot, &p_back);
  }

  // .workspace-thumbnail pills: the wallpaper drawn cover-style inside a
  // rounded clip with a subtle 1px border for crisp definition.
  for (int i = 0; i < (int)self->pills.size (); ++i) {
    const ThumbRect &pill = self->pills[i];
    graphene_rect_t rect;
    graphene_rect_init (&rect, (float)pill.x, (float)pill.y, (float)pill.w,
                        (float)pill.h);
    GskRoundedRect clip;
    gsk_rounded_rect_init_from_rect (&clip, &rect,
                                     (float)self->thumb_radius);
    gtk_snapshot_push_rounded_clip (snapshot, &clip);

    bool drawn = false;
    if (self->wallpaper != nullptr) {
      double tw = gdk_texture_get_width (self->wallpaper);
      double th = gdk_texture_get_height (self->wallpaper);
      if (tw > 0 && th > 0 && pill.w > 0 && pill.h > 0) {
        WallpaperDrawRect wr = compute_workarea_wallpaper_draw_rect (
            pill.x, pill.y, pill.w, pill.h, tw, th, self->panel_top_ratio);
        graphene_rect_t tex_rect;
        graphene_rect_init (&tex_rect, (float)wr.x, (float)wr.y, (float)wr.w,
                            (float)wr.h);
        gtk_snapshot_append_scaled_texture (
            snapshot, self->wallpaper, GSK_SCALING_FILTER_LINEAR, &tex_rect);
        drawn = true;
      }
    }
    if (!drawn)
      gtk_snapshot_append_color (snapshot, &self->pill_color, &rect);

    gtk_snapshot_pop (snapshot);

    float b_widths[4] = {1.0f, 1.0f, 1.0f, 1.0f};
    GdkRGBA b_col = {1.0f, 1.0f, 1.0f,
                     (i == self->hover_index) ? 0.22f : 0.10f};
    GdkRGBA b_cols[4] = {b_col, b_col, b_col, b_col};
    gtk_snapshot_append_border (snapshot, &clip, b_widths, b_cols);
  }

  // Drop-target outline highlight during drag.
  if (self->drop_target >= 0 &&
      self->drop_target < (int)self->pills.size ()) {
    const ThumbRect &tr = self->pills[self->drop_target];
    double bw = self->indicator_width;
    graphene_rect_t rect;
    graphene_rect_init (&rect, (float)(tr.x - bw), (float)(tr.y - bw),
                        (float)(tr.w + 2 * bw), (float)(tr.h + 2 * bw));
    GskRoundedRect outline;
    gsk_rounded_rect_init_from_rect (&outline, &rect,
                                     (float)self->indicator_radius);
    float widths[4] = {(float)bw, (float)bw, (float)bw, (float)bw};
    GdkRGBA hi = {0.48f, 0.72f, 0.98f, 1.0f};
    GdkRGBA colors[4] = {hi, hi, hi, hi};
    gtk_snapshot_append_border (snapshot, &outline, widths, colors);
  }

  // .workspace-thumbnail-indicator: 3px accent frame allocated AROUND the
  // active pill (inflated by the border widths, radius 8). While a workspace
  // switch animates, `indicator_value` slides the frame between the two
  // adjacent pills — upstream reads the scroll adjustment in vfunc_allocate
  // (indicatorUpperWs/indicatorLowerWs) so the frame tracks the eased
  // scroll position instead of snapping.
  double pos = self->indicator_value;
  if (pos < 0)
    pos = self->active;
  if (pos >= 0 && pos < (int)self->pills.size ()) {
    int lo = (int)pos;
    int hi = std::min (lo + 1, (int)self->pills.size () - 1);
    double f = pos - lo;
    const ThumbRect &a = self->pills[lo];
    const ThumbRect &b = self->pills[hi];
    double x = a.x + (b.x - a.x) * f;
    double y = a.y + (b.y - a.y) * f;
    double w = a.w + (b.w - a.w) * f;
    double h = a.h + (b.h - a.h) * f;
    double bwidth = self->indicator_width;
    graphene_rect_t rect;
    graphene_rect_init (&rect, (float)(x - bwidth), (float)(y - bwidth),
                        (float)(w + 2 * bwidth), (float)(h + 2 * bwidth));
    GskRoundedRect outline;
    gsk_rounded_rect_init_from_rect (&outline, &rect,
                                     (float)self->indicator_radius);
    float widths[4] = {(float)bwidth, (float)bwidth, (float)bwidth,
                       (float)bwidth};
    GdkRGBA colors[4] = {self->indicator_color, self->indicator_color,
                         self->indicator_color, self->indicator_color};
    gtk_snapshot_append_border (snapshot, &outline, widths, colors);
  }

  if (has_transform)
    gtk_snapshot_restore (snapshot);
}

// --- measure ---------------------------------------------------------------------

static void
winome_thumbs_measure (GtkWidget *widget, GtkOrientation orientation,
                       int for_size, int *minimum, int *natural,
                       int *minimum_baseline, int *natural_baseline)
{
  (void)for_size;
  (void)minimum_baseline;
  (void)natural_baseline;
  WinomeThumbs *self = WINOME_THUMBS (widget);

  int w = 0, h = 0;
  if (!self->pills.empty ()) {
    double right = 0;
    for (const ThumbRect &pill : self->pills)
      right = std::max (right, pill.x + pill.w);
    if (self->has_add_pill)
      right = std::max (right, self->add_pill.x + self->add_pill.w);
    w = (int)(right + self->padding + 0.5);
    h = (int)(self->pills[0].h + 2 * self->padding + 0.5);
  }

  if (orientation == GTK_ORIENTATION_HORIZONTAL) {
    *minimum = *natural = w;
  } else {
    *minimum = *natural = h;
  }
}

// --- hover & clicks -----------------------------------------------------------

static void
winome_thumbs_motion (GtkEventControllerMotion *motion G_GNUC_UNUSED, double x,
                      double y, gpointer user_data)
{
  WinomeThumbs *self = WINOME_THUMBS (user_data);
  int hit = overview_thumbs_drop_target_at (GTK_WIDGET (self), x, y);
  if (hit != self->hover_index || self->hover_close) {
    self->hover_index = hit;
    self->hover_close = false;
    gtk_widget_queue_draw (GTK_WIDGET (self));
  }
}

static void
winome_thumbs_leave (GtkEventControllerMotion *motion G_GNUC_UNUSED,
                     gpointer user_data)
{
  WinomeThumbs *self = WINOME_THUMBS (user_data);
  if (self->hover_index != -1 || self->hover_close) {
    self->hover_index = -1;
    self->hover_close = false;
    gtk_widget_queue_draw (GTK_WIDGET (self));
  }
}

static void
winome_thumbs_clicked (GtkGestureClick *gesture, int n_press, double x,
                       double y, gpointer user_data)
{
  (void)n_press;
  WinomeThumbs *self = WINOME_THUMBS (user_data);
  guint button =
      gtk_gesture_single_get_current_button (GTK_GESTURE_SINGLE (gesture));

  int hit = overview_thumbs_drop_target_at (GTK_WIDGET (self), x, y);
  if (hit < 0)
    return;

  if (hit < (int)self->pills.size ()) {
    if (button == GDK_BUTTON_PRIMARY && self->click_cb != nullptr)
      self->click_cb (hit, self->click_data);
  }
}

static void
winome_thumbs_dispose (GObject *object)
{
  WinomeThumbs *self = WINOME_THUMBS (object);
  g_clear_object (&self->wallpaper);
  G_OBJECT_CLASS (winome_thumbs_parent_class)->dispose (object);
}

static void
winome_thumbs_finalize (GObject *object)
{
  WinomeThumbs *self = WINOME_THUMBS (object);
  using ThumbVecT = std::vector<ThumbRect>;
  self->pills.~ThumbVecT ();
  G_OBJECT_CLASS (winome_thumbs_parent_class)->finalize (object);
}

static void
winome_thumbs_class_init (WinomeThumbsClass *klass)
{
  GTK_WIDGET_CLASS (klass)->snapshot = winome_thumbs_snapshot;
  GTK_WIDGET_CLASS (klass)->measure = winome_thumbs_measure;
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  object_class->dispose = winome_thumbs_dispose;
  object_class->finalize = winome_thumbs_finalize;
}

static void
winome_thumbs_init (WinomeThumbs *self)
{
  // g_object_new zero-fills C structs without running C++ constructors or
  // default member initializers.
  new (&self->pills) std::vector<ThumbRect> ();
  self->active = -1;
  self->hover_index = -1;
  self->drop_target = -1;
  self->indicator_value = -1.0;
  self->anim_scale = 1.0;
  self->anim_translate_y = 0.0;
  self->panel_top_ratio = 0.0;
  self->spacing = kSpacingFallback;
  self->padding = kPaddingFallback;
  self->thumb_radius = kThumbRadiusFallback;
  self->indicator_width = kIndicatorWidthFallback;
  self->indicator_radius = kIndicatorRadiusFallback;
  gtk_widget_set_overflow (GTK_WIDGET (self), GTK_OVERFLOW_VISIBLE);
  GtkEventController *motion = gtk_event_controller_motion_new ();
  g_signal_connect (motion, "enter", G_CALLBACK (winome_thumbs_motion), self);
  g_signal_connect (motion, "motion", G_CALLBACK (winome_thumbs_motion), self);
  g_signal_connect (motion, "leave", G_CALLBACK (winome_thumbs_leave), self);
  gtk_widget_add_controller (GTK_WIDGET (self), motion);

  GtkGesture *click = gtk_gesture_click_new ();
  gtk_gesture_single_set_button (GTK_GESTURE_SINGLE (click), 0);
  g_signal_connect (click, "released", G_CALLBACK (winome_thumbs_clicked),
                    self);
  gtk_widget_add_controller (GTK_WIDGET (self),
                             GTK_EVENT_CONTROLLER (click));
}

}  // namespace

GtkWidget *
overview_thumbs_new (void)
{
  return (GtkWidget *)g_object_new (winome_thumbs_get_type (), nullptr);
}

void
overview_thumbs_repopulate (GtkWidget *thumbs, int count, int active,
                            double box_h, double avail_w, double porthole_w,
                            double porthole_h, GdkTexture *wallpaper,
                            bool can_manage, bool force_show,
                            double panel_top_ratio)
{
  if (thumbs == nullptr)
    return;
  WinomeThumbs *self = WINOME_THUMBS (thumbs);
  resolve_theme (self);
  self->panel_top_ratio = std::max (panel_top_ratio, 0.0);

  const int old_w = overview_thumbs_get_width (thumbs);
  const int old_h = overview_thumbs_get_height (thumbs);

  self->pills.clear ();
  self->has_add_pill = false;
  self->add_pill = ThumbRect{};
  self->can_manage = can_manage;
  self->active = -1;
  self->hover_index = -1;
  self->hover_close = false;
  self->drop_target = -1;
  self->indicator_value = -1.0;
  self->scale = 0;

  ThumbLayoutSummary summary =
      compute_thumbs_summary (count, can_manage, force_show);
  if (!summary.visible || porthole_h <= 0 || porthole_w <= 0 || box_h <= 0) {
    g_clear_object (&self->wallpaper);
    gtk_widget_queue_draw (thumbs);
    gtk_widget_queue_resize (thumbs);
    return;
  }

  double pill_h = std::max (box_h - 2 * self->padding, 1.0);
  double add_slots = summary.has_add_pill ? 0.55 : 0.0;
  double n = (double)count + add_slots;
  double spacing_count = (double)(count - 1 + (summary.has_add_pill ? 1 : 0));
  double available_width =
      std::max (avail_w - 2 * self->padding - spacing_count * self->spacing, 1.0);
  double h_scale = (available_width / n) / porthole_w;
  double v_scale = pill_h / porthole_h;
  double scale = std::min ({h_scale, v_scale, kMaxThumbnailScale});

  double thumb_h = std::round (porthole_h * scale);
  double thumb_w = std::round (porthole_h * scale * (porthole_w / porthole_h));
  if (thumb_h < 1 || thumb_w < 1) {
    g_clear_object (&self->wallpaper);
    gtk_widget_queue_draw (thumbs);
    gtk_widget_queue_resize (thumbs);
    return;
  }

  double x = self->padding;
  double y = self->padding;
  for (int i = 0; i < count; ++i) {
    self->pills.push_back (ThumbRect{x, y, thumb_w, thumb_h});
    x += thumb_w + self->spacing;
  }

  if (summary.has_add_pill) {
    double add_w = std::max (thumb_h, 28.0);
    self->has_add_pill = true;
    self->add_pill = ThumbRect{x, y, add_w, thumb_h};
  }

  self->active = active;
  self->scale = thumb_h / porthole_h;

  g_set_object (&self->wallpaper, wallpaper);

  const int new_w = overview_thumbs_get_width (thumbs);
  const int new_h = overview_thumbs_get_height (thumbs);
  if (new_w != old_w || new_h != old_h)
    gtk_widget_queue_resize (thumbs);
  gtk_widget_queue_draw (thumbs);
}

void
overview_thumbs_set_wallpaper (GtkWidget *thumbs, GdkTexture *wallpaper)
{
  if (thumbs == nullptr)
    return;
  WinomeThumbs *self = WINOME_THUMBS (thumbs);
  if (self->wallpaper == wallpaper)
    return;
  g_set_object (&self->wallpaper, wallpaper);
  if (!self->pills.empty () && gtk_widget_get_visible (thumbs) &&
      gtk_widget_get_opacity (thumbs) > 0.01) {
    gtk_widget_queue_draw (thumbs);
  }
}

int
overview_thumbs_get_width (GtkWidget *thumbs)
{
  WinomeThumbs *self = WINOME_THUMBS (thumbs);
  if (self->pills.empty ())
    return 0;
  double right = 0;
  for (const ThumbRect &pill : self->pills)
    right = std::max (right, pill.x + pill.w);
  if (self->has_add_pill)
    right = std::max (right, self->add_pill.x + self->add_pill.w);
  return (int)(right + self->padding + 0.5);
}

int
overview_thumbs_get_height (GtkWidget *thumbs)
{
  WinomeThumbs *self = WINOME_THUMBS (thumbs);
  if (self->pills.empty ())
    return 0;
  return (int)(self->pills[0].h + 2 * self->padding + 0.5);
}

gboolean
overview_thumbs_get_rect (GtkWidget *thumbs, int index, GdkRectangle *out)
{
  WinomeThumbs *self = WINOME_THUMBS (thumbs);
  if (index < 0 || index >= (int)self->pills.size () || out == nullptr)
    return FALSE;
  const ThumbRect &pill = self->pills[index];
  out->x = (int)pill.x;
  out->y = (int)pill.y;
  out->width = (int)pill.w;
  out->height = (int)pill.h;
  return TRUE;
}

double
overview_thumbs_get_scale (GtkWidget *thumbs)
{
  return WINOME_THUMBS (thumbs)->scale;
}

// Slide the accent indicator to a continuous workspace position during the
// switch animation (ThumbnailsBox follows the eased scroll adjustment);
// pass a negative value to snap back to the discrete active pill.
void
overview_thumbs_set_indicator_value (GtkWidget *thumbs, double value)
{
  WinomeThumbs *self = WINOME_THUMBS (thumbs);
  if (std::abs (self->indicator_value - value) < 1e-4)
    return;
  self->indicator_value = value;
  gtk_widget_queue_draw (thumbs);
}

void
overview_thumbs_set_transform (GtkWidget *thumbs, double scale,
                               double translate_y)
{
  if (thumbs == nullptr)
    return;
  WinomeThumbs *self = WINOME_THUMBS (thumbs);
  const double clamped_scale = std::clamp (scale, 0.05, 2.0);
  if (std::abs (self->anim_scale - clamped_scale) < 0.002 &&
      std::abs (self->anim_translate_y - translate_y) < 0.25) {
    return;
  }
  self->anim_scale = clamped_scale;
  self->anim_translate_y = translate_y;
  gtk_widget_queue_draw (thumbs);
}

void
overview_thumbs_set_click_cb (GtkWidget *thumbs,
                              void (*cb) (int index, void *data), void *data)
{
  WinomeThumbs *self = WINOME_THUMBS (thumbs);
  self->click_cb = cb;
  self->click_data = data;
}

void
overview_thumbs_set_drop_target (GtkWidget *thumbs, int target_index)
{
  WinomeThumbs *self = WINOME_THUMBS (thumbs);
  if (self->drop_target != target_index) {
    self->drop_target = target_index;
    gtk_widget_queue_draw (thumbs);
  }
}

int
overview_thumbs_drop_target_at (GtkWidget *thumbs, double x, double y)
{
  WinomeThumbs *self = WINOME_THUMBS (thumbs);
  if (self->pills.empty ())
    return -1;

  const double v_top = self->pills[0].y - self->padding - 6.0;
  const double v_bot =
      self->pills[0].y + self->pills[0].h + self->padding + 6.0;
  if (y < v_top || y > v_bot)
    return -1;

  for (int i = 0; i < (int)self->pills.size (); ++i) {
    const ThumbRect &pill = self->pills[i];
    const double left =
        pill.x - (i == 0 ? self->padding + 10.0 : self->spacing * 0.5);
    const double right =
        pill.x + pill.w +
        (i + 1 == (int)self->pills.size () ? self->padding + 10.0
                                           : self->spacing * 0.5);
    if (x >= left && x <= right)
      return i;
  }
  return -1;
}

void
overview_thumbs_set_manage_cbs (GtkWidget *thumbs,
                                void (*on_add) (void *data),
                                void (*on_remove) (int index, void *data),
                                void *data)
{
  WinomeThumbs *self = WINOME_THUMBS (thumbs);
  self->add_cb = on_add;
  self->remove_cb = on_remove;
  self->manage_data = data;
}

}  // namespace winome
