winome_sources = files(
  'main.cpp',
  'shell-panel.cpp',
  'fonts.cpp',
  'native-taskbar.cpp',
  'desktop-layer.cpp',
  'host-utils.cpp',
  'win-event-hook.cpp',
  'overview-trigger.cpp',
  'fullscreen-watcher.cpp',
  'hot-corner.cpp',
  'system-status.cpp',
  'quick-settings.cpp',
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
wininet_dep = cc.find_library('wininet', required: true)

gnome = import('gnome')

subdir('theme')

resources = gnome.compile_resources('winome-resources',
  'theme/winome.gresource.xml',
  source_dir: meson.current_build_dir() / 'theme',
  dependencies: [gnome_shell_gtk4_css, gnome_shell_dark_css, cantarell_fonts],
)

executable('winome',
  [winome_sources, resources],
  dependencies: [gtk4_dep, fontconfig_dep, pangoft2_dep, shell32_dep, user32_dep, gdi32_dep, psapi_dep, shlwapi_dep, advapi32_dep, ole32_dep, wininet_dep, stcompat_dep],
  include_directories: [winome_src_inc, include_directories('.')],
  cpp_args: ['-DUNICODE', '-D_UNICODE'],
  win_subsystem: 'windows',
  install: true,
)
