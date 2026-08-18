#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
# Copyright (C) 2026 WINOME contributors
#
# Translate GNOME Shell (St) CSS into a GTK4-compatible stylesheet.
# St-specific properties that GTK4 does not understand are mapped to their
# GTK4 equivalents; unmappable ones are dropped (the C++ host applies them
# via extracted tokens instead).

import re
import sys

# Default accent colors. -st-accent-color / -st-accent-fg-color are runtime
# variables in St (read from the GTK theme); we substitute fixed values.
ACCENT_COLOR = '#3584e4'
ACCENT_FG_COLOR = '#ffffff'

# -st-accent-* variables -> replacement color.
ACCENT_VARS = {
    '-st-accent-color': ACCENT_COLOR,
    '-st-accent-fg-color': ACCENT_FG_COLOR,
}

# Properties to map St -> GTK4 (value kept as-is).
RENAME = {
    'height': 'min-height',
    'width': 'min-width',
}

# -natural-hpadding: X -> padding-left/right: X (expanded to two decls).
EXPAND = {
    '-natural-hpadding': ('padding-left', 'padding-right'),
}

# Properties to drop entirely (GTK4 has no equivalent; handled by tokens
# or code, or are St drawing primitives we do not support yet).
DROP = set([
    '-minimum-hpadding', 'icon-size', 'spacing', 'spacing-rows',
    'spacing-columns', 'row-spacing', 'column-spacing', 'max-row-spacing',
    'max-column-spacing', 'page-padding-top', 'page-padding-right',
    'page-padding-bottom', 'page-padding-left', 'text-align', 'icon-shadow',
    'link-color', 'selected-color', 'selection-background-color',
    'warning-color', 'success-color', 'error-color', 'caret-color',
    'caret-size', 'visible-width', 'offset-y', '-arrow-background-color',
    '-arrow-base', '-arrow-border-color', '-arrow-border-radius',
    '-arrow-border-width', '-arrow-rise', '-barlevel-active-background-color',
    '-barlevel-background-color', '-barlevel-height',
    '-barlevel-overdrive-color', '-barlevel-overdrive-separator-width',
    '-boxpointer-gap', '-pie-background-color', '-pie-border-color',
    '-pie-border-width', '-slider-handle-radius', '-st-hfade-offset',
    '-st-vfade-offset', '-st-icon-style', '-y-offset', 'max-width',
    'max-height', 'background-gradient-start', 'background-gradient-end',
    'background-gradient-direction',
])


# --- St color function evaluation (delegated to st-css-eval) ---
#
# Color math is done by the st-css-eval tool, which links the extracted St
# engine (st-theme-node.c) so the results are bit-exact with GNOME. We no
# longer reimplement st-mix/st-lighten/... in Python.

import subprocess
import os

ST_CSS_EVAL = os.environ.get(
    'ST_CSS_EVAL',
    os.path.join(os.path.dirname(os.path.abspath(__file__)),
                 '..', 'build', 'st-compat', 'st-css-eval.exe'))


def eval_color_expr(expr):
    """Evaluate a St color expression via the st-css-eval tool."""
    expr = expr.strip()
    # Fast path for plain hex / rgba / transparent (no need to spawn).
    if re.match(r'^#[0-9a-fA-F]{3,8}$', expr) or \
       re.match(r'^rgba?\(', expr) or expr == 'transparent':
        return expr

    try:
        out = subprocess.run([ST_CSS_EVAL, expr],
                             capture_output=True, text=True,
                             timeout=5, check=True).stdout.strip()
        return out
    except (subprocess.SubprocessError, OSError):
        return expr


def split_args(s):
    """Split a function argument string by top-level commas."""
    args = []
    depth = 0
    cur = ''
    for c in s:
        if c == '(':
            depth += 1
            cur += c
        elif c == ')':
            depth -= 1
            cur += c
        elif c == ',' and depth == 0:
            args.append(cur.strip())
            cur = ''
        else:
            cur += c
    if cur.strip():
        args.append(cur.strip())
    return args


def find_color_expr_end(s, start):
    """Given a string and the start of 'st-xxx(...)', return the index just
    past the matching close paren (handles nesting)."""
    depth = 0
    i = start
    while i < len(s):
        if s[i] == '(':
            depth += 1
        elif s[i] == ')':
            depth -= 1
            if depth == 0:
                return i + 1
        i += 1
    return len(s)


