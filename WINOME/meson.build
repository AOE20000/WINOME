project('winome', ['c', 'cpp'],
  version: '0.1.4',
  meson_version: '>= 1.3.0',
  default_options: [
    'cpp_std=c++17',
    'c_std=c11',
    'warning_level=2',
    'buildtype=debugoptimized',
  ],
  license: 'GPL-2.0-only',
)

ver_conf = configuration_data()
ver_conf.set_quoted('WINOME_VERSION', meson.project_version())
configure_file(output: 'winome-version.h', configuration: ver_conf)
winome_root_inc = include_directories('.')

gtk4_dep = dependency('gtk4', version: '>= 4.0')
fontconfig_dep = dependency('fontconfig')
pangoft2_dep = dependency('pangoft2')
sqlite3_dep = dependency('sqlite3')

cc = meson.get_compiler('cpp')

subdir('st-compat')
subdir('src')
subdir('ctl')
subdir('tests')
