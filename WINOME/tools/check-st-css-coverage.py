#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
# Copyright (C) 2026 WINOME contributors
#
# Coverage check for the St -> GTK4 stylesheet conversion.
#
# Why this exists: the converter drops or rewrites declarations silently, so a
# property the upstream stylesheet relies on can vanish from the output without
# any error — which is exactly how `max-width: 12em` disappeared (it was
# rewritten to a second `min-width`, so GNOME's fixed-width toggle grid became
# label-sized and the panel lost its visual rhythm) and how the implicit
# `transition-property` default went missing (St defaults it to
# `background-color`, GTK4 does not, so every hover snapped).
#
# This script turns "someone remembered to check the stylesheet" into a build
# step. Three groups of checks:
#
#   1. Coverage      — every property used upstream is either mapped, expanded,
#                      dropped on purpose, or passed through as a GTK4-native
#                      property. Anything else is a silent drop.
#   2. Pass-through   — properties we pass through must actually appear in the
#                      output, or the converter is eating them.
#   3. Semantics      — declarations that only work as a *pair* in St must remain
#                      a pair after conversion. min-width/max-width is the
#                      motivating case: dropping one half turns a fixed size
#                      into a minimum.
#
# Usage:  check-st-css-coverage.py <st.css> <gtk4.css>
# Exit status is non-zero when a check fails, so it can gate a build.

import os
import re
import sys

# --- property tables, kept in sync with convert-st-css.py --------------------
#
# Read from the converter itself so the two can never disagree. A regex pulls
# each literal out rather than importing the module, which would run main().

HERE = os.path.dirname(os.path.abspath(__file__))
CONVERTER = os.path.join(HERE, 'convert-st-css.py')


def _load_table(name, terminator):
    src = open(CONVERTER, encoding='utf-8').read()
    # Match either a dict literal or a set([...]) literal, whichever the table
    # uses. The brackets are escaped so the group itself stays balanced.
    m = re.search(r'^%s\s*=\s*(?:\{|set\(\s*\[)' % re.escape(name), src, re.M)
    if not m:
        raise SystemExit('converter: table %s not found' % name)
    start = m.end()
    end = src.index(terminator, start)
    return set(re.findall(r"'([^']+)'", src[start:end]))


RENAME = _load_table('RENAME', '\n}')
EXPAND = _load_table('EXPAND', '\n}')
DROP = _load_table('DROP', '])')

DECLARED = RENAME | EXPAND | DROP

# Properties that need no entry: GTK4's own stylesheet language defines them,
# so the converter forwards them untouched. Listing them explicitly (rather
# than passing anything unknown through) keeps the check meaningful — a new
# St-only property shows up as a gap instead of disappearing.
GTK4_NATIVE = {
    'background-color', 'color', 'background', 'border', 'border-color',
    'border-radius', 'border-width', 'border-top-width', 'border-right-width',
    'border-bottom-width', 'border-left-width',
    'padding', 'padding-top', 'padding-right', 'padding-bottom', 'padding-left',
    'margin', 'margin-top', 'margin-right', 'margin-bottom', 'margin-left',
    'font-weight', 'font-size', 'font-style', 'font-family',
    'font-feature-settings', 'min-width', 'min-height',
    'background-image', 'background-size', 'opacity',
    'transition', 'transition-duration', 'transition-property',
    'transition-timing-function',
    # GTK4 implements box-shadow natively. The converter additionally rewrites
    # St's "inset 0 0 0 100px <color>" background-fill hack into a
    # background-color, because GTK4 renders a spread that large unreliably.
    'box-shadow',
}

# Pseudo-classes whose rules the converter drops wholesale (see
# UNKNOWN_PSEUDO_CLASSES in convert-st-css.py). Properties that appear *only*
# inside such rules are legitimately absent from the output, so they must not
# fail the pass-through check. This host is LTR-only, so :rtl/:ltr rules go.
DIRECTION_PSEUDO = re.compile(r':(rtl|ltr)\b')

# Declarations whose meaning depends on another being present, keyed by the pair
# as it appears in the *converted* St stylesheet. Note sassc has already turned
# St's `width` into `min-width` (St treats width as a minimum), so a fixed-size
# rule reads `min-width: 12em; max-width: 12em` — never `width` + `max-width`.
# If only the minimum survives, the row stops aligning with its neighbours,
# which is how the panel lost its grid rhythm.
SEMANTIC_PAIRS = {
    'min-width': 'max-width',
    'min-height': 'max-height',
}


def parse_properties(css, skip_direction_rules=False):
    """Return {property: occurrence count} for a stylesheet's declarations.

    With skip_direction_rules, declarations inside a rule whose selector carries
    :rtl or :ltr are left out. The converter drops those rules entirely (this
    host is LTR-only), so their properties are expected to be missing from the
    output.
    """
    body = re.sub(r'/\*.*?\*/', '', css, flags=re.S)
    counts = {}
    for m in re.finditer(r'([^{}]*)\{([^{}]*)\}', body):
        selector = m.group(1)
        if skip_direction_rules and DIRECTION_PSEUDO.search(selector):
            continue
        for decl in m.group(2).split(';'):
            d = decl.strip()
            if not d or ':' not in d:
                continue
            prop = d.split(':', 1)[0].strip()
            if re.match(r'^-?[a-zA-Z][a-zA-Z0-9-]*$', prop):
                counts[prop] = counts.get(prop, 0) + 1
    return counts


