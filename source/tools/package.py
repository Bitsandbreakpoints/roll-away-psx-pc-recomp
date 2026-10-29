#!/usr/bin/env python3
"""Packs the PC port for players: build/package/roll-away-pc.zip.

    python tools/package.py [--scan-only]

The zip holds roll_away.exe, the level editor (roll_away_editor.exe), the player's README (README.txt) and the
licences of the third-party code built into the exe.  It never holds
anything from the game's disc: before packing, the exe is searched for
every 64-byte block (that is not mostly one repeated value) of the discs'
files - the executables of the three releases in build/disc/ and, if they
are there, the files of each disc in orig/files/ (USA), orig/files_eu/ and
orig/files_jp/ (except the streamed movies and XA music, sound and video
sectors far larger than the exe) - at every offset.  Any hit stops the packing.
(The exe contains the executable's code translated to x86, but none of
its bytes: data and tables are read from the disc when the game runs.)
Settings and logs from test runs never go in (only the files listed here
are packed).  The packed executables are stripped (llvm-strip --strip-all:
no debug information or symbol table, the same code); the ones in build/
keep them for debugging.
"""
import os
import struct
import subprocess
import sys
import zipfile

HERE = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
EXE = os.path.join(HERE, 'build', 'roll_away.exe')
EDITOR_EXE = os.path.join(HERE, 'build', 'roll_away_editor.exe')
OUT_DIR = os.path.join(HERE, 'build', 'package')
ZIP_NAME = 'roll-away-pc.zip'
FOLDER = 'Roll Away PC'
STRIP = os.path.join(HERE, 'tools', 'llvm-mingw', 'bin', 'llvm-strip.exe')

BLOCK = 64
MAX_SCANNED_FILE = 4 * 1024 * 1024  # (the STR movies and XA music: streamed audio/video)
MIN_DISTINCT_BYTES = 12        # skip blocks that are mostly one value (zeros, padding)
DISC_DIRS = ['build/disc', 'orig/files', 'orig/files_eu', 'orig/files_jp']

# Public tables that both the game (its own inflate code) and the port (the
# decompressors in libchdr) contain: the DEFLATE length and distance bases
# of RFC 1951, as 32-bit words with zlib's two closing zeros.  A disc block
# that lies wholly inside one of them is not the game's data.
DEFLATE_LENGTH_BASE = [3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31, 35, 43, 51, 59,
                       67, 83, 99, 115, 131, 163, 195, 227, 258, 0, 0]
DEFLATE_DISTANCE_BASE = [1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193, 257, 385,
                         513, 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385,
                         24577, 0, 0]
PUBLIC_TABLES = [struct.pack(f'<{len(t)}I', *t) for t in (DEFLATE_LENGTH_BASE,
                                                         DEFLATE_DISTANCE_BASE)]

# licence files: (name in the zip, source, None or the text a licence
# block at the end of a single-file library starts with)
LICENSES = [
    ('imgui.txt', 'third_party/imgui/LICENSE.txt', None),
    ('libchdr.txt', 'third_party/libchdr/LICENSE.txt', None),
    ('zstd.txt', 'third_party/libchdr/deps/zstd-1.5.7/LICENSE', None),
    ('lzma.txt', 'third_party/libchdr/deps/lzma-26.02/LICENSE', None),
    ('miniz.txt', 'third_party/libchdr/deps/miniz-3.1.2/miniz.c', 'Copyright 2013-2014 RAD'),
    ('dr_flac.txt', 'third_party/libchdr/include/dr_libs/dr_flac.h', 'This software is available as a choice'),
    ('stb_image.txt', 'third_party/stb/stb_image.h', 'This software is available under 2 licenses'),
]

