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
#
# St's `width`/`height` are minima, so they become GTK4's min-*. The max-* forms
# must map to max-*, NOT to min-*: mapping them to min- silently deleted the
# upper bound, so `.quick-toggle { min-width: 12em; max-width: 12em }` — GNOME's
# fixed-width toggle grid — collapsed into two min-width rules and every capsule
# grew to fit its label. That is what broke the panel's visual rhythm.
RENAME = {
    'height': 'min-height',
    'width': 'min-width',
    'max-height': 'max-height',
    'max-width': 'max-width',
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

/* The quick-settings / calendar popovers are custom borderless toplevels
   (see shell-panel.cpp): the window is transparent and a wrapper card carries
   the GNOME dark background, matching .popup-menu-content.quick-settings
   (background + 1px border + drop shadow; border-radius is .quick-settings'
   36px, which overrides popup-menu-content's 20px). The shadow-wrap leaves
   room for the drop shadow. The popover is a separate toplevel window, so the
   Cantarell 11pt font is re-asserted here for the same em resolution as GNOME
   (applied via the shared font map). */
window.quick-settings-popover-window {
  background-color: transparent;
}
.quick-settings-popover-shadow-wrap {
  padding: 6px;
}
.quick-settings-popover-card {
  background-color: #36363a;
  color: #ffffff;
  border-radius: 36px;
  border: 1px solid #424247;
  box-shadow: 0 2px 4px 0 rgba(0, 0, 0, 0.2);
  font: 11pt "Cantarell";
}
/* The calendar popover keeps its own radius (.datemenu-popover). */
.quick-settings-popover-card.datemenu-card {
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

/* Transparent panel over the Activities overview, matching native
   #panel:overview (the :overview pseudo-class is dropped by the conversion;
   the host toggles this class on the panel window via the overview state
   callback). The panel strip then shows the overview behind it, GNOME style. */
#panel.overview {
  background-color: transparent;
}

/* In-host Activities overview toplevel (layering: popover > panel >
   overview > apps > wallpaper). Opaque #222226, exactly like the native
   #overviewGroup: the overview is dark everywhere outside the wallpaper
   rect, so any region not yet rendered by GTK (e.g. a frame between a
   surface resize and the first paint) shows the overview's own color
   instead of the desktop behind it. The font mirrors the St theme context
   so every em value (search-entry 24em width, caption, dash) resolves
   exactly like St's "Sans 11" at 96dpi. */
window.winome-overview {
  background-color: #222226;
  font: 11pt "Cantarell";
}

/* The overview chrome windows (window-close button, hover app icon) are
   borderless transparent toplevels layered above the DWM thumbnails. */
window.winome-overview-chrome {
  background-color: transparent;
  font: 11pt "Cantarell";
}

/* Search entry: GNOME renders the hint text at the entry's dimmed color
   (StLabel.hint-text, alpha 0.7) and the magnifier at the scalable icon size
   (1.091em = 16px), with 4px padding either side (icon padding rule). */
.search-entry placeholder {
  color: rgba(250, 250, 251, 0.7);
}
.search-entry image {
  -gtk-icon-size: 16px;
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

/* Strip GTK4 Default-dark button gradient/border bleed on GNOME UI controls
   whose upstream St selectors only set background-color/border-radius, and
   suppress focus rings when focus is not keyboard-visible. */
.calendar .calendar-day,
.calendar .calendar-month-header .pager-button,
.datemenu-today-button,
.events-button,
.world-clocks-button,
.weather-button,
.message,
.message .message-header .message-close-button,
.message .message-header .message-expand-button,
.message-media-control,
.message-list-clear-button,
.overview-tile,
.page-navigation-arrow,
.page-indicator,
.modal-dialog-linked-button,
.cancel-button,
.window-close {
  background-image: none;
  border-image: none;
  border-style: none;
}
.calendar .calendar-day:focus:not(:focus-visible),
.calendar .calendar-month-header .pager-button:focus:not(:focus-visible),
.datemenu-today-button:focus:not(:focus-visible),
.events-button:focus:not(:focus-visible),
.world-clocks-button:focus:not(:focus-visible),
.weather-button:focus:not(:focus-visible),
.message:focus:not(:focus-visible),
.message .message-header .message-close-button:focus:not(:focus-visible),
.message .message-header .message-expand-button:focus:not(:focus-visible),
.message-media-control:focus:not(:focus-visible),
.message-list-clear-button:focus:not(:focus-visible),
.overview-tile:focus:not(:focus-visible),
.page-navigation-arrow:focus:not(:focus-visible),
.page-indicator:focus:not(:focus-visible),
.modal-dialog-linked-button:focus:not(:focus-visible),
.cancel-button:focus:not(:focus-visible),
.window-close:focus:not(:focus-visible) {
  outline-style: none;
  outline-width: 0;
  box-shadow: none;
}

.popup-menu-item {
  background-image: none;
  border-image: none;
  border-style: none;
}
.popup-menu-item:focus:not(:focus-visible) {
  outline-style: none;
  outline-width: 0;
  box-shadow: none;
}

/* Page indicator buttons should stay transparent around the dot indicator. */
.page-indicator {
  background-color: transparent;
  box-shadow: none;
  padding: 6px;
  min-width: 12px;
  min-height: 12px;
}

/* Match GNOME St switch (.toggle-switch + .handle) on GTK4 GtkSwitch. */
switch.toggle-switch {
  min-width: 46px;
  min-height: 26px;
  padding: 0;
  border: none;
  box-shadow: none;
  background-image: none;
  border-radius: 999px;
}
switch.toggle-switch > slider {
  min-width: 20px;
  min-height: 20px;
  margin: 3px;
  padding: 0;
  border: none;
  border-radius: 999px;
  background-image: none;
  background-color: #d7d7d8;
  box-shadow: 0 2px 4px rgba(0, 0, 0, 0.2);
}
switch.toggle-switch:checked > slider {
  background-color: #ffffff;
}
switch.toggle-switch:focus:not(:focus-visible) {
  outline-style: none;
  outline-width: 0;
}

/* Match upstream StScrollBar thumb styling in scrollable shell surfaces. */
window.quick-settings-popover-window scrollbar,
window.winome-overview scrollbar {
  background-color: transparent;
  background-image: none;
  border: none;
}
window.quick-settings-popover-window scrollbar trough,
window.winome-overview scrollbar trough {
  background-color: transparent;
  background-image: none;
  border: none;
  margin: 0;
  padding: 2px;
}
window.quick-settings-popover-window scrollbar slider,
window.winome-overview scrollbar slider {
  min-width: 6px;
  min-height: 24px;
  border-radius: 8px;
  border: none;
  background-image: none;
  background-color: #727275;
}
window.quick-settings-popover-window scrollbar slider:hover,
window.winome-overview scrollbar slider:hover {
  background-color: #9b9b9d;
}
window.quick-settings-popover-window scrollbar slider:active,
window.winome-overview scrollbar slider:active {
  background-color: #868689;
}

/* Overview context GtkPopovers delegate frame styling to .popup-menu-content. */
window.winome-overview popover.background > contents {
  background: transparent;
  background-color: transparent;
  border: none;
  box-shadow: none;
  padding: 0;
}
window.winome-overview popover.background > arrow {
  background: transparent;
  background-color: transparent;
  border: none;
  box-shadow: none;
}

/* Floating notification toast toplevel keeps a transparent frame around cards. */
window#winomeToastWindow {
  background-color: transparent;
  box-shadow: none;
}

/* QuickToggleMenu inside its own BoxPointer window sits flush under the arrow. */
window.quick-settings-popover-window .quick-toggle-menu {
  margin-top: 0;
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
# only these are dropped (they describe St-specific states the shell does not
# use). A selector group may mix mapped and unmapped pseudo-classes on one line;
# individual comma-separated sub-selectors are filtered below.
UNKNOWN_PSEUDO_CLASSES = set([
    ':overview', ':rtl', ':highlighted', ':logged-in', ':latched',
    ':cast', ':second-in-stack', ':primary-monitor', ':lower-in-stack',
    ':drop',
])


def _clean_subselector(sub):
    """Map one comma-separated sub-selector, or return None to drop it."""
    for cls in UNKNOWN_PSEUDO_CLASSES:
        if cls in sub:
            return None
    # Standalone ' :ltr' targets arbitrary LTR descendants; drop it so we do
    # not mistakenly apply child offsets to the parent selector.
    if re.search(r'(?<!\S):ltr\b', sub):
        return None

    out = re.sub(r'(?<=\S):ltr\b', '', sub)
    for st_cls, gtk_cls in PSEUDO_CLASS_MAP.items():
        out = out.replace(st_cls, gtk_cls)

    # The panel buttons share the highlight between :hover and :focus in GNOME.
    # GTK4, unlike gnome-shell, gives the first focusable panel button keyboard
    # focus when the window is shown (FOCUSED, but not FOCUS_VISIBLE), which
    # would leave a permanent fill. :focus-visible matches GTK4's keyboard-only
    # focus (Tab), so the panel only highlights on real keyboard focus.
    if '#panel' in out:
        out = re.sub(r':focus(?!-visible)', ':focus-visible', out)

    return map_element_selectors(out)


def process_selector(selector):
    """Map St pseudo-classes to GTK4 names.

    Returns (kind, text) where kind is 'keep' or 'drop'.
    """
    if '{' in selector:
        head, _, tail = selector.partition('{')
        indent = head[:len(head) - len(head.lstrip())]
        kept = []
        for part in head.split(','):
            cleaned = _clean_subselector(part)
            if cleaned is not None and cleaned.strip():
                kept.append(cleaned.strip())
        if not kept:
            return ('drop', None)
        return ('keep', f"{indent}{', '.join(kept)} {{{tail}")

    stripped = selector.rstrip()
    has_comma = stripped.endswith(',')
    body = stripped[:-1] if has_comma else stripped
    indent = body[:len(body) - len(body.lstrip())]
    kept = []
    for part in body.split(','):
        cleaned = _clean_subselector(part)
        if cleaned is not None and cleaned.strip():
            kept.append(cleaned.strip())
    if not kept:
        return ('drop', None)
    suffix = ',' if has_comma else ''
    return ('keep', f"{indent}{', '.join(kept)}{suffix}")


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


# St's `transition-property` defaults to `background-color` when a rule only
# sets `transition-duration`, so GNOME animates hover fills with the bare
# `transition-duration: 100ms` seen all over _common.scss (%menuitem,
# %card_common). GTK4 has no such default and silently ignores the declaration,
# which is why ported hover states snap instead of fading.
#
# The properties below are what GNOME's button/card mixins actually transition;
# supplying them restores the timing without changing any colour or duration.
TRANSITION_PROPERTY_DEFAULT = 'background-color, color, border-color, box-shadow'


def inject_transition_property(css):
    """Add the implicit transition-property to blocks that only set a duration.

    Purely textual and block-local: walks each {...} body, and when it finds a
    `transition-duration` with no `transition-property` (and no `transition`
    shorthand that already carries one), inserts the default list immediately
    before the duration so the declarations keep St's reading order.
    """
    lines = css.split('\n')
    out = []
    block_start = None   # index in `out` of the line holding the opening '{'
    depth = 0
    has_duration = False
    has_property = False

    def close_block():
        """Insert the default property list into the finished body, if needed.

        Looks back from the end of `out` for the transition-duration line. The
        caller must have appended the block's final line already: when the block
        closes on its last declaration ("transition-duration: 400ms; }"), that
        line is both the duration and the closer.
        """
        nonlocal has_duration, has_property
        if not (has_duration and not has_property):
            return
        for i in range(len(out) - 1, block_start, -1):
            m = re.match(r'^(\s*)transition-duration\s*:', out[i])
            if m:
                out.insert(i, '%stransition-property: %s;' % (
                    m.group(1), TRANSITION_PROPERTY_DEFAULT))
                break
        has_duration = False
        has_property = False

    for line in lines:
        stripped = line.strip()

        # A body we are collecting: only its declarations matter.
        if block_start is not None:
            if re.match(r'^\s*transition-duration\s*:', line):
                has_duration = True
            elif re.match(r'^\s*(transition-property|transition)\s*:', line):
                has_property = True

            # The body may close on its own line or ride on the last
            # declaration ("padding: 9px 12px; }"). Append first so the closer
            # is part of the block when close_block() searches for the
            # duration line.
            closes = (stripped == '}'
                      or (stripped.endswith('}') and '{' not in stripped
                          and stripped != ''))
            out.append(line)
            if closes:
                close_block()
                block_start = None
                depth = 0
            continue
            continue

        out.append(line)
        if stripped.endswith('{'):
            block_start = len(out) - 1
            depth = line.count('{') - line.count('}')
            has_duration = False
            has_property = False

    return '\n'.join(out)


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
                if ('{' in line and out_lines
                        and out_lines[-1].rstrip().endswith(',')):
                    brace_idx = line.index('{')
                    prev = re.sub(r',\s*$', '', out_lines.pop().rstrip())
                    text = f'{prev} {line[brace_idx:].lstrip()}'
                else:
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

    out = inject_transition_property(out)

    with open(sys.argv[2], 'w', encoding='utf-8') as f:
        f.write('/* SPDX-License-Identifier: GPL-2.0-only */\n')
        f.write('/* Converted for GTK4 from gnome-shell-dark.css. */\n')
        f.write(out)

    print('Wrote %s' % sys.argv[2])


if __name__ == '__main__':
    main()
