winome_sources = files(
  'main.cpp',
  'config.cpp',
  'i18n.cpp',
  'shell-panel.cpp',
  'fonts.cpp',
  'native-taskbar.cpp',
  'host-utils.cpp',
  'perf-metrics.cpp',
  'win-event-hook.cpp',
  'overview-trigger.cpp',
  'live-wallpaper.cpp',
  'overview.cpp',
  'overview-layout.cpp',
  'overview-dash.cpp',
  'overview-search.cpp',
  'overview-thumbs.cpp',
  'app-index.cpp',
  'app-grid.cpp',
  'virtual-desktop.cpp',
  'workspace-manager.cpp',
  'ws-osd.cpp',
  'fullscreen-watcher.cpp',
  'monitor-manager.cpp',
  'hot-corner.cpp',
  'system-status.cpp',
  'power-modes.cpp',
  'wifi-networks.cpp',
  'easing.cpp',
  'winrt-bridge.cpp',
  'notifications.cpp',
  'systray.cpp',
  'settings-window.cpp',
  'quick-settings.cpp',
  'end-session-dialog.cpp',
  'shell-ipc.cpp',
  'window-theming.cpp',
  'crash-dialog.cpp',
  'diagnostics-export.cpp',
  'debug-runtime.cpp',
  'looking-glass.cpp',
  'plugin-host.cpp',
  'run-dialog.cpp',
  'calendar.cpp',
  'st-engine.cpp',
)

winome_src_inc = include_directories('.')

cc = meson.get_compiler('cpp')

shell32_dep = cc.find_library('shell32', required: true)
user32_dep = cc.find_library('user32', required: true)
gdi32_dep = cc.find_library('gdi32', required: true)
psapi_dep = cc.find_library('psapi', required: true)
shlwapi_dep = cc.find_library('shlwapi', required: true)
advapi32_dep = cc.find_library('advapi32', required: true)
ole32_dep = cc.find_library('ole32', required: true)
oleaut32_dep = cc.find_library('oleaut32', required: true)
wininet_dep = cc.find_library('wininet', required: true)
dwmapi_dep = cc.find_library('dwmapi', required: true)
version_dep = cc.find_library('version', required: true)
powrprof_dep = cc.find_library('powrprof', required: true)
bcrypt_dep = cc.find_library('bcrypt', required: true)
epoxy_dep = dependency('epoxy', required: true)

gnome = import('gnome')

subdir('theme')

resources = gnome.compile_resources('winome-resources',
  'theme/winome.gresource.xml',
  source_dir: [meson.current_build_dir() / 'theme', meson.current_source_dir() / 'theme'],
  dependencies: [gnome_shell_gtk4_css, gnome_shell_dark_css, cantarell_fonts],
)

executable('winome',
  [winome_sources, resources],
  dependencies: [gtk4_dep, fontconfig_dep, pangoft2_dep, sqlite3_dep, zlib_dep, shell32_dep, user32_dep, gdi32_dep, psapi_dep, shlwapi_dep, advapi32_dep, ole32_dep, oleaut32_dep, wininet_dep, dwmapi_dep, version_dep, powrprof_dep, bcrypt_dep, stcompat_dep, epoxy_dep],
  include_directories: [winome_src_inc, winome_root_inc, include_directories('.')],
  cpp_args: ['-DUNICODE', '-D_UNICODE'],
  win_subsystem: 'windows',
  install: true,
)
