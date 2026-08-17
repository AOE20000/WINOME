# GNOME Shell theme handling.
#
# Pipeline:
#   1. gnome-shell-dark.css (committed) is the sassc-compiled GNOME theme.
#      It is packed into GResource as-is for the St CSS engine (st-compat) to
#      parse at runtime, giving exact St property values (icon-size, spacing,
#      -natural-hpadding, ...).
#   2. convert-st-css.py -> gnome-shell-gtk4.css (GTK4-compatible, drops or
#      maps St-specific properties). This is what GTK4's GtkCssProvider loads.
#
# Both files are placed in the build dir so a single gresource source_dir can
# reference them.

python = find_program('python3')

# Copy the raw GNOME stylesheet into the build dir for GResource.
gnome_shell_dark_css = custom_target('gnome-shell-dark.css',
  input: files('gnome-shell-dark.css'),
  output: 'gnome-shell-dark.css',
  command: [python, '-c', 'import shutil,sys; shutil.copyfile(sys.argv[1], sys.argv[2])', '@INPUT@', '@OUTPUT@'],
  build_by_default: true,
)

gnome_shell_gtk4_css = custom_target('gnome-shell-gtk4.css',
  input: files('gnome-shell-dark.css'),
  output: 'gnome-shell-gtk4.css',
  command: [
    python, meson.source_root() / 'tools' / 'convert-st-css.py',
    '@INPUT@', '@OUTPUT@',
  ],
  env: {'ST_CSS_EVAL': st_css_eval.full_path()},
  depends: [st_css_eval],
  depend_files: [meson.source_root() / 'tools' / 'convert-st-css.py'],
  build_by_default: true,
)
