# st-compat: extracted St CSS engine (st-theme.c / st-theme-node.c + libcroco)
# with Clutter/Cogl dependencies shimmed out so it compiles on Windows.
#
# Provides runtime querying of GNOME Shell stylesheet properties
# (icon-size, spacing, padding, colors, lengths, ...) via StTheme/StThemeNode.

cc = meson.get_compiler('c')

glib_dep = dependency('glib-2.0')
gobject_dep = dependency('gobject-2.0')
gio_dep = dependency('gio-2.0')
pango_dep = dependency('pango')

croco_sources = files(
  'croco/cr-additional-sel.c',
  'croco/cr-attr-sel.c',
  'croco/cr-cascade.c',
  'croco/cr-declaration.c',
  'croco/cr-doc-handler.c',
  'croco/cr-input.c',
  'croco/cr-num.c',
  'croco/cr-om-parser.c',
  'croco/cr-parser.c',
  'croco/cr-parsing-location.c',
  'croco/cr-pseudo.c',
  'croco/cr-rgb.c',
  'croco/cr-selector.c',
  'croco/cr-simple-sel.c',
  'croco/cr-statement.c',
  'croco/cr-string.c',
  'croco/cr-stylesheet.c',
  'croco/cr-term.c',
  'croco/cr-tknzr.c',
  'croco/cr-token.c',
  'croco/cr-utils.c',
)

st_sources = files(
  'st-theme.c',
  'st-theme-node.c',
  'st-shadow.c',
  'st-border-image.c',
  'st-icon-colors.c',
  'st-compat-impl.c',
)

stcompat_inc = include_directories('.', 'croco')

libstcompat = static_library('stcompat',
  [croco_sources, st_sources],
  include_directories: stcompat_inc,
  dependencies: [glib_dep, gobject_dep, gio_dep, pango_dep],
  c_args: [
    '-DST_COMPILATION',
    '-DG_LOG_DOMAIN="St"',
    '-Wno-unused-parameter',
    '-Wno-deprecated-declarations',
  ],
)

stcompat_dep = declare_dependency(
  link_with: libstcompat,
  include_directories: stcompat_inc,
  dependencies: [glib_dep, gobject_dep, gio_dep, pango_dep],
)

# Build-time helper: evaluate a St color expression to a hex string.
# Used by tools/convert-st-css.py to reuse St's exact color math.
st_css_eval = executable('st-css-eval',
  '../tools/st-css-eval.c',
  include_directories: stcompat_inc,
  dependencies: [glib_dep, gobject_dep, gio_dep, pango_dep],
  link_with: libstcompat,
  c_args: ['-DST_COMPILATION'],
)
