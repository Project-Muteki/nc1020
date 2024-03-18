project('nc1020', 'cpp',
  version : '0.0.1',
  default_options : ['warning_level=3', 'cpp_std=c++14'])

cpp = meson.get_compiler('cpp')

add_project_arguments(cpp.get_supported_arguments([
    '-fno-exceptions',
    '-fno-rtti',
]), language: 'cpp')

include_dir = include_directories('include')

executable('nc1020',
           'src/main.cpp',
           'src/nc1020.cpp',
           install : false,
           include_directories : include_dir)
