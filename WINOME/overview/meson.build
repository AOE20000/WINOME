# WinOverview overview components, compiled from the extracted sources.
#
# These two binaries are separate from the host because the overview DLL is
# injected into RuntimeBroker.exe, and the launcher must run at ordinary
# integrity level (it is started inside explorer.exe via WinExec).

extracted_root = '../..' / 'extracted' / 'WinOverview'

ov_lib_inc = include_directories(
  extracted_root / 'WinOverviewLibrary',
  extracted_root / 'WinOverview',
)

ov_win32_libs = [
  cc.find_library('user32', required: true),
  cc.find_library('gdi32', required: true),
  cc.find_library('dwmapi', required: true),
  cc.find_library('shell32', required: true),
  cc.find_library('shlwapi', required: true),
  cc.find_library('psapi', required: true),
]

# Overview rendering DLL (injected into RuntimeBroker.exe).
# The launcher loads it by the literal name "WinOverview.dll".
shared_library('WinOverview',
  files(
    extracted_root / 'WinOverviewLibrary' / 'dllmain.cpp',
    extracted_root / 'WinOverviewLibrary' / 'workspace.cpp',
    extracted_root / 'WinOverviewLibrary' / 'helpers.cpp',
    extracted_root / 'WinOverviewLibrary' / 'pch.cpp',
  ),
  dependencies: ov_win32_libs,
  include_directories: ov_lib_inc,
  cpp_args: ['-DWIN32_LEAN_AND_MEAN', '-DUNICODE', '-D_UNICODE'],
  # Statically link the MinGW runtimes so the DLL only depends on system DLLs:
  # the DLL is loaded inside RuntimeBroker.exe, whose DLL search path (inherited
  # from explorer.exe) never sees the MSYS2 bin/ directory. Keep both libstdc++
  # and winpthread under -Bstatic so libstdc++'s pthread mutex stubs resolve to
  # the static winpthread instead of the runtime DLL.
  link_args: ['-static-libgcc', '-Wl,-Bstatic', '-lstdc++', '-lwinpthread',
              '-Wl,-Bdynamic'],
  name_prefix: '',
  install: false,
)

# Launcher executable (started inside explorer.exe by the host).
# win_subsystem 'windows' is required: meson otherwise appends
# -Wl,--subsystem,console after -mwindows, making the launcher a console app
# whose window pops up on every overview trigger.
executable('WinOverviewLauncher',
  files(
    extracted_root / 'WinOverviewLauncher' / 'WinOverviewLauncher.cpp',
  ),
  dependencies: ov_win32_libs,
  include_directories: ov_lib_inc,
  cpp_args: ['-DWIN32_LEAN_AND_MEAN', '-DUNICODE', '-D_UNICODE'],
  link_args: ['-municode'],
  win_subsystem: 'windows',
  install: false,
)
