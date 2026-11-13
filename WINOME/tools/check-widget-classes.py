#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
# Copyright (C) 2026 WINOME contributors
#
# Widget-structure coverage check.
#
# The stylesheet checker (check-st-css-coverage.py) verifies that declarations
# survive the St -> GTK4 conversion. It cannot see the opposite failure: a rule
# that reaches the stylesheet but has no widget behind it. That is exactly how
# the QuickToggleMenu header went missing — `.quick-toggle-menu .header` and
# its `.icon` badge had been in gnome-shell-gtk4.css the whole time, fully
# styled, and nothing in the C++ ever created the node.
#
# This checks that gap from the other side: for every class the stylesheet
# mentions, does the source actually add it?
#
# Two failure modes it catches:
#   * a class the stylesheet styles but the code never creates (missing widget)
#   * a class the code adds that the stylesheet never mentions (dead class —
#     it looks like it should be styling something but does nothing)
#
# Classes are grouped so a report is readable: anything under nm- / bt- /
# background-app / device- belongs to an unimplemented subsystem and is
# expected to be absent.
#
# Usage: check-widget-classes.py <gtk4.css> <source.cpp> [...]
# Exit status is non-zero when an unexpected gap is found.

import os
import re
import sys

# Subsystems this host does not implement yet, or that live in another file.
# GNOME batches unrelated widgets into shared rules, so a scoped rule routinely
# drags along classes for the calendar, the login dialog, the screen shield or
# the lock screen. Those are not findings about the quick settings tree.
OPTIONAL_PREFIXES = (
    'nm-',            # NetworkManager wifi rows
    'bt-',            # bluetooth rows
    'background-app', # background applications section
    'device-subtitle',
    'keyboard-brightness',
    'calendar',
    'datemenu',
    'candidate-popup',
    'caps-lock',
    'login-dialog',
    'dialog-',
    'message',
    'a11y-button',
    'app-menu',
    'quick-toggle-menu-container',
    'world-clocks-button',
    'weather-button',
    'events-button',
    'default',
    'cancel-button',
    'next-button',
    'switch-user-button',
    'screenshot-',
    'prompt-dialog',
    'end-session',
    'lga-',
    'lg-',
    # The calendar's event list, the polkit/auth dialogs, the run dialog, the
    # weather and clocks entries: shared rules pull them in, none of them is
    # part of the quick settings tree.
    'event-',
    'events-',
    'horizontal',
    'icon-label-button-container',
    'lightweight',
    'osd-window',
    'pager-button',
    'panel-menu',
    'parental-controls',
    'polkit-',
    'run-dialog',
    'restart-message',
    'unlock-dialog',
    'user-widget',
    'weather-',
    'word-suggestions',
    'world-clocks-',
)

# Classes that exist in the stylesheet but describe a layer this host replaces
# rather than instantiates. Each is justified; dropping them from the check
# keeps real findings visible.
IGNORED = {
    # The popover/quick-settings window chrome is created by shell-panel.cpp,
    # not by the file passed on the command line.
    'quick-settings-popover-card',
    'quick-settings-popover-shadow-wrap',
    'quick-settings-popover-window',
    # Clutter paints the bubble and its arrow on a canvas; there is no widget
    # to create and no way to reproduce it from CSS alone.
    'popup-menu-boxpointer',
    'popup-menu-arrow',
    # .popup-menu-content is deliberately not used: upstream's QuickToggleMenu
    # box takes 'quick-toggle-menu' as its only style class, and nesting a
    # content box inside it paints a second rounded background.
    'popup-menu-content',
    # Structural wrapper kept for parity with upstream's PopupMenuBase box.
    'popup-menu-box',
    # PopupMenuBase assigns box.style_class = styleClass (popupMenu.js:647) —
    # an assignment, not an append. QuickToggleMenu passes 'quick-toggle-menu'
    # (quickSettings.js:365), so a secondary menu's box carries that one class
    # only, and .popup-menu's min-width: 15em does not apply to it.
    'popup-menu',
    # The main app menu / calendar / shield / run dialog live in other
    # modules. GNOME shares rules between them and the quick settings, so
    # these arrive alongside the scoped selectors without belonging to it.
    'popup-menu-section',
    'popup-separator-menu-item',
    'popup-separator-menu-item-separator',
    'popup-inactive-menu-item',
    'popup-sub-menu',
}


# Only rules rooted at one of these belong to the widget tree under review. The
# stylesheet also covers the calendar, the overview, screen shield and the
# login dialog, which this check has nothing to say about — those live in
# calendar.cpp / overview*.cpp and are checked by their own means.
SCOPE_PREFIXES = (
    'quick-toggle',
    'quick-settings',
    'popup-menu',
    'popup-sub-menu',
    'quick-slider',
)


