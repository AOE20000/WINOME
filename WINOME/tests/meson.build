# SPDX-License-Identifier: GPL-2.0-only
# Copyright (C) 2026 WINOME contributors

winome_test_inc = include_directories('../src')

test_deps = [
  gtk4_dep,
  cc.find_library('shell32', required: true),
  cc.find_library('user32', required: true),
  cc.find_library('gdi32', required: true),
  cc.find_library('advapi32', required: true),
  cc.find_library('ole32', required: true),
  cc.find_library('wininet', required: true),
  cc.find_library('powrprof', required: true),
  cc.find_library('shlwapi', required: true),
  cc.find_library('version', required: true),
]

winome_tests = executable('winome-tests',
  files(
    'test-main.cpp',
    '../src/config.cpp',
    '../src/i18n.cpp',
    '../src/host-utils.cpp',
    '../src/system-status.cpp',
    '../src/winrt-bridge.cpp',
    '../src/notifications.cpp',
    '../src/easing.cpp',
    '../src/wifi-networks.cpp',
    '../src/overview-layout.cpp',
  ),
  dependencies: test_deps,
  include_directories: [winome_test_inc],
  cpp_args: ['-DUNICODE', '-D_UNICODE'],
)

test('unit-tests', winome_tests)

python3 = find_program('python3', 'python')
test('css-converter', python3,
  args: [files('test-css-converter.py'),
         meson.project_source_root() / 'tools' / 'convert-st-css.py'],
)
