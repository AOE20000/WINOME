# SPDX-License-Identifier: GPL-2.0-only
# Copyright (C) 2026 WINOME contributors

winome_test_inc = include_directories('../src')

test_deps = [
  gtk4_dep,
  fontconfig_dep,
  pangoft2_dep,
  stcompat_dep,
  cc.find_library('shell32', required: true),
  cc.find_library('user32', required: true),
  cc.find_library('gdi32', required: true),
  cc.find_library('advapi32', required: true),
  cc.find_library('ole32', required: true),
  cc.find_library('oleaut32', required: true),
  cc.find_library('wininet', required: true),
  cc.find_library('dwmapi', required: true),
  cc.find_library('powrprof', required: true),
  cc.find_library('shlwapi', required: true),
  cc.find_library('version', required: true),
  cc.find_library('psapi', required: true),
  cc.find_library('bcrypt', required: true),
]

winome_mock_plugin = executable('winome-mock-plugin',
  files('mock-plugin.cpp'),
  win_subsystem: 'console',
)

winome_tests = executable('winome-tests',
  files(
    'test-main.cpp',
    '../src/config.cpp',
    '../src/i18n.cpp',
    '../src/host-utils.cpp',
    '../src/perf-metrics.cpp',
    '../src/system-status.cpp',
    '../src/winrt-bridge.cpp',
    '../src/notifications.cpp',
    '../src/systray.cpp',
    '../src/native-taskbar.cpp',
    '../src/monitor-manager.cpp',
    '../src/easing.cpp',
    '../src/wifi-networks.cpp',
    '../src/overview-layout.cpp',
    '../src/app-index.cpp',
    '../src/app-grid.cpp',
    '../src/fonts.cpp',
    '../src/st-engine.cpp',
    '../src/virtual-desktop.cpp',
    '../src/end-session-dialog.cpp',
    '../src/shell-ipc.cpp',
    '../src/window-theming.cpp',
    '../src/crash-dialog.cpp',
    '../src/debug-runtime.cpp',
    '../src/plugin-host.cpp',
    '../src/run-dialog.cpp',
  ),
  dependencies: test_deps,
  include_directories: [winome_test_inc, winome_root_inc],
  cpp_args: ['-DUNICODE', '-D_UNICODE'],
)

test('unit-tests', winome_tests, depends: [winome_mock_plugin])

python3 = find_program('python3', 'python')
test('css-converter', python3,
  args: [files('test-css-converter.py'),
         meson.project_source_root() / 'tools' / 'convert-st-css.py'],
)