def main():
    if len(sys.argv) < 3:
        print('usage: check-st-css-coverage.py <st.css> <gtk4.css>')
        return 2

    st_path, gtk4_path = sys.argv[1], sys.argv[2]
    st_css = open(st_path, encoding='utf-8').read()
    st_props = parse_properties(st_css)
    # Coverage judges the whole stylesheet; pass-through ignores :rtl/:ltr
    # rules, which the converter drops by design.
    st_props_ltr = parse_properties(st_css, skip_direction_rules=True)
    gtk4_css = open(gtk4_path, encoding='utf-8').read()
    gtk4_props = parse_properties(gtk4_css)
    gtk4_body = re.sub(r'/\*.*?\*/', '', gtk4_css, flags=re.S)

    failures = []

    # 1. Coverage: every upstream property must have a declared fate.
    undeclared = {p: c for p, c in st_props.items()
                  if p not in DECLARED and p not in GTK4_NATIVE}
    if undeclared:
        print('FAIL coverage: %d propert%s neither mapped nor dropped:'
              % (len(undeclared), 'y' if len(undeclared) == 1 else 'ies'))
        for p, c in sorted(undeclared.items(), key=lambda kv: -kv[1]):
            print('       %5dx  %s' % (c, p))
        print('       Add each to RENAME / EXPAND / DROP in convert-st-css.py,'
              ' or to GTK4_NATIVE here if GTK4 understands it as-is.')
        failures.append('coverage')

    # 2. Pass-through: anything GTK4 understands must reach the output.
    missing = {p: c for p, c in st_props_ltr.items()
               if p in GTK4_NATIVE and p not in gtk4_props}
    if missing:
        print('FAIL pass-through: %d GTK4-native propert%s never reached the'
              ' output:' % (len(missing), 'y' if len(missing) == 1 else 'ies'))
        for p, c in sorted(missing.items(), key=lambda kv: -kv[1]):
            print('       %5dx  %s' % (c, p))
        failures.append('pass-through')

    # 3. Semantics: a rule setting both halves of a size range must keep both.
    #    Counting is per rule, so "N rules upstream set both but max-* is absent
    #    from the output" means the upper bound was rewritten away.
    body = re.sub(r'/\*.*?\*/', '', st_css, flags=re.S)
    for base, upper in SEMANTIC_PAIRS.items():
        paired = 0
        for m in re.finditer(r'([^{}]*)\{([^{}]*)\}', body):
            selector = m.group(1)
            if DIRECTION_PSEUDO.search(selector):
                continue
            decls = m.group(2)
            if re.search(r'(^|;)\s*%s\s*:' % re.escape(base), decls) and \
               re.search(r'(^|;)\s*%s\s*:' % re.escape(upper), decls):
                paired += 1
        if paired and upper not in gtk4_props:
            print('FAIL semantics: %d rule(s) set both %s and %s upstream, but'
                  ' %s is absent from the output — the upper bound was lost,'
                  ' turning a fixed size into a minimum.'
                  % (paired, base, upper, upper))
            failures.append('semantics:%s' % upper)

    # 4. St's implicit transition-property default must have been supplied.
    #    St defaults transition-property to background-color, so a bare
    #    `transition-duration: 100ms` animates the hover fill. GTK4 has no such
    #    default and the declaration goes inert.
    #
    #    A count comparison, not a presence test: the converter has hand-written
    #    overrides (the panel button rules) that already spell out
    #    transition-property, so the property is always present at least twice.
    #    What matters is that *every* timed rule in the output got one.
    timed = 0
    with_property = 0
    for m in re.finditer(r'([^{}]*)\{([^{}]*)\}', gtk4_body):
        decls = m.group(2)
        if re.search(r'(^|;)\s*transition-duration\s*:', decls):
            timed += 1
            if re.search(r'(^|;)\s*(transition-property|transition)\s*:',
                         decls):
                with_property += 1
    if timed and with_property < timed:
        print('FAIL transitions: %d timed rule(s) in the output, but only %d'
              ' carry a transition-property. St defaults it to'
              ' background-color; GTK4 does not, so the rest animate nothing'
              ' and hover snaps.' % (timed, with_property))
        print('       Check that inject_transition_property() is wired into'
              ' main() in convert-st-css.py.')
        failures.append('transitions')

    if failures:
        print('\n%d check(s) failed: %s' % (len(failures), ', '.join(failures)))
        return 1

    print('OK  %d upstream properties all accounted for;'
          ' %d reached the GTK4 stylesheet.' % (len(st_props), len(gtk4_props)))
    return 0


if __name__ == '__main__':
    sys.exit(main())