def eval_color_value(value):
    """Evaluate any st-*() color functions and -st-accent-* in a value."""
    out = []
    i = 0
    while i < len(value):
        # Match 'st-name('
        m = re.match(r'st-[a-z]+\(', value[i:])
        if m:
            end = find_color_expr_end(value, i + m.end() - 1)
            expr = value[i:end]
            out.append(eval_color_expr(expr))
            i = end
            continue
        # Match '-st-accent-*'
        matched = False
        for var in ACCENT_VARS:
            if value[i:].startswith(var):
                out.append(eval_color_expr(var))
                i += len(var)
                matched = True
                break
        if matched:
            continue
        out.append(value[i])
        i += 1
    return ''.join(out)


# St pseudo-classes that GTK4 names differently.
PSEUDO_CLASS_MAP = {
    ':insensitive': ':disabled',
}

# St element types -> GTK4 CSS node names. The stylesheet is full of
# element selectors such as "StBoxLayout" / "StIcon" / "StLabel"; GTK4 does
# not know those GTypes, so without this map the rules silently never match.
# Each St type maps to the GTK4 widget that plays its role.
ST_ELEMENT_MAP = {
    'StBoxLayout': 'box',
    'StIcon': 'image',
    'StLabel': 'label',
    'StButton': 'button',
    'StEntry': 'entry',
    'StScrollBar': 'scrollbar',
    'StScrollView': 'scrolledwindow',
    'StBin': 'box',
}


def map_element_selectors(selector):
    """Rewrite St element names in a selector to their GTK4 equivalents.

    Only standalone tokens (not part of a class like ".StIcon" or an
    attribute) are replaced.
    """
    import re
    for st_name, gtk_name in ST_ELEMENT_MAP.items():
        selector = re.sub(r'(?<![.\w-])' + st_name + r'(?=\b)',
                          gtk_name, selector)
    return selector

