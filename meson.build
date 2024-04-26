project('nc1020', 'cpp',
  version : '0.0.1',
  default_options : ['warning_level=3', 'cpp_std=c++14'])

cpp = meson.get_compiler('cpp')
elf2bestape = find_program('elf2bestape')

add_project_arguments(cpp.get_supported_arguments([
    '-fno-exceptions',
    '-fno-rtti',
]), language: 'cpp')

include_dir = include_directories('include')

elf = executable('nc1020',
    'src/main.cpp',
    'src/nc1020.cpp',
    name_suffix: 'elf',
    install: false,
    include_directories: include_dir)

custom_target('nc1020-bestape',
    input: elf,
    output: 'nc1020.exe',
    command: [elf2bestape, '-o', '@OUTPUT@', '@INPUT@'],
    build_by_default: true)
