project('winome', ['c', 'cpp'],
  version: '0.1.0',
  meson_version: '>= 1.3.0',
  default_options: [
    'cpp_std=c++17',
    'c_std=c11',
    'warning_level=2',
    'buildtype=debugoptimized',
  ],
  license: 'GPL-2.0-only',
)

gtk4_dep = dependency('gtk4', version: '>= 4.0')
fontconfig_dep = dependency('fontconfig')
pangoft2_dep = dependency('pangoft2')

cc = meson.get_compiler('cpp')

subdir('st-compat')
subdir('overview')
subdir('src')
