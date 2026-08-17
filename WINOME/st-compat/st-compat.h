/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright (C) 2026 WINOME contributors */
/*
 * st-compat.h: minimal type shims so the extracted St theme engine
 * (st-theme.c / st-theme-node.c) compiles without Clutter/Cogl, which do not
 * exist on Windows. Only the value types (colors, boxes) are provided; the
 * rendering APIs are not used by the extracted code.
 */

#pragma once

#include <glib.h>
#include <glib-object.h>

G_BEGIN_DECLS

/* --- CoglColor (value type, 4 x 8-bit RGBA) --- */
typedef struct _CoglColor {
  guint8 red;
  guint8 green;
  guint8 blue;
  guint8 alpha;
} CoglColor;

static inline gboolean
cogl_color_equal (const CoglColor *a,
                  const CoglColor *b)
{
  return a->red == b->red && a->green == b->green &&
         a->blue == b->blue && a->alpha == b->alpha;
}

/* Convert RGB 0..255 to HSL (h,s,l each in 0..1). */
static inline void
cogl_color_to_hsl (const CoglColor *color,
                   float           *hue,
                   float           *saturation,
                   float           *luminance)
{
  float r = color->red / 255.0f;
  float g = color->green / 255.0f;
  float b = color->blue / 255.0f;
  float max = MAX (MAX (r, g), b);
  float min = MIN (MIN (r, g), b);
  float h = 0.0f, s = 0.0f;
  float l = (max + min) / 2.0f;

  if (max != min)
    {
      float d = max - min;
      s = l > 0.5f ? d / (2.0f - max - min) : d / (max + min);
      if (max == r)
        h = (g - b) / d + (g < b ? 6.0f : 0.0f);
      else if (max == g)
        h = (b - r) / d + 2.0f;
      else
        h = (r - g) / d + 4.0f;
      h /= 6.0f;
    }

  if (hue)
    *hue = h;
  if (saturation)
    *saturation = s;
  if (luminance)
    *luminance = l;
}

static float
_hue_to_rgb (float p, float q, float t)
{
  if (t < 0.0f) t += 1.0f;
  if (t > 1.0f) t -= 1.0f;
  if (t < 1.0f / 6.0f) return p + (q - p) * 6.0f * t;
  if (t < 1.0f / 2.0f) return q;
  if (t < 2.0f / 3.0f) return p + (q - p) * (2.0f / 3.0f - t) * 6.0f;
  return p;
}

static inline void
cogl_color_init_from_hsl (CoglColor *color,
                          float      hue,
                          float      saturation,
                          float      luminance)
{
  float r, g, b;

  if (saturation == 0.0f)
    {
      r = g = b = luminance;
    }
  else
    {
      float q = luminance < 0.5f
        ? luminance * (1.0f + saturation)
        : luminance + saturation - luminance * saturation;
      float p = 2.0f * luminance - q;
      r = _hue_to_rgb (p, q, hue + 1.0f / 3.0f);
      g = _hue_to_rgb (p, q, hue);
      b = _hue_to_rgb (p, q, hue - 1.0f / 3.0f);
    }

  color->red   = (guint8) CLAMP (r * 255.0f, 0.0f, 255.0f);
  color->green = (guint8) CLAMP (g * 255.0f, 0.0f, 255.0f);
  color->blue  = (guint8) CLAMP (b * 255.0f, 0.0f, 255.0f);
  color->alpha = 0xff;
}

/* --- ClutterActorBox (geometry value type) --- */
typedef struct _ClutterActorBox {
  float x1;
  float y1;
  float x2;
  float y2;
} ClutterActorBox;

/* --- Opaque Clutter actor/context types (not used by the extracted code,
 *        but referenced in a few signatures we retain). --- */
typedef struct _ClutterActor ClutterActor;
typedef struct _ClutterPaintContext ClutterPaintContext;
typedef struct _ClutterPaintNode ClutterPaintNode;
typedef struct _CoglPipeline CoglPipeline;
typedef struct _CoglTexture CoglTexture;
typedef struct _CoglContext CoglContext;
typedef struct _ClutterStage ClutterStage;

G_END_DECLS
