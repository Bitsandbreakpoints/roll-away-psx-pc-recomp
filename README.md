![Bits and Breakpoints reComps](images/banner.webp)

<p align="center"><strong>Classic games, recompiled for today. Easy to play, with optional enhancements, for everyone.</strong></p>

# Roll Away PC port

A native Windows version of the PlayStation game **Roll Away**
(Psygnosis, 1998; developed by Game Design Sweden AB). The game's own code
is recompiled to run natively on Windows, with no emulator and no BIOS, and
the port adds sharper graphics, a pause menu and a number of quality-of-life
improvements.

**You need your own copy of the game.** The port reads the graphics, sound,
music, movies, text and data from a disc image that you make from your own
original disc.

<p align="center">
  <img src="images/cover.jpg" alt="Roll Away (USA), PlayStation, SLUS-00724 - front cover" width="320">
  <br>
  <em>The game this port needs: Roll Away (USA), SLUS-00724. Cover © its respective owners.</em>
</p>

## Download

**[⬇ Download the latest release (roll-away-pc.zip)](https://github.com/Bitsandbreakpoints/roll-away-psx-pc-recomp/releases/latest/download/roll-away-pc.zip)**

All releases are on the [Releases page](https://github.com/Bitsandbreakpoints/roll-away-psx-pc-recomp/releases).

1. Download `roll-away-pc.zip` and unzip it anywhere.
2. Start `roll_away.exe` (Windows 10 or later; no installation).
3. Click **CHOOSE DISC** and pick your disc image, or drop it on the window.
   `.bin`/`.cue` and `.chd` work (an `.iso` works too, but without music
   and movies: it holds no XA sectors). The launcher checks that the image
   is *Roll Away (USA)*.
4. Click **PLAY**.

![The launcher](images/launcher.png)

## Pictures

![The Psygnosis intro movie](images/intro.png)

![The title screen](images/title.png)

![A stage](images/level.png)

![Widescreen (16:9)](images/widescreen.png)

![The pause menu](images/pause-menu.png)

*In-game pictures: Roll Away running in this port (4× internal resolution,
soft-TV filter; one in the widescreen mode). The game's pictures shown here are © their respective
owners.*

## Disclaimer

- **You must own the game.** To play, you need a dump (disc image) of your
  own original *Roll Away (USA)* disc. This project does not provide, and
  will not help you find, game files.
- **This repository contains no game files:** no code, graphics, sound,
  music, movies, text or data files from the game, and no BIOS. It holds
  this README, the third-party licence list, the source code of the port
  (see [Source code](#source-code)) and pictures: our banner, the launcher
  (our own, with a background drawn by a script), plus the box cover and
  in-game pictures, which are © their respective owners and shown only to
  identify the game.
- **No game assets are distributed.** The release contains the port's own
  program and the licences of the open-source libraries it uses. Every
  graphic, sound, music track, movie and text is read at run time from your
  own disc image. The program is the game's logic recompiled for Windows;
  it holds none of the game's files or data (before every release, the
  program is searched for every part of the disc's files, and none may be
  found).
- *Roll Away* is a trademark of its owner. This is a non-commercial fan
  project and is not affiliated with or endorsed by Psygnosis, Game Design
  Sweden or Sony.

## Improvements over the original

All of these can be changed under **SETTINGS** (and in the game from the
pause menu).

**Pictures**
- **Higher internal resolution**, up to 4×.
- **Precise vertices**: no PS1 "wobble", perspective-correct textures.
- **Picture filters**: sharp pixels, bilinear, CRT, or a soft-TV filter
  (GTU-like).
- **Widescreen (16:9)**, optional: the 3D world is seen wider and keeps its
  proportions (the menus and the flat HUD pictures are stretched; the
  movies stay 4:3).
- **Window or borderless full screen**, any window size, 4:3, 16:9 or
  stretched, vsync.

**Sound**
- **Separate volumes** for the music (the disc's XA tracks) and the sound
  effects.

**Comfort**
- **Pause menu** with Select + Start (or F1): resume, back to the launcher,
  quit the game, and every setting.
- **Fast forward**: hold Tab.
- **No loading pauses**: files are read at once (music and movies still
  stream in real time, as on the console).
- **Launcher**: a resizable splash window that remembers its size; choose
  the disc and have it checked, change every setting, with mouse, keyboard
  or controller.
- **Controls**: keyboard or XInput controllers (Xbox and compatible), two
  players, every button remappable, automatic mapping.
- **Disc images**: `.bin`/`.cue`, `.chd` (compressed) and `.iso`.
- **Memory card in a file**: `roll_away_card1.mcd` next to the program, in
  the raw format emulators use. No BIOS needed.

## Controls

Plug-and-play with modern controllers!

| PS1 | Keyboard | Controller |
|---|---|---|
| D-pad | Arrow keys | D-pad or left stick |
| Cross | X | A |
| Circle | C | B |
| Square | Z | X |
| Triangle | S | Y |
| L1 / R1 | Q / E | LB / RB |
| L2 / R2 | 1 / 3 | LT / RT |
| Start | Enter | Start |
| Select | Backspace | Back |

Everything can be changed under **SETTINGS → CONTROLS**, also for player 2.

In the game: **Select + Start** (or **F1**) pauses and opens the menu,
**Tab** fast-forwards, **Esc** goes back to the launcher.

## Files

Everything stays in the program's folder:

- `settings.ini`: the settings.
- `roll_away_card1.mcd`: the memory card in slot 1.
- `roll_away.log`: a log for troubleshooting.

## FAQ

**Do I need a BIOS?** No. Everything the BIOS and Sony's libraries do on a
PlayStation is written anew in the port.

**Where are my saves?** On the memory card file `roll_away_card1.mcd`. It
is the format DuckStation and other emulators use, so a card from an
emulator can be copied in (rename it).

**Which version of the game?** *Roll Away (USA)*, SLUS-00724. The port is
recompiled from that release's code, so the launcher checks the game's
program on the disc; other releases (such as the European *Kula World*)
are not supported.

**Why is there no music with my .iso?** The music and the movies are XA
sectors, which an `.iso` image does not keep. Use a `.bin`/`.cue` or
`.chd` image.

**Is the source code available?** Yes, in [`source/`](source), and it can
be built: see [Source code](#source-code).

## Source code

The [`source/`](source) folder holds the whole port: the recompiler that
turns the game's MIPS code into C, the run-time that runs it (memory,
interrupts, and Sony's libraries for graphics, the CD-ROM with its XA music
and movie streaming, the movie decoder, controllers, the memory card and
the BIOS written anew), the PlayStation's graphics, geometry, sound and
movie chips in software, the launcher and the window, picture and sound
output, plus the build scripts, the tests and the open-source libraries it
uses.

The game's code is not in the repository. It is recompiled when you build,
from the program on **your own disc**:

```bat
source\build.bat --disc "D:\Games\Roll Away (USA).chd"
```

The first build downloads the compiler (llvm-mingw) and ninja; Python 3 is
needed. See [source/README.md](source/README.md).

## Credits

- Built with [Dear ImGui](https://github.com/ocornut/imgui) and
  [libchdr](https://github.com/rtissera/libchdr) (with zstd, LZMA SDK,
  miniz and dr_flac), and [stb_image](https://github.com/nothings/stb).
  Their licences are in [THIRD_PARTY.md](THIRD_PARTY.md) and in the
  `licenses` folder of the release.
