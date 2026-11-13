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
    assert 'max-width: 12em;' in out, out
    assert 'padding-left: 12px;' in out and 'padding-right: 12px;' in out, out
    assert '-st-hfade-offset' not in out, out
    assert '.weird' not in out, out
    assert 'transition-property: background-color, color, border-color, box-shadow;' in out, out
    assert '.toggle-btn:disabled' in out, out
    assert ':insensitive' not in out, out
    assert 'box.foo label' in out, out
    assert out.count('{') == out.count('}'), (
        f"unbalanced braces: {out.count('{')} vs {out.count('}')}")
    print('CSS converter regression tests passed.')
    return 0


if __name__ == '__main__':
    sys.exit(main())
