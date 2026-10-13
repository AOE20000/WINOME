#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
# Copyright (C) 2026 WINOME contributors
#
# Build-step wrapper: convert the St stylesheet, then verify the result.
#
# The two stages are chained here rather than as two entries in meson's
# `command` array because meson treats the first element as the program and
# passes the rest as arguments, and a portable `cmd1 && cmd2` line would need a
# shell that `-c` does not provide on Windows.
#
# The check's exit status wins: if the conversion produced a stylesheet that
# silently lost something (an unmapped property, a swallowed pass-through, a
# max-*/min-width pair collapsed into a single minimum, timed rules left
# without St's implicit transition-property), the build stops.

import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
CONVERT = os.path.join(HERE, 'convert-st-css.py')
CHECK = os.path.join(HERE, 'check-st-css-coverage.py')


def main():
    if len(sys.argv) < 3:
        print('usage: convert-then-check.py <st.css> <gtk4.css>', file=sys.stderr)
        return 2

    src, dst = sys.argv[1], sys.argv[2]

    converted = subprocess.run([sys.executable, CONVERT, src, dst])
    if converted.returncode != 0:
        print('stylesheet conversion failed', file=sys.stderr)
        return converted.returncode

    checked = subprocess.run([sys.executable, CHECK, src, dst])
    return checked.returncode


if __name__ == '__main__':
    sys.exit(main())
