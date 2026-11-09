#!/usr/bin/env python3
"""After build.bat, run: python tests/editor_checks.py.

Builds and runs native editor regression checks without opening a window or
replacing an editor executable that may currently be running.
"""
from pathlib import Path
import subprocess
import sys

SOURCE = Path(__file__).resolve().parents[1]


def run(args):
    subprocess.run([str(arg) for arg in args], cwd=SOURCE, check=True)


def main():
    compiler = SOURCE / 'tools/llvm-mingw/bin/i686-w64-mingw32-clang++.exe'
    ninja = SOURCE / 'tools/ninja/ninja.exe'
    manifest = SOURCE / 'build.ninja'
    if not compiler.is_file() or not ninja.is_file() or not manifest.is_file():
        sys.exit('Run build.bat first to install the tools and configure the build.')
    lines = manifest.read_text().splitlines()
    editor = next(line for line in lines if line.startswith('build build/roll_away_editor.exe: link '))
    excluded = {f'build/obj/src/editor/{name}.o' for name in ('editor_main', 'editor', 'editor_view')}
    objects = [obj for obj in editor.split(': link ', 1)[1].split() if obj not in excluded]
    libraries = next(line for line in lines if line.startswith('libs = ')).split(' = ', 1)[1].split()
    run([ninja, *objects])
    (SOURCE / 'build/test').mkdir(parents=True, exist_ok=True)
    flags = ['-O2', '-Wall', '-Wextra', '-std=gnu++17', '-static', '-DNOMINMAX',
             '-DWIN32_LEAN_AND_MEAN', '-DUNICODE', '-D_UNICODE', '-Isrc',
             '-Ithird_party/imgui', '-Ithird_party/libchdr/include']
    core = ['src/editor/level_model.cpp', 'build/obj/src/level/level_file.o',
            'build/obj/src/platform/files.o']
    for name in ('test_placement_settings', 'test_switch_chains', 'test_editor_strokes'):
        print(f'Checking {name}', flush=True)
        output = f'build/test/{name}.exe'
        inputs = objects if name == 'test_editor_strokes' else core
        libs = libraries if name == 'test_editor_strokes' else []
        run([compiler, *flags, f'tests/{name}.cpp', *inputs, '-o', output, *libs])
        run([SOURCE / output])


if __name__ == '__main__':
    main()
