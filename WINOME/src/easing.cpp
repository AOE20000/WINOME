// SPDX-License-Identifier: GPL-2.0-only
// Copyright (C) 2026 WINOME contributors
// Easing curves and a tiny tween runner, matching Clutter's easing functions.

#include "easing.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include <functional>

// Everything below lives in winome so it can name Ease/TweenTarget from the
// header; only the bezier math and the tick handler stay in an inner namespace.
namespace winome {
namespace {

// A normalised cubic bezier through (0,0), (x1,y1), (x2,y2), (1,1) — the same
// parametrisation CSS transitions and Clutter's easing tables use.
struct Bezier {
  double x1, y1, x2, y2;
};

// Clutter's curves (clutter-easing.c). ease_out_cubic is what GNOME uses for
// virtually every popup, so it is the default everywhere.
constexpr Bezier kLinear = {0.0, 0.0, 1.0, 1.0};
constexpr Bezier kEaseInQuad = {0.55, 0.085, 0.68, 0.53};
constexpr Bezier kEaseOutQuad = {0.25, 0.46, 0.45, 0.94};
constexpr Bezier kEaseInOutQuad = {0.42, 0.0, 0.58, 1.0};
constexpr Bezier kEaseInCubic = {0.55, 0.055, 0.675, 0.19};
constexpr Bezier kEaseOutCubic = {0.215, 0.61, 0.355, 1.0};
constexpr Bezier kEaseInOutCubic = {0.645, 0.045, 0.355, 1.0};
constexpr Bezier kEaseOutQuart = {0.165, 0.84, 0.44, 1.0};
// ease_out_back overshoots slightly, which is what gives GNOME's menus their
// small "settle" as they open.
constexpr Bezier kEaseOutBack = {0.34, 1.56, 0.64, 1.0};

// ease_out_bounce is piecewise rather than a single bezier; Clutter
// approximates it with this one, so reuse the same shape.
constexpr Bezier kEaseOutBounce = {0.34, 1.56, 0.64, 1.0};

double
bezier_axis (double t, double a, double b)
{
  // B(t) for a component, given the two control values.
  const double inv = 1.0 - t;
  return 3.0 * inv * inv * t * a + 3.0 * inv * t * t * b + t * t * t;
}

double
bezier_axis_derivative (double t, double a, double b)
{
  const double inv = 1.0 - t;
  return 3.0 * inv * inv * a + 6.0 * inv * t * (b - a) + 3.0 * t * t * (1.0 - b);
}

// Solve the parametric curve for the y value at a given x. Newton-Raphson with
// a bisection fallback — the standard approach used by browsers for
// cubic-bezier() timing functions.
double
bezier_solve (const Bezier &b, double x)
{
  if (x <= 0.0)
    return 0.0;
  if (x >= 1.0)
    return 1.0;

  double t = x;
  for (int i = 0; i < 8; i++) {
    const double err = bezier_axis (t, b.x1, b.x2) - x;
    if (fabs (err) < 1e-6)
      return bezier_axis (t, b.y1, b.y2);
    const double d = bezier_axis_derivative (t, b.x1, b.x2);
    if (fabs (d) < 1e-6)
      break;
    t -= err / d;
  }

  // Newton stalled (flat derivative, e.g. ease_out_back): bisect instead.
  double lo = 0.0, hi = 1.0;
  t = x;
  for (int i = 0; i < 24; i++) {
    const double cur = bezier_axis (t, b.x1, b.x2);
    if (fabs (cur - x) < 1e-6)
      break;
    if (cur < x)
      lo = t;
    else
      hi = t;
    t = (lo + hi) * 0.5;
  }
  return bezier_axis (t, b.y1, b.y2);
}

// Per-tween state. Freed by the tick callback when the tween ends, so a widget
// that is destroyed mid-animation simply drops the callback with its closure.
struct TweenState {
  GtkWidget *widget = nullptr;
  guint tick_id = 0;
  TweenTarget target;
  int duration_us = 0;
  Ease ease = Ease::ease_out_cubic;
  gint64 start_us = 0;
  std::function<void()> on_complete;
};

static const char *
target_key (TweenTarget::Kind kind)
{
  switch (kind) {
  case TweenTarget::Kind::opacity:
    return "winome-tween-opacity";
  case TweenTarget::Kind::width:
    return "winome-tween-width";
  case TweenTarget::Kind::height:
    return "winome-tween-height";
  }
  return "winome-tween-unknown";
}

static void
tween_state_destroy (gpointer data)
{
  delete static_cast<TweenState *> (data);
}

void
apply_value (GtkWidget *widget, const TweenTarget &target, double value)
{
  switch (target.kind) {
  case TweenTarget::Kind::opacity:
    if (std::fabs (gtk_widget_get_opacity (widget) - value) > 1e-4)
      gtk_widget_set_opacity (widget, value);
    break;
  case TweenTarget::Kind::width: {
    const int want = (int) (value + 0.5);
    int cur_w = -1, cur_h = -1;
    gtk_widget_get_size_request (widget, &cur_w, &cur_h);
    if (cur_w != want)
      gtk_widget_set_size_request (widget, want, -1);
    break;
  }
  case TweenTarget::Kind::height: {
    const int want = (int) (value + 0.5);
    int cur_w = -1, cur_h = -1;
    gtk_widget_get_size_request (widget, &cur_w, &cur_h);
    if (cur_h != want)
      gtk_widget_set_size_request (widget, -1, want);
    break;
  }
  }
}

gboolean
on_tick (GtkWidget *widget, GdkFrameClock *clock, gpointer data)
{
  auto *st = static_cast<TweenState *> (data);
  if (st->widget == nullptr)
    return G_SOURCE_REMOVE;

  g_object_ref (widget);

  const gint64 now = clock != nullptr ? gdk_frame_clock_get_frame_time (clock)
                                      : g_get_monotonic_time ();
  if (st->start_us == 0)
    st->start_us = now;

  double progress = 1.0;
  if (st->duration_us > 0) {
    progress = (double) (now - st->start_us) / (double) st->duration_us;
  }
  if (progress >= 1.0)
    progress = 1.0;

  if (progress < 1.0) {
    const double eased = ease_sample (st->ease, progress);
    apply_value (widget, st->target,
                 st->target.from + (st->target.to - st->target.from) * eased);
    g_object_unref (widget);
    return G_SOURCE_CONTINUE;
  }

  // Finished: apply the exact target (so no rounding residue) then detach.
  // Move on_complete out before running it because G_SOURCE_REMOVE invokes the
  // tick callback's GDestroyNotify, and cb() itself may destroy widget early.
  apply_value (widget, st->target, st->target.to);
  const char *key = target_key (st->target.kind);
  if (GPOINTER_TO_UINT (g_object_get_data (G_OBJECT (widget), key)) == st->tick_id)
    g_object_set_data (G_OBJECT (widget), key, nullptr);
  st->widget = nullptr;
  auto cb = std::move (st->on_complete);
  if (cb)
    cb ();
  g_object_unref (widget);
  return G_SOURCE_REMOVE;
}

}  // namespace

// }  // namespace winome

double
ease_sample (Ease ease, double t)
{
  if (t <= 0.0)
    return 0.0;
  if (t >= 1.0)
    return 1.0;

  Bezier b;
  switch (ease) {
  case Ease::linear:            b = kLinear; break;
  case Ease::ease_in_quad:      b = kEaseInQuad; break;
  case Ease::ease_out_quad:     b = kEaseOutQuad; break;
  case Ease::ease_in_out_quad:  b = kEaseInOutQuad; break;
  case Ease::ease_in_cubic:     b = kEaseInCubic; break;
  case Ease::ease_out_cubic:    b = kEaseOutCubic; break;
  case Ease::ease_in_out_cubic: b = kEaseInOutCubic; break;
  case Ease::ease_out_quart:    b = kEaseOutQuart; break;
  case Ease::ease_out_back:     b = kEaseOutBack; break;
  case Ease::ease_out_bounce:   b = kEaseOutBounce; break;
  default:                      b = kEaseOutCubic; break;
  }

  return bezier_solve (b, t);
}

void
tween_property (GtkWidget *widget, const TweenTarget &target,
                int duration_ms, Ease ease, std::function<void()> on_complete)
{
  if (widget == nullptr)
    return;

  // Stop any tween already driving this widget's target so they do not fight.
  // The frame-clock callback is removed with the widget, so a detached widget
  // cannot outlive its tween.
  const char *key = target_key (target.kind);
  guint prev_id = GPOINTER_TO_UINT (g_object_get_data (G_OBJECT (widget), key));
  if (prev_id != 0) {
    gtk_widget_remove_tick_callback (widget, prev_id);
    g_object_set_data (G_OBJECT (widget), key, nullptr);
  }

  auto *st = new TweenState ();
  st->widget = widget;
  st->target = target;
  st->duration_us = duration_ms * 1000;
  st->ease = ease;
  st->on_complete = std::move (on_complete);

  guint id = gtk_widget_add_tick_callback (widget, on_tick, st, tween_state_destroy);
  st->tick_id = id;
  g_object_set_data (G_OBJECT (widget), key, GUINT_TO_POINTER (id));
}

void
tween_opacity (GtkWidget *widget, double from, double to, int duration_ms,
               Ease ease, std::function<void()> on_complete)
{
  TweenTarget t;
  t.kind = TweenTarget::Kind::opacity;
  t.from = from;
  t.to = to;
  tween_property (widget, t, duration_ms, ease, std::move (on_complete));
}

void
tween_height (GtkWidget *widget, double from, double to, int duration_ms,
              Ease ease, std::function<void()> on_complete)
{
  TweenTarget t;
  t.kind = TweenTarget::Kind::height;
  t.from = from;
  t.to = to;
  tween_property (widget, t, duration_ms, ease, std::move (on_complete));
}

}  // namespace winome
