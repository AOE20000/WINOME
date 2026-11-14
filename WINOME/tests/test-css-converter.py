#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
# Copyright (C) 2026 WINOME contributors
# Regression tests for tools/convert-st-css.py.

import os
import subprocess
import sys
import tempfile


def main():
    if len(sys.argv) < 2:
        print('usage: test-css-converter.py <path/to/convert-st-css.py>')
        return 1

    converter = sys.argv[1]
    sample_st = """
.quick-toggle {
  width: 12em;
  height: 44px;
  max-width: 12em;
  -natural-hpadding: 12px;
  -st-hfade-offset: 24px;
  transition-duration: 150ms;
}

.toggle-btn:insensitive {
  color: rgba(255, 255, 255, 0.5);
}

.weird:rtl {
  padding: 4px;
}

StBoxLayout.foo StLabel {
  font-size: 11pt;
}

.datemenu-calendar-column:ltr, .datemenu-calendar-column.rtl-drop:rtl {
  margin-left: 6px;
}

.overview-tile:highlighted, .overview-tile:selected {
  background-color: rgba(255, 255, 255, 0.15);
}

.entry :ltr {
  padding-right: 2.5em;
}

.multi-keep:ltr,
.multi-drop:rtl {
  margin-right: 4px;
}
"""

    with tempfile.TemporaryDirectory() as td:
        in_css = os.path.join(td, 'in.css')
        out_css = os.path.join(td, 'out.css')
        with open(in_css, 'w', encoding='utf-8') as f:
            f.write(sample_st)

        res = subprocess.run([sys.executable, converter, in_css, out_css],
                             capture_output=True, text=True)
        assert res.returncode == 0, f'converter failed: {res.stderr}'

        with open(out_css, 'r', encoding='utf-8') as f:
            out = f.read()

    assert 'min-width: 12em;' in out, out
    assert 'min-height: 44px;' in out, out
    assert 'max-width:' not in out, out
    assert 'max-height:' not in out, out
    assert 'padding-left: 12px;' in out and 'padding-right: 12px;' in out, out
    assert '-st-hfade-offset' not in out, out
    assert '.weird' not in out, out
    assert 'transition-property: background-color, color, border-color, box-shadow;' in out, out
    assert '.toggle-btn:disabled' in out, out
    assert ':insensitive' not in out, out
    assert 'box.foo label' in out, out
    out_body = (out.split('\n#panel .panel-button')[0]
                if '\n#panel .panel-button' in out else out)
    assert '.datemenu-calendar-column {' in out_body, out_body
    assert ':ltr' not in out_body, out_body
    assert '.datemenu-calendar-column.rtl-drop' not in out_body, out_body
    assert '.overview-tile:selected' in out_body, out_body
    assert ':highlighted' not in out_body, out_body
    assert '.entry {' not in out_body, out_body
    assert '.multi-keep {' in out_body and '.multi-drop' not in out_body, out_body
    assert out.count('{') == out.count('}'), (
        f"unbalanced braces: {out.count('{')} vs {out.count('}')}")

    verify_full_theme_with_gtk4(converter)
    print('CSS converter regression tests passed.')
    return 0


def _load_gtk4_dlls():
    import ctypes
    for entry in [os.path.dirname(sys.executable)] + os.environ.get('PATH', '').split(os.pathsep):
        if entry and os.path.isfile(os.path.join(entry, 'libgtk-4-1.dll')):
            if hasattr(os, 'add_dll_directory'):
                os.add_dll_directory(entry)
            break
    gtk = ctypes.CDLL('libgtk-4-1.dll')
    gio = ctypes.CDLL('libgio-2.0-0.dll')
    gdk = ctypes.CDLL('libgtk-4-1.dll')
    gobject = ctypes.CDLL('libgobject-2.0-0.dll')
    return ctypes, gtk, gdk, gio, gobject


