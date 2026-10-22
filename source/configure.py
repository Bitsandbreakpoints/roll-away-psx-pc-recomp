#!/usr/bin/env python3
"""Prepares the build of the Roll Away PC port (32-bit Windows, llvm-mingw).

    python configure.py [--debug] [--disc <your Roll Away disc image>]

1. Builds build/tools/discdump.exe and copies the game's executable
   (SLUS_007.24) out of your disc image into build/disc/.
2. Recompiles it to C (recomp/recomp.py -> build/gen/).
3. Writes build.ninja for the program itself.

The disc image (.chd, .cue/.bin or .iso) is found with --disc, the RA_DISC
environment variable, or by looking for "Roll Away*" images in the
project folder and the one above it.  Nothing from the disc is kept
outside build/.
"""
import glob
import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
TOOLS = os.path.join(HERE, 'tools')
BIN = os.path.join(TOOLS, 'llvm-mingw', 'bin')
CC = os.path.join(BIN, 'i686-w64-mingw32-clang.exe')
CXX = os.path.join(BIN, 'i686-w64-mingw32-clang++.exe')
WINDRES = os.path.join(BIN, 'i686-w64-mingw32-windres.exe')

OUT = 'build/roll_away.exe'
SOURCE_DIRS = ['src']
# Dear ImGui (MIT): core + Win32/D3D11 backends
EXTRA_SOURCES = ['third_party/imgui/' + f for f in (
    'imgui.cpp', 'imgui_draw.cpp', 'imgui_tables.cpp', 'imgui_widgets.cpp',
    'backends/imgui_impl_win32.cpp', 'backends/imgui_impl_dx11.cpp')]
# libchdr (BSD-3) with zstd, LZMA, miniz, FLAC: one unity file, own flags
CHDR_DIR = 'third_party/libchdr'
CHDR_SOURCES = [CHDR_DIR + '/unity.c', CHDR_DIR + '/src/libchdr_codec_avhuff.c']
CHDR_CFLAGS = ('-O2 -w -std=gnu11 ' + ' '.join('-I' + CHDR_DIR + '/' + d for d in (
    'include', 'deps/lzma-26.02/include', 'deps/miniz-3.1.2', 'deps/zstd-1.5.7')))
# the executables on the disc and their recompiler configurations
MODULES = [('/SLUS_007.24;1', 'SLUS_007.24', 'recomp/config/main.cfg')]
DISC_PATTERNS = ['Roll*Away*.chd', 'Roll*Away*.cue', 'Roll*Away*.iso', 'Roll*Away*.bin']
# files that resources.rc builds into the exe
RESOURCE_FILES = ['assets/launcher/background.png', 'assets/icon/roll_away.ico', 'src/resources.h']
LIBS = ['d3d11', 'dxgi', 'd3dcompiler_47', 'xinput', 'ole32', 'user32', 'gdi32', 'shell32',
        'comdlg32', 'winmm', 'dwmapi']


def rel(path):
    return os.path.relpath(path, HERE).replace('\\', '/')


def find_disc():
    if '--disc' in sys.argv:
        return sys.argv[sys.argv.index('--disc') + 1]
    if os.environ.get('RA_DISC'):
        return os.environ['RA_DISC']
    saved = os.path.join(HERE, 'build', 'disc_path.txt')
    if os.path.exists(saved):
        path = open(saved, encoding='utf-8').read().strip()
        if os.path.exists(path):
            return path
    for folder in (HERE, os.path.dirname(HERE), os.path.join(HERE, 'orig')):
        for pattern in DISC_PATTERNS:
            found = sorted(glob.glob(os.path.join(folder, pattern)))
            if found:
                return found[0]
    return None


def run(cmd):
    result = subprocess.run(cmd, cwd=HERE)
    if result.returncode:
        sys.exit(f'failed: {" ".join(cmd)}')


def build_discdump():
    """The small tool that reads files from the disc image (with libchdr)."""
    exe = os.path.join(HERE, 'build', 'tools', 'discdump.exe')
    sources = ['tools/discdump/discdump.c', 'src/disc/disc.cpp', 'src/disc/sha1.c'] + CHDR_SOURCES
    newest = max(os.path.getmtime(os.path.join(HERE, s)) for s in sources)
    if os.path.exists(exe) and os.path.getmtime(exe) >= newest:
        return exe
    os.makedirs(os.path.join(HERE, 'build', 'tools', 'obj'), exist_ok=True)
    objs = []
    for s in sources:
        obj = os.path.join('build', 'tools', 'obj', os.path.basename(s) + '.o')
        if s.startswith(CHDR_DIR):
            flags = CHDR_CFLAGS.split()
        else:
            flags = ['-O2', '-Isrc', '-I' + CHDR_DIR + '/include', '-DWIN32_LEAN_AND_MEAN']
        compiler = CXX if s.endswith('.cpp') else CC
        print(f'discdump: compiling {s}')
        run([compiler] + flags + ['-c', s, '-o', obj])
        objs.append(obj)
    run([CXX, '-static'] + objs + ['-o', exe])
    return exe