README = """Roll Away - PC port
===================

A native Windows version of the PlayStation game Roll Away (Psygnosis,
1998; developed by Game Design Sweden AB), made by recompiling the game's
own code. It contains no graphics, sound, music, movies, text or other
files from the game: you need your own copy of the disc (Roll Away (USA),
SLUS-00724) as a disc image.

How to play
-----------
1. Make an image of your own disc: "Roll Away (USA).bin" + ".cue" or a
   .chd. An .iso image is NOT supported: it holds no XA sectors (the
   movies and the music), and the screen stays black.
2. Start roll_away.exe and choose CHOOSE DISC (or drop the image on the
   window). The image is checked against the release the port was made
   from.
3. Choose PLAY.

In the game: Esc goes back to the launcher, F1 (or Select+Start on the
controller) pauses and shows the settings with Resume, Back to launcher and
Quit game, and holding Tab fast-forwards.

Controls (keyboard and XInput controllers, two players), graphics
(internal resolution up to 4x, precise vertices, true colour, detailed distant textures, anti-aliasing (FXAA), an optional 16:9
widescreen mode, picture filters such as
CRT, Soft CRT, Composite CRT and PVM, full screen), sound (separate music and effects volumes) and the
game (a save point after every level; Timeless Arcade: no time limit, no
hourglasses or clocks; Kula World (Europe) at 60 Hz, on by default) are set
under SETTINGS. The settings are kept in settings.ini next to the program.
The game saves to a memory card, kept in roll_away_card1.mcd next to the
program (the format emulators use). No BIOS is needed.

Your own levels
---------------
roll_away_editor.exe (LEVEL EDITOR in the launcher) is a 3D level editor:
put blocks (they snap onto the side you click), the start, keys, the exit,
coins, gems, fruit, power-ups, hazards, transporters, buttons, moving
blocks and lasers, choose the world and the time, and save the level as a
.rlv file. The editor shows the world's own block textures, read from your
disc. "Play" (F5) starts the game with the level at once; in the launcher,
PLAY OWN LEVEL (or dropping a .rlv on the window) plays a saved level.
On an exit you choose the level that comes next (a whole series of your
own levels, in any worlds). File > Open a level of the game opens any of
the game's levels from your
disc to look at or to start from. The game's rules are checked as you
build (one start, things only on open sides, the exit opens when all keys
are taken; choose a normal or a bonus level - a normal level without
keys has its exit open at once).

Requires Windows 10 or later.

The port's own code is under the MIT License (LICENSE.txt). The licences
of the third-party code are in the "licenses" folder.
Roll Away is a trademark of its owner; this project is not affiliated
with or endorsed by Psygnosis, Game Design Sweden or Sony.
"""


def read(path):
    with open(path, 'rb') as f:
        return f.read()


def scan(exe):
    """[(file, offset, exe offset)] of blocks of disc files found in the exe."""
    files = []
    for d in DISC_DIRS:
        for dirpath, _, names in os.walk(os.path.join(HERE, d)):
            files += [os.path.join(dirpath, n) for n in names
                      if os.path.getsize(os.path.join(dirpath, n)) <= MAX_SCANNED_FILE]
    blocks = {}
    for path in files:
        data = read(path)
        for offset in range(0, len(data) - BLOCK + 1, BLOCK):
            block = data[offset:offset + BLOCK]
            if len(set(block)) >= MIN_DISTINCT_BYTES and not any(block in t for t in PUBLIC_TABLES):
                blocks.setdefault(block[:8], {}).setdefault(block, (path, offset))
    print(f'scan: {sum(len(v) for v in blocks.values())} blocks of {len(files)} disc files')
    hits = []
    for i in range(len(exe) - BLOCK + 1):
        candidates = blocks.get(exe[i:i + 8])
        if candidates:
            found = candidates.get(exe[i:i + BLOCK])
            if found:
                hits.append((found[0], found[1], i))
    return hits


def license_text(source, start):
    text = read(os.path.join(HERE, source)).decode('utf-8', 'replace')
    if start:
        text = text[text.index(start):]
    return text


def stripped(path):
    """The executable without its debug information and symbol table
    (~4 MB smaller; the code is the same)."""
    os.makedirs(OUT_DIR, exist_ok=True)
    out = os.path.join(OUT_DIR, os.path.basename(path))
    subprocess.run([STRIP, '--strip-all', '-o', out, path], check=True)
    data = read(out)
    os.remove(out)
    print(f'strip: {os.path.basename(path)} {os.path.getsize(path) // 1024} KB -> {len(data) // 1024} KB')
    return data


def main():
    exe = stripped(EXE)
    editor = stripped(EDITOR_EXE) if os.path.exists(EDITOR_EXE) else None
    for name, data in (('roll_away.exe', exe), ('roll_away_editor.exe', editor)):
        if data is None:
            continue
        hits = scan(data)
        for path, offset, at in hits[:20]:
            print(f'  {os.path.relpath(path, HERE)} +{offset:#x} found at {name} offset {at:#x}')
        if hits:
            sys.exit(f'{len(hits)} blocks of disc files are in {name}: not packed')
        print(f'scan: nothing from the disc in {name}')
    if '--scan-only' in sys.argv:
        return
    os.makedirs(OUT_DIR, exist_ok=True)
    out = os.path.join(OUT_DIR, ZIP_NAME)
    with zipfile.ZipFile(out, 'w', zipfile.ZIP_DEFLATED) as z:
        z.writestr(f'{FOLDER}/roll_away.exe', exe)
        if editor:
            z.writestr(f'{FOLDER}/roll_away_editor.exe', editor)
        z.writestr(f'{FOLDER}/README.txt', README.replace('\n', '\r\n'))
        z.writestr(f'{FOLDER}/LICENSE.txt', read(os.path.join(HERE, 'LICENSE')).replace(b'\r\n', b'\n')
                   .replace(b'\n', b'\r\n'))
        z.writestr(f'{FOLDER}/licenses/THIRD_PARTY.md', read(os.path.join(HERE, 'THIRD_PARTY.md')))
        for name, source, start in LICENSES:
            z.writestr(f'{FOLDER}/licenses/{name}', license_text(source, start))
    print(f'{out} ({os.path.getsize(out) // 1024} KB)')


if __name__ == '__main__':
    main()
