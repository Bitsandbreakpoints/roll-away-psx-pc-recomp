# Roll Away: PC port

A native Windows version of the PS1 game *Roll Away* (SLUS-00724,
Psygnosis 1998, developed by Game Design Sweden AB), known in Europe as
*Kula World* (SCES-01000, PAL) and in Japan as *KulaQuest* (SCPS-10064):
all three releases run. No emulator: the game's own MIPS code is
translated (static recompilation) to C and compiled for x86, and Sony's
libraries for graphics, the CD (with XA music and movies), the movie
decoder (MDEC), the controller, the memory card and the interrupts are
replaced with PC code. No BIOS is needed.

**Status:** the game is fully playable: the Psygnosis movie, the title,
the menus, Arcade with the levels in 3D, Time Trial, the demo and Load Game
from the memory card, with the XA music from the disc and the sound
effects, keyboard or XInput controllers (two players) and graphics at up
to 4× resolution, bilinear filtering, a soft TV filter or four CRT filters
(CRT, Soft CRT, Composite CRT, PVM), an optional widescreen mode (16:9),
true colour (no dithering), detailed distant textures and anti-aliasing
(FXAA). F1 (or
Select+Start) pauses and opens a menu with the settings and the buttons
Resume, Back to launcher and Quit game; Tab fast-forwards and Esc goes back
to the launcher. See `PLAN.md`.

## Level editor
`build/roll_away_editor.exe` (LEVEL EDITOR in the launcher) is a separate
program for making levels in 3D: blocks that snap to the side you click
(one click, one block), every kind of block (fire, ice, invisible, acid,
moving, crumbling, flashing, lasers) and everything that can lie on the
blocks (start, keys, exit, coins, gems, fruit, power-ups, hazards,
transporters, buttons, enemies). Each world's own block textures are read
from the disc. The game's rules are checked as you build. A level is saved
as `.rlv` with its type (a normal level or a bonus level; without the type
the game makes a level without keys a bonus level). "Play" (F5) starts the
game straight into the level, and in the launcher PLAY OWN LEVEL (or a
`.rlv` dropped on the window) plays a saved level. File > Open a level of
the game opens the game's own levels from the disc. On an exit you choose
the level that comes next (also in another world), so your own levels can
make up a whole series.

The game itself is not changed: when a level of your own is played, the
port replaces the contents of the worlds' `.PAK` files as they are read
from the disc (every level becomes yours), chooses the level's world and
presses X through the intro and the menus until the game is on.

## You need your own disc
Neither the source code nor the program contains graphics, sound, music,
movies, text or other data from the game. Everything is read at run time
from your own disc of *Roll Away (USA)*, *Kula World (Europe)* or
*KulaQuest (Japan)*, as a `.chd` or `.bin`/`.cue` image. **`.iso` images
are not supported**: they hold no XA sectors (the movies and the music),
so the screen stays black. Choose the image in the launcher with
"CHOOSE DISC" or drop the file on the window; the launcher sees which
release it is and checks that it is the one the port was made from.

The build needs the disc too: the code is translated from the disc's
executable (`SLUS_007.24`, `SCES_010.00`, `SCPS_100.64`; one module per
release), and the translated code only ever lands in `build/`, never in
the source code. The build includes the releases whose discs it finds (at
least one).

## Building
Requirements: Windows 10/11 and Python 3.
```bash
build.bat --disc "D:\Games\Roll Away (USA).chd" --disc "D:\Games\Kula World (Europe).chd"
```
The first time, the compiler (llvm-mingw) and ninja are downloaded to
`tools/` (SHA256-checked). The discs' paths are remembered, so after that
`build.bat` is enough. The result is `build/roll_away.exe` and
`build/roll_away_editor.exe`. `run.bat` builds and starts; `build.bat
--debug` makes a debug build. Discs named `Roll*Away*`, `Kula*World*` or
`Kula*Quest*` (`.chd`, `.cue`) in the project folder are found without
`--disc`.

Package for players: `python tools/package.py` makes
`build/package/roll-away-pc.zip` (the game, the editor, README.txt,
licences), after checking that the executables contain nothing from the
disc.

## How it works
| Part | Where | What |
|---|---|---|
| Recompiler | `recomp/` | Python: MIPS R3000A + GTE to C, one C function per game function, jump tables as `switch` |
| Configuration | `recomp/config/main.cfg`, `eu.cfg`, `jp.cfg` | SHA1, which Sony functions are replaced (`hle`), signature names, hooks |
| Run-time | `src/runtime/` | CPU registers, memory, interrupts/VSync, BIOS functions, replacements for LIBGPU, LIBCD (a real-time CD drive, XA, St* streaming), LIBPRESS, LIBPAD, LIBMCRD |
| PS1 hardware in software | `src/psx/` | GPU (with a high-resolution copy), GTE, SPU, MDEC, XA-ADPCM |
| PC | `src/platform/`, `src/launcher/`, `src/disc/` | window, Direct3D 11, XInput, XAudio2, launcher, disc reader (bin/cue/chd) |
| Your own levels | `src/level/`, `src/runtime/custom_level.c`, `src/editor/` | the `.rlv` format, the level in the game, the level editor |

The game's RAM lies at its own address (0x80000000) in a 32-bit process,
and the game (linked to KUSEG, 0x00010000) reaches it through a mask, so a
PS1 address is in practice a plain pointer. The sound libraries (LIBSPU),
the VLC decoding of the movies and the game's own code run as translated
code.

## Tests
None of the tests use the global keyboard; the computer can be used while
they run.

- `python tests/play.py <name> <frames> "<buttons>" <frames…>`: a scripted
  run with a picture montage in `build/test/<name>/`.
- `python tests/stress.py [frames] [seed] [disc image]`: random input for a
  long time, looking for FATAL and unmapped memory accesses in the log.
- `python tests/compare_duckstation.py`: the same screens (title, the
  1 Player menu) in the port and in the original in DuckStation
  (`tools/emulator/duckstation`, BIOS `scph5502.bin` in the project
  folder), with difference pictures.
- `python tests/window_shot.py build/test/launcher.png`: the launcher's window.
- `python tests/game_shots.py <folder> <from s> <to s> [RA_...=…] [ini:key=value]`:
  the game's window (filters, widescreen, the pause menu) every half
  second, e.g. the README pictures (`ini:window_mode=0 ini:window_width=1280
  ini:window_height=720 ini:aspect=2`).
- `python tests/editor_shot.py <out.png> [level]` and
  `python tests/editor_drive.py [level] "click X Y; drag …; key 1; shot out.png"`:
  the level editor's window, driven with window messages.

Debugging with environment variables: `RA_AUTOSTART=1`, `RA_QUIT=1`,
`RA_INPUT=<frame>[-<last>]:<button>,…`, `RA_LEAVE=<frame>`,
`RA_VRAM_DUMP=<frame>,…`, `RA_WAV_DUMP=<file>`, `RA_MUTE=1`,
`RA_NATIVE=1`, `RA_FAST=1`, `RA_FRAME_LOG=1`, `RA_SPU_TRACE=1` (see
`src/main_pc.cpp`).

See `THIRD_PARTY.md` for the licences.