# Override appended to the output: disable GtkButton's default Adwaita gradient
# background for the panel buttons, so only the GNOME highlight (translated to
# background-color from the box-shadow fill) is used.
#
# The clock-display button uses GNOME's $highlighted_child pattern: the button
# itself has no fill and the highlight lives on the .clock child. GNOME relies
# on !important to enforce that; we re-assert it here with later, more specific
# rules.
#
# NOTE: the `background` shorthand is NOT parsed by GTK4's CSS engine, so we
# must use the longhand `background-image: none` (the default GtkButton fill is
# a background-image gradient, which would otherwise paint over our
# background-color highlight).
GTK4_BUTTON_OVERRIDE = """
#panel .panel-button {
  background-image: none;
  background-color: transparent;
  outline: none;
  transition-property: background-color;
  transition-duration: 150ms;
  transition-timing-function: ease;
}
#panel .panel-button:hover,
#panel .panel-button:active,
#panel .panel-button:checked,
#panel .panel-button:focus {
  background-image: none;
}
#panel .panel-button.clock-display,
#panel .panel-button.clock-display:hover,
#panel .panel-button.clock-display:active,
#panel .panel-button.clock-display:checked,
#panel .panel-button.clock-display:focus,
#panel .panel-button.clock-display:active:hover,
#panel .panel-button.clock-display:checked:hover {
  background-image: none;
  background-color: transparent;
  border: none;
}
#panel .panel-button.clock-display .clock {
  transition-property: background-color;
  transition-duration: 150ms;
  transition-timing-function: ease;
}

/* GTK4's default button theme draws its background via a background-image
   gradient (not background-color), so GNOME's background-color colors are
   painted over but hidden. Clear the default gradient on GNOME-styled
   controls so the GNOME background-color shows through. */
.quick-toggle.button,
.quick-toggle-has-menu,
.quick-slider,
.icon-button,
.quick-settings-system-item .icon-button {
  background-image: none;
  border: none;
}
/* GNOME draws a focus ring only for keyboard navigation. GTK4's :focus also
   matches mouse clicks, so strip the box-shadow on plain :focus and keep it
   only on :focus-visible (keyboard focus). */
.quick-toggle.button:focus,
.quick-toggle-has-menu:focus,
.icon-button:focus,
.quick-settings-system-item .icon-button:focus {
  box-shadow: none;
  outline: none;
}
.quick-toggle.button:focus-visible,
.quick-toggle-has-menu:focus-visible,
.icon-button:focus-visible,
.quick-settings-system-item .icon-button:focus-visible {
  box-shadow: inset 0 0 0 2px rgba(134, 181, 239, 0.800);
}
.quick-toggle > box {
  /* Native: .quick-toggle > StBoxLayout { padding: 0 12px; } plus the :ltr
     rule (dropped by the conversion) adding 15px on the left. */
  padding: 0 12px 0 15px;
}
/* The inner toggle of a has-menu toggle shrinks to its content width (native
   sets min-width: auto, which the conversion drops because GTK4 rejects
   'auto'), while keeping the 3.273em capsule height. */
.quick-toggle-has-menu .quick-toggle {
  min-width: 0;
}
/* Native has-menu inner-toggle box: padding-right 0.6135em (9px). */
.quick-toggle-has-menu .quick-toggle > box {
  padding-right: 9px;
}
/* The has-menu toggle is one continuous capsule split by the 1px separator:
   the left .quick-toggle is rounded only on its outer (left) corners and the
   ">" .quick-toggle-menu-button only on its right, so their join is square.
   Native does this with the :ltr rules, which the conversion drops. */
.quick-toggle-has-menu .quick-toggle {
  border-radius: 999px 0 0 999px;
}
.quick-toggle-has-menu .quick-toggle-menu-button {
  border-radius: 0 999px 999px 0;
}
/* Both toggle styles are fixed to exactly 12em: native uses
   min-width/max-width: 12em, but the conversion drops max-width. Without it
   a has-menu toggle (inner toggle + separator + menu button) or a long title
   grows past 12em and misaligns the grid. */
.quick-toggle,
.quick-toggle-has-menu {
  max-width: 12em;
}
/* GtkButton adds its own padding on top of the GNOME padding. Native has no
   padding on the buttons themselves (it lives in the inner box), so strip it;
   otherwise a has-menu toggle renders wider/taller than a plain toggle. */
.quick-toggle-has-menu {
  padding: 0;
}

/* Quick slider: rendered by the custom WinomeQuickSlider widget (GNOME
   trough rgba(255,255,255,0.15), accent #3584e4 highlight, 18px white
   handle). The .slider-bin padding/radius comes from the converted theme. */

/* Icon buttons (battery/volume/brightness round buttons) use the GNOME %button
   dark-grey fill; their exact colors and circular radius come from the
   converted stylesheet (evaluated bit-exactly via st-css-eval). */

/* The quick-settings / calendar popover: GNOME dark card background, matching
   .popup-menu-content.quick-settings (background + 1px border + drop shadow;
   border-radius is .quick-settings' 36px, which overrides popup-menu-content's
   20px). The popover is a separate toplevel window, so re-assert the panel
   font here for the same em resolution as GNOME (Cantarell 11pt, applied via
   the shared font map). GTK4's GtkPopover adds a default margin around the
   contents node, so also remove it to avoid a ring of empty card around the
   content. */
popover.quick-settings-popover > contents {
  background-color: #36363a;
  color: #ffffff;
  border-radius: 36px;
  border: 1px solid #424247;
  box-shadow: 0 2px 4px 0 rgba(0, 0, 0, 0.2);
  font: 11pt "Cantarell";
  padding: 0;
}
/* The calendar popover keeps its own radius (.datemenu-popover). */
popover.quick-settings-popover.datemenu-popover > contents {
  border-radius: 30px;
}
/* The quick-settings box itself adds the GNOME 18px padding. */
.quick-settings {
  padding: 18px;
}

/* Panel font: the bundled Cantarell (GNOME's default UI font) at 11pt, so
   every em value in the stylesheet (panel height, workspace-dot, paddings)
   resolves to the same pixel size as GNOME Shell computes it. */
#panel {
  font: 11pt "Cantarell";
}

/* .panel-button horizontal padding mirrors -natural-hpadding: 12px (the St
   property is dropped by the conversion). Strip the default button padding
   and focus ring so only the GNOME highlight (translated to background-color)
   shows. */
#panel .panel-button {
  padding: 0 12px;
  min-height: 0;
  outline: none;
  box-shadow: none;
}
#panel .panel-button:focus,
#panel .panel-button:focus-visible {
  outline: none;
  box-shadow: none;
}

/* GNOME keeps a panel button highlighted (:active) while its menu is open;
   GTK4 clears :active when the mouse is released. The host toggles an "open"
   class so the highlight persists exactly like gnome-shell's popup menu.
   The clock-display button uses the $highlighted_child pattern: its button
   itself stays transparent and only the .clock child is highlighted. */
#panel .panel-button:not(.clock-display).open {
  background-color: rgba(255, 255, 255, 0.28);
}
#panel .panel-button:not(.clock-display).open:hover {
  background-color: rgba(255, 255, 255, 0.32);
}
#panel .panel-button.clock-display.open .clock {
  background-color: rgba(255, 255, 255, 0.28);
}
#panel .panel-button.clock-display.open:hover .clock {
  background-color: rgba(255, 255, 255, 0.32);
}
"""

