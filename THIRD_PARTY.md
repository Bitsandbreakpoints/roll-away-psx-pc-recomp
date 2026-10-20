# Third-party code and tools

| Component | Licence | Used for | In the source |
|---|---|---|---|
| llvm-mingw (clang, mingw-w64) | Apache 2.0 with LLVM exception / ZPL and others | compiler | no – downloaded by `tools/setup.py` |
| ninja | Apache 2.0 | build tool | no – downloaded by `tools/setup.py` |
| Dear ImGui 1.92.9b | MIT | launcher and settings | yes, `third_party/imgui/` (licence in `LICENSE.txt`) |
| libchdr (commit 47d7941) | BSD-3-Clause | reading `.chd` disc images | yes, `third_party/libchdr/` (licence in `LICENSE.txt`) |
| – zstd 1.5.7 | BSD-3-Clause (chosen from GPLv2/BSD) | CHD decoding | yes, `third_party/libchdr/deps/zstd-1.5.7/` |
| – LZMA SDK 26.02 (LzmaDec only) | public domain | CHD decoding | yes, `third_party/libchdr/deps/lzma-26.02/` |
| – miniz 3.1.2 | MIT | CHD decoding (zlib) | yes, `third_party/libchdr/deps/miniz-3.1.2/` |
| – dr_flac | public domain / MIT-0 | CHD decoding (FLAC) | yes, inside libchdr |
| stb_image 2.30 | public domain / MIT | decoding the launcher picture (PNG) | yes, `third_party/stb/stb_image.h` |

Code taken over from the earlier PC ports of the same author: from
American Pool the window, Direct3D 11, XInput and XAudio2 layer, the
launcher, the disc reader, the software GPU with its high-resolution
renderer, the GTE, the SPU's mixing core and the memory card; from Air
Hockey and Sorcerer's Maze the recompiler and the run-time; from D the
XA-ADPCM decoder, the MDEC's inverse DCT and the movie streaming.

## Pictures

| File | Author | Licence |
|---|---|---|
| `assets/launcher/background.png` | drawn by `tools/make_background.py` (this project: sky, dunes, a stone track and a striped ball) | as the project |

The picture is built into the exe (`src/resources.rc`).

## Fonts

The PS1 BIOS's font is not in the port. Should the game print with it
(Krom2RawAdd), its characters are drawn with a font installed in Windows
(Arial, MS Gothic), see `src/platform/font_gdi.cpp`. No font is shipped.

The game's own files (code, graphics, sound, data) are not included; they
are read from the player's own disc.
