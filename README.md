# Toy Story Racer Recompiled

<!-- retcomm-readme-metrics -->
[![GitHub downloads (all assets, all releases)](https://img.shields.io/github/downloads/IraFunesto/ToyStoryRacerRecomp/total)](https://github.com/IraFunesto/ToyStoryRacerRecomp/releases)
[![GitHub downloads (latest release)](https://img.shields.io/github/downloads/IraFunesto/ToyStoryRacerRecomp/latest/total)](https://github.com/IraFunesto/ToyStoryRacerRecomp/releases/latest)
[![GitHub release](https://img.shields.io/github/v/release/IraFunesto/ToyStoryRacerRecomp)](https://github.com/IraFunesto/ToyStoryRacerRecomp/releases/latest)
<!-- /retcomm-readme-metrics -->

<p align="center">
  <img src="docs/screenshot-ultrawide.png" alt="Toy Story Racer, two players in 32:9" width="900">
</p>

Static recompilation of **Disney•Pixar Toy Story Racer** (PlayStation: the
European Es, It, Nl disc SLES-03398, the American disc SLUS-01214 and the
European Fr, De disc SLES-03397 — any one of them is enough) built on
[psxrecomp](https://github.com/mstan/psxrecomp) and
[recomp-ui](https://github.com/RetroPortingToolKit/recomp-ui). The game runs
as native code on your PC, compiled from your own disc, with a widescreen mod,
a native 3D renderer for the tracks and a set of fixes on top.

<img src="docs/cover.png" alt="Toy Story Racer cover" width="200" align="right">

| | |
|---|---|
| Players | 2 (split screen) |
| Region | PAL (Europe) and NTSC (USA) |
| Publisher | Disney Interactive |
| Year | 2001 |

## What this adds

All enhancements live in the bundled **Toy Story Racer Widescreen** mod
package (launcher → Mods) and can be switched off.

On a first start the game is set up the way it is meant to be played:
widescreen with the native renderer on, the window opening in the shape of
your monitor, 960-line internal resolution with bilinear filtering, and the
framework's PGXP package on. Every one of them can be changed in the
launcher (Settings and Mods); an existing install keeps its own choices.

**Three discs, one game** — the setup takes whichever of your discs you
select (European Es, It, Nl, American, or European Fr, De) and builds that
version; every further disc you set up adds its version:
- *Version* (Display group, at the top): **PAL 50 Hz** plays a European
  program, **NTSC 60 Hz** the American one, with races at 60 frames per
  second. Play starts the chosen version with the same launcher settings,
  mod options and memory cards; if its disc is not set up yet, the disc
  setup opens instead.
- *Set up another disc*: at the next Play the disc setup opens, to add
  another of your discs. A version set up later starts with the settings of
  the one you were using.
- Texts and voices in every language in every version: each program carries
  the texts of all six languages, and the character voices of every disc you
  set up (English from the American disc, Italian, Spanish and Dutch from the
  Es, It, Nl disc, French and German from the Fr, De disc) are copied into
  the other versions' disc folders, so the Quick start language picks both.
  A language whose disc you do not have uses English voices if you have
  them, else the disc's own.
- One memory card for all: every version reads and writes the same save
  (the versions only name it differently), so a game saved in one continues
  in the others.

Nothing of a disc you do not set up is ever used: every file comes from your
own discs.

**The disc as files** — at the first start the game splits your disc image
into the `tsr_data` folder of the install: every data file as a normal file,
the music, voices and videos with their full sectors, and the few sectors no
file owns. Every sector is rebuilt and compared with the image before the
folder is used. From then on everything — boot, loads, music, voices,
videos, and Generate & rebuild — comes from that folder: the image is not
read any more, and is not needed to play. `tsr_data` holds the game's data:
never share it.

**Instant loading** (Fixes group, on by default) — the game's file loads are
answered at once from the folder instead of waiting for the emulated CD
drive to seek and read; the music and voices still stream in real time, as
on the console. The framework's CD Speed and Fast Loading packages are left
out of this build: they are not needed, and Fast Loading would speed the
game itself up while the music streams.

**Instant memory card** (Fixes group, on by default) — checking the card,
loading and saving are instant. The console's BIOS moves one 128-byte sector
every two frames, so checking the card took about 8 seconds and every save a
few more; the game's card calls (Sony's standard card and file functions) are
now done at once on the card file, `saves/card1.mcd`. The card file is written
exactly as the console's BIOS writes it — the same directory, blocks and
data — so it stays a normal memory card image for any emulator.

**Quick start** (Fixes group, on by default) — the game starts at the
language choice, without the Activision, Disney Interactive and Traveller's
Tales logo videos (*Logo videos*: Show plays them). *Language*: English,
Français, Deutsch, Italiano, Español or Nederlands starts straight at the
title screen in that language (texts and voices), without the language
choice and the copyright notice; Ask (default on the European programs)
keeps the game's own choice.

**Widescreen** (fit to window, 16:9, 21:9 or 32:9) — a real wider field of
view, not a stretched picture; Fit to window first opens in the shape of
your monitor:
- tracks and objects are drawn all the way to the screen edges, in one and two
  player races; the cloud sky of the title and level select fills the width
  too;
- the race HUD is anchored to the corners, and so are the prompts of the
  menus, level select, track preview and pause screen (the character select
  prompts keep following the characters' heads);
- *Track draw distance*: the original hides scenery outside the 4:3 view;
  Extended doubles the draw distance, Full (default) also ignores the 4:3
  visibility lists, so nothing pops in at the sides.

**Native scenery renderer** (default on) — all the 3D is drawn by the PC
instead of the emulated PlayStation GPU: the track scenery, the karts and the
characters, the menus' 3D scenes and the title logo, and the effects (smoke
and sparks, shadows, kart aerials, pick-up crates). No wobbling textures or
jittering polygons, correct depth between pieces (a kart's far wheels and
aerial no longer show through it), no polygons cut at the 4:3 edges. The
emulated GPU is left with the 2D HUD, menus and videos. Textures keep their
exact place on every polygon however the camera cuts it (no sliding, no
seams, straight lines on uneven floors), and the race camera and the pre-race
flight are worked out without the game's rounding: no small jolts.

**50 fps races** — the PAL game runs at 25 fps; races can run at 50 fps (one
player by default, two players optional on a fast PC). Game speed and music
are unchanged. The NTSC version runs at 30 fps and its races at 60 fps.

**HUD size** — 100 / 85 / 75 / 65 % (default 85 %): the low-resolution HUD
and menu art is drawn smaller so it looks sharper next to the high-resolution
game.

**Track preview camera** — while the camera flies over the track before a
race, Select holds it in place; then L1 / R1 move it back and forth along its
path, L2 / R2 four times as fast. Select again lets it fly on.

**Other options**: CPU overclock (races only; keeps two-player races at full
rate), near-polygon pool size, frame smoothing, race progress bar position,
VSync on high-refresh monitors.

**Fixes**
- Character voices no longer stall the menus while they load.
- Music changes no longer hitch.
- Pier: a corrupted sea-grid record on the European disc crashed the
  original GPU; broken packets are now dropped instead of halting the game.
- Pick-up crates no longer float in mid-air in wide views (a 32-bit overflow
  in the game's object position maths).
- Scenery no longer vanishes during jumps and falls (Skate Park) or at long
  range (fences and roofs in the Neighbourhood), and the sky of the outdoor
  tracks reaches the edges and the top of a 32:9 view.
- The star-shaped scene transition covers the whole 32:9 view.
- Lamp halos are round (no faint square around them) and no longer shine
  through walls.
- Two-player end-of-race camera cuts no longer flash shrunken prompts and
  stale margins for a frame.
- Track seen through a doorway at the sides of a wide view is drawn at once
  (Toy Store finish), and rooms whose doorway is above or below the view no
  longer vanish (Car Park pillar, barrier and wall with the camera high).
- Rooms seen through a doorway keep the scenery above and below it (the far
  wall of the house's upstairs corridor).
- No more one-frame gaps in the track ahead (Car Park landings) and no more
  one-frame 4:3 picture when pressing X or Triangle in the character select.
- Snow falls across the whole wide view.
- Faster widescreen rendering: the side margins are drawn in one pass per
  frame and the scenery's transparent faces in batches (Toy Store worst case:
  GPU load about 77% -> 32% on an RTX 4070 at 32:9, 6x).
- Repairs the track data of dumps with read errors (see below).

Plus everything psxrecomp provides: high internal resolution, texture
filtering, save states, controller rumble.

## Disc dumps

The American (SLUS-01214) and the European Fr, De (SLES-03397) discs are
accepted as clean Redump dumps.

Two dumps of the European disc (SLES-03398) are accepted. The first is clean:
every sector's error-detection code checks out. The second, the dump this
project was first built from, has 78 misread sectors: about 45 values in ten
track files (`3E 3E` where the disc has `80 3F` / `10 3F`), which break
textures and geometry on Skate Park, Pier, Car Lot, Cinema and Neighbourhood,
plus a few music, voice and video sectors.

The **Track data fix** feature (Fixes group, on by default) restores those
track values while the disc is read. It is a list of guarded patches in the
mod manifest: no game data is shipped, and on a clean dump it changes
nothing. If your dump is rejected, make a fresh one with
[redumper](https://github.com/superg/redumper).

The runtime can also read any file placed under `disc_override/` next to the
game executable instead of the one on the disc (same path, e.g.
`disc_override/COURSE_B/PIER.AXE`), for testing replacement data.

## How to play

1. Download the latest zip from
   [Releases](https://github.com/IraFunesto/ToyStoryRacerRecomp/releases/latest)
   and extract it into a new folder.
2. Run `ToyStoryRacer_Recompiled.exe`. Nothing else needs installing: on
   first use the launcher downloads its own portable toolchain (CMake, Clang
   and Python).
3. Select your own disc (.cue/.bin of SLES-03398, SLUS-01214 or SLES-03397)
   and follow the **Generate & rebuild** wizard: that disc's version is
   compiled on your PC.
4. Press **Play**: widescreen and the native renderer are already on; the
   launcher's Mods page has every option.
5. Other discs: in Mods set *Set up another disc* (or a *Version* not set up
   yet) and press Play — the disc setup opens; select the disc and Generate
   & rebuild again.

The download contains no game data and no Sony BIOS; OpenBIOS (MIT-licensed)
is used when you do not provide your own.

## Framework changes

The game needs a few additions to psxrecomp (a linked-list hook for the menu
prompts, disc file overrides, CD/XA loading, the disc as files, function and
kernel-call hooks for the instant loading and memory card, a disc-folder hook
and a disc-serial hook for the three-disc setup, GPU packet safety,
CPU overclock and more). They ship inside the release zip and are kept here as
`patches/psxrecomp-toystoryracer.patch` against the pinned `psxrecomp`
submodule commit.

## Legal

You must own the original game. Disc images and files taken from discs are
gitignored and must never be committed. Retail BIOS dumps are not
redistributed; OpenBIOS is used for Generate unless you supply your own SCPH
locally.

App icon: the Toy Story Racer logo (`assets/psxrecomp.ico` / `.png`); Windows
builds embed it via `APP_ICON`. Launcher box art: `launcher_assets/img/`
(see `BOXART_SOURCE.txt`).

## Quick start (dev)

```bash
git submodule update --init --recursive
git -C psxrecomp apply --ignore-whitespace ../patches/psxrecomp-toystoryracer.patch
./psxrecomp/tools/ci/build_emitters.sh
python3 psxrecomp/psxrecomp_cli.py generate \
  --config game.toml --project-root . --disc disc/<your>.cue
cmake -S . -B build-release -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build-release --target psx-runtime
```

Release kit (setup host, no Sony BIOS). The other discs' projects
(`versions/USA`, `versions/FrDe`) are made by `tools/make_disc_project.py --kit`
from each disc's probe in `disc/probes/<serial>/` (local only, never shipped):

```bash
cmake -S . -B build-setup -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DPSXRECOMP_FORCE_SETUP_HOST=ON -DPSXRECOMP_BIOS_STEMS=OpenBIOS
cmake --build build-setup --target psx-runtime
RELEASE_VERSION=x.y.z bash scripts/package_setup_release.sh build-setup windows-x64
```

Zip prefix for CI artifacts: `tsr`.

## Symbols

Progressive map: `symbols.toml` → `python3 tools/sync_symbols.py` →
`psx_symbols.h` (`PSX_FN_*`). See `psxrecomp/docs/SYMBOLS.md`.

## Framework pins

Submodule gitlinks (`psxrecomp`, optional `recomp-ui`, nested `recomp-net`)
are authoritative. `framework_pins.txt` is an optional scaffold snapshot.
Bump submodules deliberately and refresh the patch when you do.