# em values in the stylesheet resolve against the St theme context font
# ("Sans 11" at 96dpi). GTK4 computes em the same way but CEILS the result,
# while GNOME's St engine rounds (x + 0.5), so a fractional value like
# 0.5455em (8.0006px) becomes 9px in GTK4 but 8px in GNOME. Re-assert the
# exact pixel values on the panel-critical rules so the two engines agree.
def _st_em():
    """1em in px as computed by st-theme-node.c (Sans 11, resolution 96)."""
    return 11.0 * (96.0 / 72.0)

def _st_round(value):
    return int(value + 0.5)

_PANEL_EM = _st_em()

# Panel height: #panel { height: 2.2em } -> 32px (not GTK4's 33px).
_PANEL_HEIGHT_PX = _st_round(2.2 * _PANEL_EM)
# Activities workspace dot: min-width/height 0.5455em -> 8px (not 9px).
_DOT_SIZE_PX = _st_round(0.5455 * _PANEL_EM)
# Activities indicators-box padding: 0 0.2045em -> 3px (not 4px).
_ACTIVITIES_PAD_PX = _st_round(0.2045 * _PANEL_EM)

PANEL_PX_OVERRIDE = """
/* Re-assert exact px values where GTK4's em ceiling differs from St rounding. */
#panel {
  min-height: %(panel_height)spx;
}
#panel .panel-button#panelActivities box {
  padding-left: %(activities_pad)spx;
  padding-right: %(activities_pad)spx;
}
#panel .panel-button#panelActivities .workspace-dot {
  min-width: %(dot_size)spx;
  min-height: %(dot_size)spx;
}
""" % {
    'panel_height': _PANEL_HEIGHT_PX,
    'activities_pad': _ACTIVITIES_PAD_PX,
    'dot_size': _DOT_SIZE_PX,
}


# St state/language pseudo-classes with no GTK4 equivalent. Rules containing
# only these are dropped (they describe St-specific states the panel does not
# use). A rule may combine mapped and unmapped classes; we handle it below.
UNKNOWN_PSEUDO_CLASSES = set([
    ':overview', ':rtl', ':ltr', ':highlighted', ':logged-in', ':latched',
    ':cast', ':second-in-stack', ':primary-monitor', ':lower-in-stack',
    ':drop',
])


def process_selector(selector):
    """Map St pseudo-classes to GTK4 names.

    Returns (kind, text) where kind is 'keep', 'drop', or 'block-drop'.
    """
    # If the selector contains an unknown pseudo-class (St state like
    # :overview/:unlock-screen), the entire rule must be dropped (not just the
    # pseudo-class), because stripping it would change semantics and cause it
    # to wrongly apply to the default state (e.g. background: transparent).
    for cls in UNKNOWN_PSEUDO_CLASSES:
        if cls in selector:
            return ('drop', None)

    out = selector
    for st_cls, gtk_cls in PSEUDO_CLASS_MAP.items():
        out = out.replace(st_cls, gtk_cls)

    # The panel buttons share the highlight between :hover and :focus in GNOME.
    # GTK4, unlike gnome-shell, gives the first focusable panel button keyboard
    # focus when the window is shown (FOCUSED, but not FOCUS_VISIBLE), which
    # would leave a permanent fill. :focus-visible matches GTK4's keyboard-only
    # focus (Tab), so the panel only highlights on real keyboard focus.
    if '#panel' in out:
        out = re.sub(r':focus(?!-visible)', ':focus-visible', out)

    return ('keep', map_element_selectors(out))