def extract_and_recompile():
    disc_dir = os.path.join(HERE, 'build', 'disc')
    have = all(os.path.exists(os.path.join(disc_dir, name)) for _, name, _ in MODULES)
    disc = find_disc()
    if not have or disc and '--disc' in sys.argv:
        if not disc:
            sys.exit("Your Roll Away (USA) disc image is needed to build (the code is made "
                     "from its executable):\n    build.bat --disc \"C:/Games/Roll Away (USA).chd\"")
        print(f'disc: {disc}')
        os.makedirs(disc_dir, exist_ok=True)
        exe = build_discdump()
        run([exe, disc, disc_dir] + [path for path, _, _ in MODULES])
        open(os.path.join(HERE, 'build', 'disc_path.txt'), 'w', encoding='utf-8').write(disc)
    for _, name, cfg in MODULES:
        run([sys.executable, 'recomp/recomp.py', 'build/disc/' + name, cfg, 'build/gen'])
    gen = []
    for _, _, cfg in MODULES:
        mod = [l.split()[1] for l in open(os.path.join(HERE, cfg)) if l.startswith('name ')][0]
        files = open(os.path.join(HERE, 'build', 'gen', mod + '_files.txt')).read().split()
        gen += ['build/gen/' + f for f in files]
    return gen


def main():
    debug = '--debug' in sys.argv
    gen_sources = extract_and_recompile()
    # (frame pointers: the crash handler in main_pc.cpp can walk the stack)
    opt = '-O0 -g -fno-omit-frame-pointer' if debug else '-O2 -g -fno-omit-frame-pointer'
    common = (f'{opt} -Wall -Wno-unused-function -Iinclude -Isrc -Ibuild/gen -Ithird_party/imgui '
              '-Ithird_party/libchdr/include -Ithird_party/stb -DWIN32_LEAN_AND_MEAN -DNOMINMAX '
              '-DUNICODE -D_UNICODE -msse2 -mfpmath=sse')
    sources = []
    for d in SOURCE_DIRS:
        for ext in ('c', 'cpp'):
            sources += glob.glob(os.path.join(HERE, d, '**', '*.' + ext), recursive=True)
    sources.sort()
    sources += [os.path.join(HERE, p) for p in EXTRA_SOURCES]
    # the recompiled code: big files of plain register/memory operations
    gen_flags = (f'{"-O0" if debug else "-O2"} -g0 -fwrapv -fno-strict-aliasing -w -Isrc -Ibuild/gen '
                 '-msse2 -mfpmath=sse -std=gnu11')

    lines = [
        '# GENERATED by configure.py',
        f'cflags = {common} -std=gnu11 -fwrapv -fno-strict-aliasing',
        f'cxxflags = {common} -std=gnu++17 -fno-exceptions -fno-rtti',
        f'ldflags = -static -municode -mwindows -Wl,--large-address-aware -Wl,--disable-dynamicbase',
        f'libs = {" ".join("-l" + l for l in LIBS)}',
        '',
        'rule cc',
        f'  command = "{CC}" $cflags -MD -MF $out.d -c $in -o $out',
        '  depfile = $out.d',
        '  deps = gcc',
        '  description = CC $in',
        'rule cxx',
        f'  command = "{CXX}" $cxxflags -MD -MF $out.d -c $in -o $out',
        '  depfile = $out.d',
        '  deps = gcc',
        '  description = CXX $in',
        'rule rc',
        f'  command = "{WINDRES}" -Isrc -i $in -o $out',
        '  description = RC $in',
        'rule link',
        f'  command = "{CXX}" $ldflags $in -o $out $libs',
        '  description = LINK $out',
        'rule configure',
        f'  command = "{sys.executable}" configure.py {" ".join(a for a in sys.argv[1:] if a == "--debug")}',
        '  generator = 1',
        '  description = CONFIGURE (recompiling the executables)',
        '',
        'build build.ninja: configure | configure.py recomp/recomp.py recomp/mips.py recomp/psxexe.py '
        + ' '.join(cfg for _, _, cfg in MODULES),
        '',
    ]
    objs = []
    for src in sources:
        r = rel(src)
        obj = 'build/obj/' + os.path.splitext(r)[0] + '.o'
        rule = 'cxx' if src.endswith('.cpp') else 'cc'
        lines.append(f'build {obj}: {rule} {r}')
        objs.append(obj)
    for r in CHDR_SOURCES:
        obj = 'build/obj/' + os.path.splitext(r)[0] + '.o'
        lines.append(f'build {obj}: cc {r}')
        lines.append(f'  cflags = {CHDR_CFLAGS}')
        objs.append(obj)
    for r in gen_sources:
        obj = 'build/obj/' + os.path.splitext(r)[0] + '.o'
        lines.append(f'build {obj}: cc {r}')
        lines.append(f'  cflags = {gen_flags}')
        objs.append(obj)
    for r in sorted(rel(p) for p in glob.glob(os.path.join(HERE, 'src', '*.rc'))):
        obj = 'build/obj/' + os.path.splitext(r)[0] + '.res.o'
        lines.append(f'build {obj}: rc {r} | {" ".join(RESOURCE_FILES)}')
        objs.append(obj)
    lines.append(f'build {OUT}: link {" ".join(objs)}')
    lines.append(f'default {OUT}')
    open(os.path.join(HERE, 'build.ninja'), 'w', newline='\n').write('\n'.join(lines) + '\n')
    print(f'wrote build.ninja ({len(sources)} sources, {len(gen_sources)} recompiled files)')


if __name__ == '__main__':
    main()
