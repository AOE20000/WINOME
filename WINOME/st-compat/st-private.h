/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright (C) 2026 WINOME contributors */
/*
 * st-private.h: minimal private declarations for the Windows extraction.
 * The full version pulls in St widget types (st-widget.h/st-bin.h) and
 * Clutter rendering; the extracted theme engine only needs these macros.
 */

#pragma once

#include <glib.h>

#include "st-shadow.h"

G_BEGIN_DECLS

#define I_(str)         (g_intern_static_string ((str)))

#define ST_PARAM_READABLE  (G_PARAM_READABLE  | G_PARAM_STATIC_STRINGS)
#define ST_PARAM_WRITABLE  (G_PARAM_WRITABLE  | G_PARAM_STATIC_STRINGS)
#define ST_PARAM_READWRITE (G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS)

G_END_DECLS