def process_declaration(prop, value):
    """Return a list of (prop, value) tuples, or None to drop the decl."""
    prop = prop.strip()
    value = value.strip()

    # GTK4 does not support !important on some properties (e.g. box-shadow);
    # strip it globally as we have no specificity conflicts.
    value = value.replace('!important', '').strip()

    # GTK4 sizing properties reject 'auto'; in St it means "no constraint",
    # so drop such declarations entirely.
    if value == 'auto':
        return None

    # GNOME uses "box-shadow: inset 0 0 0 100px <color>" as a background fill
    # hack (see _drawing.scss panel_button_fill). GTK4 renders this huge spread
    # unreliably (state sticks after hover/click), so translate it into a
    # background-color fill, which is visually equivalent for panel buttons.
    if prop == 'box-shadow':
        m = re.match(r'^inset 0 0 0 100px (.+)$', value)
        if m:
            return [('background-color', m.group(1))]

    if prop in RENAME:
        return [(RENAME[prop], eval_color_value(value))]
    if prop in EXPAND:
        return [(p, eval_color_value(value)) for p in EXPAND[prop]]
    if prop in DROP:
        return None
    return [(prop, eval_color_value(value))]


def main():
    if len(sys.argv) < 3:
        print('usage: convert-st-css.py <in.css> <out.css>')
        sys.exit(1)

    with open(sys.argv[1], 'r', encoding='utf-8') as f:
        css = f.read()

    out_lines = []
    # Number of open braces we are currently skipping (dropped rule block).
    drop_depth = 0

    for line in css.split('\n'):
        stripped = line.strip()

        # If skipping a dropped block, still track brace depth so we know
        # when the block ends.
        if drop_depth > 0:
            drop_depth += stripped.count('{') - stripped.count('}')
            continue

        # Declaration line?
        m = re.match(r'^(\s*)([a-zA-Z-]+)\s*:\s*([^;]+);(\s*)(\}?)\s*$', line)
        if m:
            indent = m.group(1)
            prop = m.group(2)
            value = m.group(3)
            trailing = m.group(5)
            result = process_declaration(prop, value)
            if result is None:
                if trailing:
                    out_lines.append('%s}' % indent)
                continue
            for p, v in result:
                out_lines.append('%s%s: %s;%s' % (indent, p, v, trailing))
            continue

        # Selector / structural line: check for unknown pseudo-classes.
        if '{' in stripped or ':' in line:
            kind, text = process_selector(line)
            if kind == 'drop':
                # Drop this rule block entirely. Count the '{' (if any) and
                # its matching '}'.
                drop_depth = line.count('{') - line.count('}')
                continue
            out_lines.append(text)
        elif stripped.endswith(',') and re.search(r'\bSt[A-Za-z]', line):
            # Continuation line of a multi-line selector group. The { line is
            # processed above; only map St element names here (pseudo-class
            # handling must not drop a single line of the group).
            out_lines.append(map_element_selectors(line))
        else:
            out_lines.append(line)

    out = '\n'.join(out_lines)
    out = re.sub(r'\n\s*\n\s*\n', '\n\n', out)

    # GTK4's GtkButton has an Adwaita default gradient background
    # (background-image) that is NOT disabled by GNOME's background-color:
    # transparent. Without this override, the button's default gradient stacks
    # with the GNOME box-shadow hover highlight, producing a double highlight.
    out += GTK4_BUTTON_OVERRIDE
    out += PANEL_PX_OVERRIDE

    with open(sys.argv[2], 'w', encoding='utf-8') as f:
        f.write('/* SPDX-License-Identifier: GPL-2.0-only */\n')
        f.write('/* Converted for GTK4 from gnome-shell-dark.css. */\n')
        f.write(out)

    print('Wrote %s' % sys.argv[2])


if __name__ == '__main__':
    main()
