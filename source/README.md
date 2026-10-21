# Roll Away: PC port (source)

A native Windows version of the PlayStation game *Roll Away* (SLUS-00724,
Psygnosis, 1998; developed by Game Design Sweden AB). No emulator and no
BIOS: the game's own MIPS code is translated to C (static recompilation)
and compiled for x86, and Sony's libraries for graphics, the CD-ROM (with
its XA music and movies), the movie decoder (MDEC), controllers, the memory
card and interrupts are replaced by PC code.

The game plays: the Psygnosis movie, the title, the menus, Arcade mode with
its 3D stages, the demo mode and the memory card (Load Game), with
the XA music from the disc and the sound effects, keyboard or XInput
controllers (two players) and graphics at up to 4x the internal resolution,
an optional 16:9 widescreen mode, bilinear, soft-TV or four CRT filters (CRT, Soft CRT, Composite CRT,
PVM). F1 (or Select+Start) pauses and opens a
menu with the settings and Resume, Back to launcher and Quit game; Tab
fast-forwards and Esc goes back to the launcher.

## You need your own disc
Neither the source nor the program contains graphics, sound, music, movies,
text or other data from the game. Everything is read from your own disc of
*Roll Away (USA)* (`.chd` or `.bin`/`.cue`; an `.iso` works but has no music
or movies, since it holds no XA sectors) while the game runs. Choose it in
the launcher with CHOOSE DISC or drop the file on the window; the launcher
checks that it is the release the port was made from.

The build needs the disc too: the code is translated from the disc's
program (`SLUS_007.24`), and the translated code only ever lands in
`build/`, never in the source.

## Building
Needs Windows 10/11 and Python 3.
```bat
build.bat --disc "D:\Games\Roll Away (USA).chd"
```
The first time, the compiler (llvm-mingw) and ninja are downloaded to
`tools/` (SHA256-checked). The disc's path is remembered, so `build.bat` is
enough after that. The result is `build/roll_away.exe`. `run.bat` builds
and starts it; `build.bat --debug` makes a debug build. A disc image named
`Roll*Away*.chd` (or `.cue`, `.iso`) in this folder is found without
`--disc`.

A package for players: `python tools/package.py` makes
`build/package/roll-away-pc.zip` (exe, README.txt, licences), after
checking that the exe contains nothing from the disc.

## How it works
| Part | Where | What |
|---|---|---|
| Recompiler | `recomp/` | Python: MIPS R3000A + GTE to C, one C function per function of the game, jump tables as `switch` |
| Configuration | `recomp/config/main.cfg` | the program's SHA1, which of Sony's functions are replaced (`hle`) |
| Run-time | `src/runtime/` | CPU registers, memory, interrupts/VSync, the BIOS functions, the replacements for LIBGPU, LIBCD (a real-time CD drive, XA, movie streaming), LIBPRESS, LIBPAD and LIBMCRD, the hardware registers of the sound |
| PS1 hardware in software | `src/psx/` | GPU (with a high-resolution copy), GTE, SPU, MDEC, XA-ADPCM |
| PC | `src/platform/`, `src/launcher/`, `src/disc/` | window, Direct3D 11, XInput, XAudio2, launcher, disc reader (bin/cue/chd/iso) |

The recompiler follows the code from the entry point: every `jal` target,
function pointers built with `lui`/`addiu` or stored in the data, and the
config's `func` lines. Delay slots run before the jump (the condition is
computed first), backward jumps check for interrupts, and jumps through a
register become a `switch` when their table is found (its entries are read
from PS1 memory at run time), otherwise a call by address. Hand-written
routines that return to their caller's caller are followed too.

The game's RAM lies at its own address (0x80000000) in a 32-bit process;
the game is linked to the KUSEG mirror (0x00010000), which reaches the same
RAM through a mask, so a PS1 address is in effect a plain pointer.
Interrupts are taken where the code checks for them (loops) and in VSync();
the game's callbacks run with the registers saved around them.

The CD drive reads in real time (75 or 150 sectors a second): XA audio
sectors of the chosen file and channel go to the XA decoder and are mixed
through the SPU's CD input, movie sectors are put together into frames that
the game's movie player takes from its ring buffer, and the MDEC decodes
them macroblock by macroblock. The memory card in slot 1 is kept in
`roll_away_card1.mcd` next to the program (the raw format emulators use).
The sound library (LIBSPU), the movies' VLC decoding and the game's own
code run as recompiled code.

## Tests
None of the tests use the global keyboard, so the PC can be used while they
run.

- `python tests/play.py <name> <frames> "<buttons>" <frames...>`: a scripted
  run with a montage of pictures in `build/test/<name>/`.
- `python tests/stress.py [frames] [seed]`: random controller input for a
  long time; reports errors in the log.
- `python tests/compare_duckstation.py`: the same screens (title, the
  1 Player menu) in the port and in the original game in DuckStation (put
  a portable DuckStation in `tools/emulator/duckstation` and a BIOS image
  `scph5502.bin` in this folder), with difference pictures.
- `python tests/window_shot.py build/test/launcher.png`: the launcher's
  window.

Debugging with environment variables: `RA_AUTOSTART=1`, `RA_QUIT=1`,
`RA_INPUT=<frame>[-<last>]:<button>,...`, `RA_LEAVE=<frame>`,
`RA_VRAM_DUMP=<frame>,...`, `RA_WAV_DUMP=<file>`, `RA_MUTE=1`,
`RA_NATIVE=1`, `RA_FAST=1`, `RA_SPU_TRACE=1` (see `src/main_pc.cpp`).

See `THIRD_PARTY.md` for the licences.
