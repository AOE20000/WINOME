/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright (C) 2026 WINOME contributors */
/*
 * st-compat-impl.c: minimal StThemeContext / StSettings / paint-state shims.
 */

#include "st-compat-impl.h"

struct _StThemeContext {
  GObject parent_instance;
  PangoFontDescription *font;
  int scale_factor;
  StTheme *theme;
};

G_DEFINE_TYPE (StThemeContext, st_theme_context, G_TYPE_OBJECT)

static void
st_theme_context_finalize (GObject *object)
{
  StThemeContext *context = ST_THEME_CONTEXT (object);
  g_clear_pointer (&context->font, pango_font_description_free);
  g_clear_object (&context->theme);
  G_OBJECT_CLASS (st_theme_context_parent_class)->finalize (object);
}

static void
st_theme_context_init (StThemeContext *context)
{
  context->font = pango_font_description_from_string ("Sans 11");
  context->scale_factor = 1;
  context->theme = NULL;
}

static void
st_theme_context_class_init (StThemeContextClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  object_class->finalize = st_theme_context_finalize;
}

StThemeContext *
st_theme_context_new (void)
{
  return g_object_new (ST_TYPE_THEME_CONTEXT, NULL);
}

void
st_theme_context_set_font (StThemeContext             *context,
                           const PangoFontDescription *font)
{
  g_return_if_fail (ST_IS_THEME_CONTEXT (context));
  g_clear_pointer (&context->font, pango_font_description_free);
  if (font)
    context->font = pango_font_description_copy (font);
}

const PangoFontDescription *
st_theme_context_get_font (StThemeContext *context)
{
  g_return_val_if_fail (ST_IS_THEME_CONTEXT (context), NULL);
  return context->font;
}

int
st_theme_context_get_scale_factor (StThemeContext *context)
{
  g_return_val_if_fail (ST_IS_THEME_CONTEXT (context), 1);
  return context->scale_factor;
}

void
st_theme_context_set_scale_factor (StThemeContext *context, int factor)
{
  g_return_if_fail (ST_IS_THEME_CONTEXT (context));
  context->scale_factor = factor;
}

double
st_theme_context_get_resolution (StThemeContext *context)
{
  g_return_val_if_fail (ST_IS_THEME_CONTEXT (context), 96.0);
  return 96.0 * context->scale_factor;
}

void
st_theme_context_get_accent_color (StThemeContext *context,
                                   CoglColor      *color,
                                   CoglColor      *fg_color)
{
  g_return_if_fail (ST_IS_THEME_CONTEXT (context));
  /* GNOME default accent color is blue #3584e4, on which the fg is white. */
  if (color)
    {
      color->red = 0x35;
      color->green = 0x84;
      color->blue = 0xe4;
      color->alpha = 0xff;
    }
  if (fg_color)
    {
      fg_color->red = 0xff;
      fg_color->green = 0xff;
      fg_color->blue = 0xff;
      fg_color->alpha = 0xff;
    }
}

StTheme *
st_theme_context_get_theme (StThemeContext *context)
{
  g_return_val_if_fail (ST_IS_THEME_CONTEXT (context), NULL);
  return context->theme;
}

void
st_theme_context_set_theme (StThemeContext *context, StTheme *theme)
{
  g_return_if_fail (ST_IS_THEME_CONTEXT (context));
  g_set_object (&context->theme, theme);
}

/* --- StSettings --- */

struct _StSettings {
  GObject parent_instance;
  double slow_down_factor;
};

G_DEFINE_TYPE (StSettings, st_settings, G_TYPE_OBJECT)

static void
st_settings_init (StSettings *settings)
{
  settings->slow_down_factor = 1.0;
}

static void
st_settings_class_init (StSettingsClass *klass)
{
}

StSettings *
st_settings_get (void)
{
  static StSettings *settings = NULL;
  if (G_UNLIKELY (settings == NULL))
    settings = g_object_new (ST_TYPE_SETTINGS, NULL);
  return settings;
}

/* --- paint-state no-ops --- */

void
st_theme_node_paint_state_init (StThemeNodePaintState *state)
{
  memset (state, 0, sizeof (*state));
}

void
st_theme_node_paint_state_free (StThemeNodePaintState *state)
{
  memset (state, 0, sizeof (*state));
}