def stylesheet_classes(css_path):
    """Classes under review: those reachable from a scoped root selector.

    Parsed line-wise because the generated stylesheet wraps long selector
    groups across lines ("a, b,\\n  c, d {"), which a brace-matching regex
    over the whole file does not survive. A selector group is accumulated until
    its '{' arrives; if any selector in the group starts with a scoped prefix,
    every class in the group is collected — GNOME batches unrelated widgets
    into shared rules, so a scoped rule routinely names classes belonging
    elsewhere.
    """
    text = re.sub(r'/\*.*?\*/', '', open(css_path, encoding='utf-8').read(),
                  flags=re.S)
    found = set()
    pending = []
    for line in text.split('\n'):
        stripped = line.strip()
        if not stripped:
            continue
        # A line that ends with the closing brace of a previous block also
        # carries the opening brace of this one ("...: 1px; }"), and a line may
        # be pure declarations. Only a trailing '{' opens a selector group.
        if stripped.endswith('}'):
            pending = []
            continue
        pending.append(stripped)
        if not stripped.endswith('{'):
            continue
        group = ' '.join(pending)
        pending = []
        selectors = [s.strip() for s in group.split('{')[0].split(',')]
        selectors = [s for s in selectors if s]
        scoped = False
        for s in selectors:
            # Drop the state part (".quick-toggle:hover" -> ".quick-toggle")
            # and test the classes in it. A compound selector like
            # "#dash .quick-toggle-menu" scopes just as well as a bare one, so
            # match on the class names rather than on the whole string.
            base = s.split(':')[0]
            for name in re.findall(r'\.([a-zA-Z][a-zA-Z0-9_-]*)', base):
                if name.startswith(SCOPE_PREFIXES):
                    scoped = True
                    break
            if scoped:
                break
        if scoped:
            for s in selectors:
                for name in re.findall(r'\.([a-zA-Z][a-zA-Z0-9_-]*)', s):
                    found.add(name)
    return found


def source_classes(paths):
    """Classes the source adds, from the GTK CSS calls only.

    Deliberately narrow: matching every string literal would sweep up translated
    labels ("Wi-Fi") and icon names ("audio-volume-high-symbolic"), neither of
    which is a CSS class.
    """
    found = set()
    call = re.compile(
        r'gtk_widget_(?:add|remove|toggle)_css_class\s*\(\s*[^,]+,\s*"([^"]+)"')
    for path in paths:
        src = open(path, encoding='utf-8').read()
        for m in call.finditer(src):
            found.add(m.group(1))
    return found


def main():
    if len(sys.argv) < 3:
        print('usage: check-widget-classes.py <gtk4.css> <source.cpp> [...]')
        return 2

    css_path, src_paths = sys.argv[1], sys.argv[2:]
    styled = stylesheet_classes(css_path)
    used = source_classes(src_paths)

    missing = sorted(styled - used - IGNORED)
    optional = sorted(c for c in styled - used - IGNORED - set(missing)
                      if c.startswith(OPTIONAL_PREFIXES))
    missing_optional = [c for c in missing if c.startswith(OPTIONAL_PREFIXES)]
    missing = [c for c in missing if not c.startswith(OPTIONAL_PREFIXES)]

    dead = sorted(used - styled)

    failed = False

    if missing:
        print('FAIL structure: %d class(es) are styled upstream but never'
              ' created by the source — the rule has no widget behind it.'
              % len(missing))
        for c in missing:
            print('       .%s' % c)
        print('       Check the matching widget in upstream: .quick-toggle-menu'
              ' .header is built by QuickToggleMenu._init (quickSettings.js:389).')
        failed = True

    if missing_optional:
        print('note: %d class(es) belong to subsystems not implemented yet:'
              % len(missing_optional))
        for c in missing_optional:
            print('       .%s' % c)

    # Classes the source adds that nothing styles. Reported as a warning rather
    # than a failure: a class may legitimately be read by the C++ itself.
    if dead:
        print('note: %d class(es) added by the source have no rule in the'
              ' stylesheet (they style nothing):' % len(dead))
        for c in dead[:12]:
            print('       .%s' % c)
        if len(dead) > 12:
            print('       ... and %d more' % (len(dead) - 12))

    if failed:
        return 1

    print('OK  %d styled classes; %d created by the source;'
          ' %d optional, %d styled-but-unused.'
          % (len(styled), len(used & styled), len(optional),
             len(missing_optional)))
    return 0


if __name__ == '__main__':
    sys.exit(main())
