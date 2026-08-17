/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright (C) 2026 WINOME contributors */
/*
 * st-css-eval: build-time tool that evaluates a St CSS color expression
 * (e.g. "st-mix(#ffffff, #36363a, 9%)") into a hex color, using the exact
 * St color math from st-theme-node.c (via st_compat_color_from_string).
 *
 * Usage: st-css-eval <color-expression>
 * Prints the result as #RRGGBB or rgba(...) on stdout.
 */

#include <stdio.h>
#include <string.h>

#include "st-compat-impl.h"
#include "st-theme-node.h"

int
main (int argc, char **argv)
{
  StThemeContext *context;
  StThemeNode *node;
  CoglColor color;

  if (argc < 2)
    {
      fprintf (stderr, "usage: %s <color-expression>\n", argv[0]);
      return 1;
    }

  /* A context + node is needed to resolve -st-accent-color. */
  context = st_theme_context_new ();
  node = st_theme_node_new (context, NULL, NULL, G_TYPE_OBJECT,
                            NULL, NULL, NULL, NULL);

  if (!st_compat_color_from_string (node, argv[1], &color))
    {
      /* Not a color expression; echo the input unchanged so callers can
       * pass non-color values through untouched. */
      printf ("%s\n", argv[1]);
      g_object_unref (node);
      g_object_unref (context);
      return 0;
    }

  if (color.alpha == 0xff)
    printf ("#%02x%02x%02x\n", color.red, color.green, color.blue);
  else
    printf ("rgba(%d, %d, %d, %.3f)\n",
            color.red, color.green, color.blue, color.alpha / 255.0);

  g_object_unref (node);
  g_object_unref (context);
  return 0;
}
