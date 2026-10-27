# Roll Away: PC-port

En native Windows-version av PS1-spelet *Roll Away* (SLUS-00724,
Psygnosis 1998, utvecklat av Game Design Sweden AB), i Europa *Kula World*
(SCES-01000, PAL) och i Japan *KulaQuest* (SCPS-10064): alla tre
utgåvorna går. Ingen emulator:
spelets egen MIPS-kod översätts (statisk recompilering) till C och
kompileras för x86, och Sonys bibliotek för grafik, CD (med XA-musik och
filmer), filmavkodaren (MDEC), handkontroll, minneskort och avbrott
ersätts med PC-kod. Ingen BIOS behövs.

**Status:** spelet går att spela – Psygnosis-filmen, titeln, menyerna,
Arcade-läget med banorna i 3D, demoläget, Load Game mot minneskortet –
med XA-musiken från skivan och ljudeffekterna, tangentbord eller
XInput-handkontroll (två spelare) och grafik i upp till 4× upplösning,
bilinjär filtrering, mjuk TV eller fyra CRT-filter (CRT, Soft CRT,
Composite CRT, PVM), valfritt widescreen (16:9) true colour (utan dithering) och detaljerade texturer på avstånd. F1 (eller Select+Start)
pausar och öppnar en meny med inställningarna och knapparna Resume, Back
to launcher och Quit game; Tab snabbspolar och Esc går tillbaka till
launchern. Se `PLAN.md`.

## Baneditor
`build/roll_away_editor.exe` (LEVEL EDITOR i launchern) är en egen app för
att göra banor i 3D: block som fäster mot sidan man klickar på (dra för en
rad, ett golv eller en vägg), alla blocktyper (eld, is, osynliga, syra,
rörliga, smulande, blinkande, lasrar) och allt som kan ligga på blocken
(start, nycklar, utgång, mynt, ädelstenar, frukt, power-ups, faror,
teleportrar, knappar, fiender). Världens egna blocktexturer läses från
skivan. Spelets regler kontrolleras medan man bygger. Banan sparas som
`.rlv` med sin typ (vanlig bana eller bonusbana – utan typen gör spelet
en bana utan nycklar till bonusbana); "Play" (F5) startar spelet direkt i banan, och i launchern spelar
PLAY OWN LEVEL (eller en `.rlv` som dras in i fönstret) en sparad bana.
File > Open a level of the game öppnar spelets egna banor från skivan.
På en utgång väljer man vilken bana som kommer härnäst (också i en annan
värld), så att egna banor kan bli en hel serie.

Spelet själv ändras inte: när en egen bana spelas byter porten ut
innehållet i världarnas `.PAK`-filer när de läses från skivan (varje bana
blir den egna), väljer banans värld och trycker X genom intro och menyer
tills spelet är igång.

## Du behöver din egen skiva
Varken källkoden eller programmet innehåller grafik, ljud, musik, filmer,
text eller annan data från spelet. Allt läses från din egen skiva av
*Roll Away (USA)*, *Kula World (Europe)* eller *KulaQuest (Japan)*
(`.chd` eller `.bin`/`.cue`; **`.iso` stöds inte**: den saknar XA-sektorerna
med filmerna och musiken, så skärmen förblir svart) när spelet körs. Välj den i launchern
med "CHOOSE DISC" eller dra in filen i fönstret; launchern ser vilken
utgåva det är och kontrollerar att den är samma som porten gjordes från.

Även bygget behöver skivan: koden översätts från skivans körbara fil
(`SLUS_007.24`, `SCES_010.00`, `SCPS_100.64`; en modul per utgåva) och den
översatta koden hamnar bara i `build/`, aldrig i källkoden. Bygget får med
de utgåvor vars skivor det hittar (minst en).

