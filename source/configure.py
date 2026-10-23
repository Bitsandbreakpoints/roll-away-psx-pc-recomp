#!/usr/bin/env python3
"""Prepares the build of the Roll Away PC port (32-bit Windows, llvm-mingw).

    python configure.py [--debug] [--disc <your Roll Away disc image>]

1. Builds build/tools/discdump.exe and copies the game's executable out
   of your disc images into build/disc/: SLUS_007.24 of Roll Away (USA),
   SCES_010.00 of Kula World (Europe), SCPS_100.64 of KulaQuest (Japan).
2. Recompiles each to C (recomp/recomp.py -> build/gen/), one module per
   version; build/gen/modules.c lists the modules of this build.
3. Writes build.ninja for the program itself.

At least one version is needed; the program then runs the versions whose
code it contains.  Disc images (.chd, .cue/.bin or .iso) are found with
--disc (any number of times), the RA_DISC environment variable, or by
looking for "Roll Away*", "Kula World*" and "KulaQuest*" images in the
project folder, the one above it and orig/.  Nothing from the discs is
kept outside build/.
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
# the level editor: a program of its own (src/editor/), next to the game
EDITOR_OUT = 'build/roll_away_editor.exe'
EDITOR_DIR = 'src/editor'
EDITOR_SHARED = ['src/level/level_file.c', 'src/platform/files.cpp', 'src/disc/disc.cpp', 'src/disc/sha1.c']
# Dear ImGui (MIT): core + Win32/D3D11 backends
EXTRA_SOURCES = ['third_party/imgui/' + f for f in (
    'imgui.cpp', 'imgui_draw.cpp', 'imgui_tables.cpp', 'imgui_widgets.cpp',
    'backends/imgui_impl_win32.cpp', 'backends/imgui_impl_dx11.cpp')]
# libchdr (BSD-3) with zstd, LZMA, miniz, FLAC: one unity file, own flags
CHDR_DIR = 'third_party/libchdr'
CHDR_SOURCES = [CHDR_DIR + '/unity.c', CHDR_DIR + '/src/libchdr_codec_avhuff.c']
# (deflate too: custom levels are packed as the game's .PAK entries)
CHDR_CFLAGS = ('-O2 -w -std=gnu11 -DMINIZ_DEFLATE_APIS ' + ' '.join('-I' + CHDR_DIR + '/' + d for d in (
    'include', 'deps/lzma-26.02/include', 'deps/miniz-3.1.2', 'deps/zstd-1.5.7')))
# the versions: executable on the disc, recompiler configuration, title,
# disc image names
MODULES = [
    ('SLUS_007.24', 'recomp/config/main.cfg', 'Roll Away (USA)', ['Roll*Away*']),
    ('SCES_010.00', 'recomp/config/eu.cfg', 'Kula World (Europe)', ['Kula*World*']),     # PAL
    ('SCPS_100.64', 'recomp/config/jp.cfg', 'KulaQuest (Japan)', ['Kula*Quest*']),
]
DISC_EXTENSIONS = ['.chd', '.cue', '.iso', '.bin']
# files that resources.rc builds into the exe
RESOURCE_FILES = ['assets/launcher/background.png', 'assets/icon/roll_away.ico', 'src/resources.h']
LIBS = ['d3d11', 'dxgi', 'd3dcompiler_47', 'xinput', 'ole32', 'user32', 'gdi32', 'shell32',
        'comdlg32', 'winmm', 'dwmapi']


def rel(path):
    return os.path.relpath(path, HERE).replace('\\', '/')


def candidate_discs(patterns):
    """disc images that may hold a version: given ones first, then by name"""
    found = [sys.argv[i + 1] for i, a in enumerate(sys.argv[:-1]) if a == '--disc']
    if os.environ.get('RA_DISC'):
        found.append(os.environ['RA_DISC'])
    for saved in glob.glob(os.path.join(HERE, 'build', 'disc_path*.txt')):
        path = open(saved, encoding='utf-8').read().strip()
        if os.path.exists(path):
            found.append(path)
    for folder in (HERE, os.path.dirname(HERE), os.path.join(HERE, 'orig')):
        for pattern in patterns:
            for ext in DISC_EXTENSIONS:
                found += sorted(glob.glob(os.path.join(folder, pattern + ext)))
    return list(dict.fromkeys(found))


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


def module_name(cfg):
    return [l.split()[1] for l in open(os.path.join(HERE, cfg)) if l.startswith('name ')][0]


def extract_and_recompile():
    """The versions this build contains: [(exe name, cfg, title)]"""
    disc_dir = os.path.join(HERE, 'build', 'disc')
    os.makedirs(disc_dir, exist_ok=True)
    present = []
    for name, cfg, title, patterns in MODULES:
        target = os.path.join(disc_dir, name)
        if not os.path.exists(target) or '--disc' in sys.argv:
            exe = build_discdump()
            for disc in candidate_discs(patterns):
                result = subprocess.run([exe, disc, disc_dir, '/' + name + ';1'], cwd=HERE,
                                        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
                if result.returncode == 0:
                    print(f'{title}: {disc}')
                    open(os.path.join(HERE, 'build', f'disc_path_{module_name(cfg)}.txt'), 'w',
                         encoding='utf-8').write(disc)
                    break
        if os.path.exists(target):
            present.append((name, cfg, title))
        else:
            print(f'{title}: no disc image found (that version is left out)')
    if not present:
        sys.exit('A disc image of the game is needed to build (the code is made from its '
                 'executable):\n    build.bat --disc "C:/Games/Roll Away (USA).chd"\n'
                 '(Roll Away (USA), Kula World (Europe) or KulaQuest (Japan)).')
    gen = []
    for name, cfg, _ in present:
        run([sys.executable, 'recomp/recomp.py', 'build/disc/' + name, cfg, 'build/gen'])
        files = open(os.path.join(HERE, 'build', 'gen', module_name(cfg) + '_files.txt')).read().split()
        gen += ['build/gen/' + f for f in files]
    # the list of modules for the runtime
    text = ['/* GENERATED by configure.py: the versions whose code this build contains */',
            '#include "runtime/runtime.h"', '']
    for _, cfg, _ in present:
        mod = module_name(cfg)
        text += [f'extern const RtFunction g_{mod}_functions[];',
                 f'extern const int g_{mod}_function_count;', f'extern const char g_{mod}_sha1[];']
    text += ['', 'const RtModule g_rt_modules[] = {']
    for name, cfg, title in present:
        mod = module_name(cfg)
        pal = int(title.endswith('(Europe)'))
        text.append(f'    {{ "{name}", "{title}", g_{mod}_functions, &g_{mod}_function_count, g_{mod}_sha1, {pal} }},')
    text += ['};', f'const int g_rt_module_count = {len(present)};', '']
    path = os.path.join(HERE, 'build', 'gen', 'modules.c')
    if not os.path.exists(path) or open(path).read() != '\n'.join(text):
        open(path, 'w', newline='\n').write('\n'.join(text))
    return gen + ['build/gen/modules.c']


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
    sources = [p for p in sources if not rel(p).startswith(EDITOR_DIR + '/')]
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
        + ' '.join(cfg for _, cfg, _, _ in MODULES),
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
    # the level editor: its own sources, the shared ones (objects of the game's build), ImGui, libchdr
    editor_objs = []
    for src in sorted(glob.glob(os.path.join(HERE, EDITOR_DIR, '*.cpp'))):
        r = rel(src)
        obj = 'build/obj/' + os.path.splitext(r)[0] + '.o'
        lines.append(f'build {obj}: cxx {r}')
        editor_objs.append(obj)
    for r in EDITOR_SHARED + EXTRA_SOURCES + CHDR_SOURCES:
        editor_objs.append('build/obj/' + os.path.splitext(r)[0] + '.o')
    lines.append(f'build build/obj/{EDITOR_DIR}/editor.res.o: rc {EDITOR_DIR}/editor.rc | assets/icon/roll_away.ico src/resources.h')
    editor_objs.append(f'build/obj/{EDITOR_DIR}/editor.res.o')
    lines.append(f'build {EDITOR_OUT}: link {" ".join(editor_objs)}')
    lines.append(f'default {OUT} {EDITOR_OUT}')
    open(os.path.join(HERE, 'build.ninja'), 'w', newline='\n').write('\n'.join(lines) + '\n')
    print(f'wrote build.ninja ({len(sources)} sources, {len(gen_sources)} recompiled files)')


if __name__ == '__main__':
    main()
