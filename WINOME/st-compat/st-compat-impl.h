/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright (C) 2026 WINOME contributors */
/*
 * st-compat-impl.h: minimal StThemeContext / StSettings / paint-state shims
 * for the Windows extraction. These replace the full GNOME implementations
 * (which need Clutter/GSettings/gnome-desktop) with fixed sensible values.
 */

#pragma once

#include <glib-object.h>
#include <pango/pango.h>

#include "st-compat.h"
#include "st-theme-node.h"

G_BEGIN_DECLS

/* StThemeContext: opaque; we only need the getters below. */
#define ST_TYPE_THEME_CONTEXT (st_theme_context_get_type ())
G_DECLARE_FINAL_TYPE (StThemeContext, st_theme_context,
                      ST, THEME_CONTEXT, GObject)

StThemeContext *st_theme_context_new (void);
void            st_theme_context_set_font (StThemeContext             *context,
                                           const PangoFontDescription *font);
const PangoFontDescription *st_theme_context_get_font (StThemeContext *context);
int   st_theme_context_get_scale_factor (StThemeContext *context);
void  st_theme_context_set_scale_factor (StThemeContext *context, int factor);
double st_theme_context_get_resolution (StThemeContext *context);
void  st_theme_context_get_accent_color (StThemeContext *context,
                                         CoglColor      *color,
                                         CoglColor      *fg_color);
StTheme *st_theme_context_get_theme (StThemeContext *context);
void     st_theme_context_set_theme (StThemeContext *context, StTheme *theme);

/* StSettings: minimal (slow-down-factor only). */
#define ST_TYPE_SETTINGS (st_settings_get_type ())
G_DECLARE_FINAL_TYPE (StSettings, st_settings, ST, SETTINGS, GObject)
StSettings *st_settings_get (void);

/* Paint-state no-ops (rendering is done by GTK4). */
void st_theme_node_paint_state_init (StThemeNodePaintState *state);
void st_theme_node_paint_state_free (StThemeNodePaintState *state);

G_END_DECLS
