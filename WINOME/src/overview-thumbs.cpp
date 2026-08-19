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
#include "st-engine.h"

#include <algorithm>
#include <vector>

namespace winome {

namespace {

// upstream MAX_THUMBNAIL_SCALE (also enforced by the caller's box_h).
constexpr double kMaxThumbnailScale = 0.05;
constexpr double kSpacingFallback = 6.0;    // .workspace-thumbnails spacing
constexpr double kPaddingFallback = 6.0;    // .workspace-thumbnails padding
constexpr double kThumbRadiusFallback = 4.0;
// .workspace-thumbnail-indicator: border shorthand values are not directly
// queryable through the St engine; the accent color matches the fixed
// -st-accent-color default used elsewhere in WINOME.
constexpr double kIndicatorWidthFallback = 3.0;
constexpr double kIndicatorRadiusFallback = 8.0;

struct ThumbRect {
  double x, y, w, h;
};

struct WinomeThumbs {
  GtkWidget parent_instance;

  std::vector<ThumbRect> pills;
  int active = -1;
  double scale = 0;  // pill px per porthole px

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
    self->spacing = v;
  if (box.lookup_length ("padding", &v) && v >= 0)
    self->padding = v;

  StEngine::Node thumb (*engine, &box, "", "workspace-thumbnail", "");
  std::string color;
  if (thumb.lookup_color ("background-color", &color))
    gdk_rgba_parse (&self->pill_color, color.c_str ());
  if (thumb.lookup_length ("border-radius", &v) && v > 0)
    self->thumb_radius = v;
}

// --- snapshot ------------------------------------------------------------------

static void
winome_thumbs_snapshot (GtkWidget *widget, GtkSnapshot *snapshot)
{
  WinomeThumbs *self = WINOME_THUMBS (widget);
  resolve_theme (self);

  if (self->pills.empty ())
    return;

  // .workspace-thumbnail pills: the wallpaper drawn cover-style (the porthole
  // background) inside a 4px rounded clip.
  for (const ThumbRect &pill : self->pills) {
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
        double s = std::max (pill.w / tw, pill.h / th);
        double dw = tw * s, dh = th * s;
        graphene_rect_t tex_rect;
        graphene_rect_init (&tex_rect,
                            (float)(pill.x + (pill.w - dw) / 2.0),
                            (float)(pill.y + (pill.h - dh) / 2.0),
                            (float)dw, (float)dh);
        gtk_snapshot_append_texture (snapshot, self->wallpaper, &tex_rect);
        drawn = true;
      }
    }
    if (!drawn)
      gtk_snapshot_append_color (snapshot, &self->pill_color, &rect);

    gtk_snapshot_pop (snapshot);
  }

  // .workspace-thumbnail-indicator: 3px accent frame allocated AROUND the
  // active pill (inflated by the border widths, radius 8).
  if (self->active >= 0 && self->active < (int)self->pills.size ()) {
    const ThumbRect &pill = self->pills[self->active];
    double b = self->indicator_width;
    graphene_rect_t rect;
    graphene_rect_init (&rect, (float)(pill.x - b), (float)(pill.y - b),
                        (float)(pill.w + 2 * b), (float)(pill.h + 2 * b));
    GskRoundedRect outline;
    gsk_rounded_rect_init_from_rect (&outline, &rect,
                                     (float)self->indicator_radius);
    float widths[4] = {(float)b, (float)b, (float)b, (float)b};
    GdkRGBA colors[4] = {self->indicator_color, self->indicator_color,
                         self->indicator_color, self->indicator_color};
    gtk_snapshot_append_border (snapshot, &outline, widths, colors);
  }
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
    w = (int)(right + self->padding + 0.5);
    h = (int)(self->pills[0].h + 2 * self->padding + 0.5);
  }

  if (orientation == GTK_ORIENTATION_HORIZONTAL) {
    *minimum = *natural = w;
  } else {
    *minimum = *natural = h;
  }
}

// --- clicks ------------------------------------------------------------------------

static void
winome_thumbs_clicked (GtkGestureClick *gesture, int n_press, double x,
                       double y, gpointer user_data)
{
  (void)gesture;
  (void)n_press;
  WinomeThumbs *self = WINOME_THUMBS (user_data);

  if (self->click_cb == nullptr)
    return;

  for (int i = 0; i < (int)self->pills.size (); ++i) {
    const ThumbRect &pill = self->pills[i];
    if (x >= pill.x && x <= pill.x + pill.w && y >= pill.y &&
        y <= pill.y + pill.h) {
      self->click_cb (i, self->click_data);
      return;
    }
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
winome_thumbs_class_init (WinomeThumbsClass *klass)
{
  GTK_WIDGET_CLASS (klass)->snapshot = winome_thumbs_snapshot;
  GTK_WIDGET_CLASS (klass)->measure = winome_thumbs_measure;
  G_OBJECT_CLASS (klass)->dispose = winome_thumbs_dispose;
}

static void
winome_thumbs_init (WinomeThumbs *self)
{
  GtkGesture *click = gtk_gesture_click_new ();
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
                            double porthole_h, GdkTexture *wallpaper)
{
  WinomeThumbs *self = WINOME_THUMBS (thumbs);
  resolve_theme (self);

  self->pills.clear ();
  self->active = -1;
  self->scale = 0;

  // GNOME hides the box for a single workspace (_updateShouldShow).
  if (count <= 1 || porthole_h <= 0 || porthole_w <= 0 || box_h <= 0) {
    gtk_widget_queue_resize (thumbs);
    return;
  }

  // ThumbnailsBox.vfunc_allocate: scale = min(hScale, vScale,
  // MAX_THUMBNAIL_SCALE); the pill keeps the porthole aspect.
  double pill_h = std::max (box_h - 2 * self->padding, 1.0);
  double n = (double)count;
  double available_width =
      std::max (avail_w - 2 * self->padding - (n - 1) * self->spacing, 1.0);
  double h_scale = (available_width / n) / porthole_w;
  double v_scale = pill_h / porthole_h;
  double scale = std::min ({h_scale, v_scale, kMaxThumbnailScale});

  double thumb_h = std::round (porthole_h * scale);
  double thumb_w = std::round (porthole_h * scale * (porthole_w / porthole_h));
  if (thumb_h < 1 || thumb_w < 1) {
    gtk_widget_queue_resize (thumbs);
    return;
  }

  double x = self->padding;
  double y = self->padding;
  for (int i = 0; i < count; ++i) {
    self->pills.push_back (ThumbRect{x, y, thumb_w, thumb_h});
    x += thumb_w + self->spacing;
  }

  self->active = active;
  self->scale = thumb_h / porthole_h;

  g_clear_object (&self->wallpaper);
  if (wallpaper != nullptr) {
    self->wallpaper = wallpaper;
    g_object_ref (self->wallpaper);
  }

  gtk_widget_queue_resize (thumbs);
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

void
overview_thumbs_set_click_cb (GtkWidget *thumbs,
                              void (*cb) (int index, void *data), void *data)
{
  WinomeThumbs *self = WINOME_THUMBS (thumbs);
  self->click_cb = cb;
  self->click_data = data;
}

}  // namespace winome
