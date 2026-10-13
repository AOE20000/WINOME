// SPDX-License-Identifier: GPL-2.0-only
// Copyright (C) 2026 WINOME contributors
// Easing curves and a tiny tween runner, matching Clutter's easing functions.
//
// GNOME animates through Clutter, whose `Clutter.EaseFunction`s are cubic
// beziers defined in gjs (Clutter's `ease_out_cubic` etc). Every GNOME shell
// animation — including the quick-settings menus' two-phase open/close —
// routes through them, so reproducing the curves is what makes the motion read
// as GNOME rather than as an approximation.
//
// GTK4 ships no animation framework, so this provides the missing half: a
// frame-clock-driven runner that samples a curve and applies the result to a
// widget property. Timing follows the same discipline as the rest of the
// project: driven by gtk_widget_add_tick_callback so samples align with
// vblank, never by a 16ms timer.

#pragma once

#include <gtk/gtk.h>

#include <functional>

namespace winome {

// Clutter's named easing curves, as normalised cubic beziers
// (x1, y1, x2, y2). These are the exact values Clutter registers
// (clutter-easing.c); GNOME picks between them by name.
enum class Ease {
  linear,
  ease_in_quad,
  ease_out_quad,
  ease_in_out_quad,
  ease_out_cubic,
  ease_in_cubic,
  ease_in_out_cubic,
  ease_out_quart,
  ease_out_back,
  ease_out_bounce,
};

// Sample @ease at @t (0..1) and return the eased value. Exposed for tests and
// for callers that drive their own frame clock.
double ease_sample (Ease ease, double t);

// Duration constants lifted from gnome-shell so timings match:
//   js/ui/quickSettings.js:17   POPUP_ANIMATION_TIME = 400  (each phase half)
//   js/ui/boxpointer.js:14      POPUP_ANIMATION_TIME = 150
enum {
  QUICK_SETTINGS_ANIMATION_TIME = 400,
  BOXPOPOVER_ANIMATION_TIME = 150,
};

// What a tween animates. Widget properties are applied through their GTK
// setters; opacity maps to the "opacity" CSS property because GTK4 has no
// dedicated widget opacity API.
struct TweenTarget {
  enum class Kind {
    opacity,     // 0.0 .. 1.0
    width,       // logical px
    height,      // logical px
  };

  Kind kind = Kind::opacity;
  double from = 0.0;
  double to = 0.0;
};

// Run a single-property tween on @widget over @duration_ms, sampling @ease
// once per frame. The tween owns a tick callback for its lifetime and removes
// it on completion, so the caller only keeps the widget alive.
//
// Used for the quick-settings menu open/close, where upstream runs two phases
// back to back (height, then opacity). Chain them by passing a callback that
// starts the second tween.
void tween_property (GtkWidget *widget, const TweenTarget &target,
                     int duration_ms, Ease ease,
                     std::function<void()> on_complete = nullptr);

// Convenience wrappers matching GNOME's `actor.ease({...})` call shape.
void tween_opacity (GtkWidget *widget, double from, double to,
                    int duration_ms, Ease ease = Ease::ease_out_cubic,
                    std::function<void()> on_complete = nullptr);

void tween_height (GtkWidget *widget, double from, double to,
                   int duration_ms, Ease ease = Ease::ease_out_cubic,
                   std::function<void()> on_complete = nullptr);

}  // namespace winome