## Bygga
Krav: Windows 10/11 och Python 3.
```bash
build.bat --disc "D:\Spel\Roll Away (USA).chd" --disc "D:\Spel\Kula World (Europe).chd"
```
Första gången laddas kompilatorn (llvm-mingw) och ninja ner till `tools/`
(SHA256-kontrollerade). Skivornas sökvägar sparas, så därefter räcker
`build.bat`. Resultatet blir `build/roll_away.exe` och
`build/roll_away_editor.exe`. `run.bat` bygger
och startar; `build.bat --debug` ger ett felsökningsbygge. Ligger
skivorna som `Roll*Away*`, `Kula*World*` eller `Kula*Quest*` (`.chd`,
`.cue`, `.iso`) i projektmappen hittas de utan `--disc`.

Paket för spelare: `python tools/package.py` ger
`build/package/roll-away-pc.zip` (spelet, editorn, README.txt, licenser),
efter en kontroll av att exe-filerna inte innehåller något från skivan.

## Hur det fungerar
| Del | Var | Vad |
|---|---|---|
| Recompiler | `recomp/` | Python: MIPS R3000A + GTE → C, en C-funktion per spelfunktion, hopptabeller som `switch` |
| Konfiguration | `recomp/config/main.cfg` | SHA1, vilka Sony-funktioner som ersätts (`hle`), signaturnamn |
| Körmiljö | `src/runtime/` | CPU-register, minne, avbrott/VSync, BIOS-funktioner, ersättningar för LIBGPU, LIBCD (CD-enhet i realtid, XA, St*-strömning), LIBPRESS, LIBPAD, LIBMCRD |
| PS1-hårdvara i mjukvara | `src/psx/` | GPU (med högupplöst kopia), GTE, SPU, MDEC, XA-ADPCM |
| PC | `src/platform/`, `src/launcher/`, `src/disc/` | fönster, Direct3D 11, XInput, XAudio2, launcher, skivläsare (bin/cue/chd/iso) |
| Egna banor | `src/level/`, `src/runtime/custom_level.c`, `src/editor/` | `.rlv`-formatet, banan i spelet, baneditorn |

Spelets RAM ligger på sin egen adress (0x80000000) i en 32-bitarsprocess,
och spelet (länkat till KUSEG, 0x00010000) når det genom en mask, så en
PS1-adress är i praktiken en vanlig pekare. Ljudbiblioteken (LIBSPU),
VLC-avkodningen av filmerna och spelets egen kod körs som översatt kod.

`CLAUDE.md` beskriver tekniken och fallgroparna utförligt.

## Tester
Inga av testerna använder det globala tangentbordet; datorn går att
använda medan de körs.

- `python tests/play.py <namn> <bildrutor> "<knappar>" <bildrutor…>`:
  skriptad körning med bildmontage i `build/test/<namn>/`.
- `python tests/stress.py [bildrutor] [frö]`: slumpad input länge, letar
  efter FATAL och oavbildade minnesåtkomster i loggen.
- `python tests/compare_duckstation.py`: samma skärmar (titel,
  1 Player-menyn) i porten och i originalet i DuckStation
  (`tools/emulator/duckstation`, BIOS `scph5502.bin` i projektroten) med
  differensbilder.
- `python tests/window_shot.py build/test/launcher.png`: launcherns fönster.
- `python tests/game_shots.py <mapp> <från s> <till s> [RA_...=…] [ini:nyckel=värde]`:
  spelets fönster (filter, widescreen, pausmenyn) varje halv sekund,
  t.ex. README-bilderna (`ini:window_mode=0 ini:window_width=1280
  ini:window_height=720 ini:aspect=2`).
- `python tests/editor_shot.py <ut.png> [bana]` och
  `python tests/editor_drive.py [bana] "click X Y; drag …; key 1; shot ut.png"`:
  baneditorns fönster, styrt med fönstermeddelanden.

Felsökning med miljövariabler: `RA_AUTOSTART=1`, `RA_QUIT=1`,
`RA_INPUT=<bildruta>[-<sista>]:<knapp>,…`, `RA_LEAVE=<bildruta>`,
`RA_VRAM_DUMP=<bildruta>,…`, `RA_WAV_DUMP=<fil>`, `RA_MUTE=1`,
`RA_NATIVE=1`, `RA_FAST=1`, `RA_SPU_TRACE=1` (se `src/main_pc.cpp`).

Se `THIRD_PARTY.md` för licenser.