def verify_full_theme_with_gtk4(converter):
    import shutil
    import xml.etree.ElementTree as ET
    repo_root = os.path.dirname(os.path.dirname(os.path.abspath(converter)))
    theme_dir = os.path.join(repo_root, 'src', 'theme')
    full_st = os.path.join(theme_dir, 'gnome-shell-dark.css')
    assert os.path.isfile(full_st), f'missing stylesheet: {full_st}'

    with tempfile.TemporaryDirectory() as td:
        full_gtk4 = os.path.join(td, 'gnome-shell-gtk4.css')
        res = subprocess.run([sys.executable, converter, full_st, full_gtk4],
                             capture_output=True, text=True)
        assert res.returncode == 0, f'full conversion failed: {res.stderr}'

        ctypes, gtk, gdk, gio, gobject = _load_gtk4_dlls()
        gtk.gtk_init_check.argtypes = []
        gtk.gtk_init_check.restype = ctypes.c_int
        gtk.gtk_css_provider_new.argtypes = []
        gtk.gtk_css_provider_new.restype = ctypes.c_void_p
        gtk.gtk_css_provider_load_from_path.argtypes = [
            ctypes.c_void_p, ctypes.c_char_p]
        gtk.gtk_css_provider_load_from_path.restype = None
        gobject.g_signal_connect_data.argtypes = [
            ctypes.c_void_p, ctypes.c_char_p, ctypes.c_void_p,
            ctypes.c_void_p, ctypes.c_void_p, ctypes.c_int]
        gobject.g_signal_connect_data.restype = ctypes.c_ulong
        gobject.g_object_unref.argtypes = [ctypes.c_void_p]
        gobject.g_object_unref.restype = None
        gdk.gdk_display_get_default.argtypes = []
        gdk.gdk_display_get_default.restype = ctypes.c_void_p
        gtk.gtk_icon_theme_get_for_display.argtypes = [ctypes.c_void_p]
        gtk.gtk_icon_theme_get_for_display.restype = ctypes.c_void_p
        gtk.gtk_icon_theme_add_resource_path.argtypes = [
            ctypes.c_void_p, ctypes.c_char_p]
        gtk.gtk_icon_theme_add_resource_path.restype = None
        gtk.gtk_icon_theme_has_icon.argtypes = [ctypes.c_void_p, ctypes.c_char_p]
        gtk.gtk_icon_theme_has_icon.restype = ctypes.c_int
        gio.g_resource_load.argtypes = [ctypes.c_char_p, ctypes.c_void_p]
        gio.g_resource_load.restype = ctypes.c_void_p
        gio.g_resources_register.argtypes = [ctypes.c_void_p]
        gio.g_resources_register.restype = None
        gio.g_resources_unregister.argtypes = [ctypes.c_void_p]
        gio.g_resources_unregister.restype = None
        gio.g_resource_unref.argtypes = [ctypes.c_void_p]
        gio.g_resource_unref.restype = None

        if not gtk.gtk_init_check():
            raise AssertionError('gtk_init_check() failed during CSS/icon test')

        class GError(ctypes.Structure):
            _fields_ = [
                ('domain', ctypes.c_uint32),
                ('code', ctypes.c_int),
                ('message', ctypes.c_char_p),
            ]

        prov = gtk.gtk_css_provider_new()
        errors = []

        @ctypes.CFUNCTYPE(None, ctypes.c_void_p, ctypes.c_void_p,
                          ctypes.c_void_p, ctypes.c_void_p)
        def on_parsing_error(_prov, _section, gerror_ptr, _ud):
            err = ctypes.cast(gerror_ptr, ctypes.POINTER(GError)).contents
            errors.append(err.message.decode('utf-8', 'replace'))

        gobject.g_signal_connect_data(
            prov, b'parsing-error', on_parsing_error, None, None, 0)
        gtk.gtk_css_provider_load_from_path(prov, full_gtk4.encode('utf-8'))
        gobject.g_object_unref(prov)
        assert not errors, f'GTK4 CSS parse errors ({len(errors)}): {errors[:10]}'

        gresource_xml = os.path.join(theme_dir, 'winome.gresource.xml')
        tree = ET.parse(gresource_xml)
        bundled_icons = []
        icon_xml_lines = [
            '<?xml version="1.0" encoding="UTF-8"?>',
            '<gresources>',
            '  <gresource prefix="/org/winome/theme">',
        ]
        for elem in tree.findall('.//file'):
            rel = (elem.text or '').strip()
            assert rel, 'empty <file> entry in winome.gresource.xml'
            if rel.startswith('icons/'):
                disk_path = os.path.join(theme_dir, rel)
                assert os.path.isfile(disk_path), f'missing SVG: {disk_path}'
                name = os.path.basename(rel)
                assert name.endswith('.svg'), f'unexpected icon file: {rel}'
                bundled_icons.append(name[:-4])
                icon_xml_lines.append(f'    <file>{rel}</file>')
        icon_xml_lines.extend(['  </gresource>', '</gresources>'])
        assert bundled_icons, 'expected bundled icons in winome.gresource.xml'

        icons_xml = os.path.join(td, 'icons.gresource.xml')
        icons_bin = os.path.join(td, 'icons.gresource')
        with open(icons_xml, 'w', encoding='utf-8') as f:
            f.write('\n'.join(icon_xml_lines))
        glib_comp = shutil.which('glib-compile-resources') or os.path.join(
            os.path.dirname(sys.executable), 'glib-compile-resources')
        cres = subprocess.run(
            [glib_comp, icons_xml, f'--sourcedir={theme_dir}', f'--target={icons_bin}'],
            capture_output=True, text=True)
        assert cres.returncode == 0, f'glib-compile-resources failed: {cres.stderr}'

        gres = gio.g_resource_load(icons_bin.encode('utf-8'), None)
        assert gres, f'g_resource_load failed for {icons_bin}'
        gio.g_resources_register(gres)
        try:
            display = gdk.gdk_display_get_default()
            assert display, 'missing default GdkDisplay'
            icon_theme = gtk.gtk_icon_theme_get_for_display(display)
            gtk.gtk_icon_theme_add_resource_path(
                icon_theme, b'/org/winome/theme/icons')

            required_stock_icons = [
                'power-profile-power-saver-symbolic',
                'network-wireless-encrypted-symbolic',
                'go-next-symbolic',
                'preferences-system-symbolic',
                'video-display-symbolic',
            ]
            for icon_name in bundled_icons + required_stock_icons:
                assert gtk.gtk_icon_theme_has_icon(
                    icon_theme, icon_name.encode('utf-8')) == 1, (
                    f'GtkIconTheme missing icon: {icon_name}')
        finally:
            gio.g_resources_unregister(gres)
            gio.g_resource_unref(gres)


if __name__ == '__main__':
    sys.exit(main())
