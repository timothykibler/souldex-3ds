# SoulSilver 3DS

A native Nintendo 3DS build of Pokémon SoulSilver from the [pret/pokeheartgold](https://github.com/pret/pokeheartgold)
decompilation: the game's code (its C, and the ~64% still in ARM/Thumb assembly, which the 3DS's
ARM11 runs as it is) compiled with devkitARM GCC, on a runtime that stands in for the DS hardware
and the parts of Nintendo's SDK that drove it. The game's files are read from your own ROM. It is
not an emulator: the game runs as a 3DS program.

**New here? See [GETTING_STARTED.md](GETTING_STARTED.md)** to build, install and play.
**[PROJECT_STATUS.md](PROJECT_STATUS.md)** covers what's done, what runs natively and what is still
emulated, and the next tasks.

## What it does

- **Plays the game**: boots, loads your existing save (the TWiLight Menu++ `.sav` format), the
  overworld, battles, menus.
- **Graphics on the 3DS's GPU**: the DS's two 2D engines are composed by the GPU, pixel-identical
  to a software reference renderer; the DS's 3D (geometry engine, textures, edge marking) is drawn
  by the 3DS's PICA200.
- **Widescreen**: the DS picture shown with exact pixels (1:1); in the overworld the 3D world is
  widened to fill the 400x240 top screen. A menu setting fills the screens' height at 1.25x instead.
- **Stereoscopic 3D**: with the 3D slider up, the overworld's 3D sits behind the screen and the
  2D in front of it; switchable off in the menu.
- **Sound**: the DS's own ARM7 sound driver runs on a second core, mixed into the 3DS's DSP.
- **Cheats**: Action Replay codes from a `usrcheat.dat` (the TWiLight Menu++ database), in an
  in-game menu.
- **A CIA** with HOME Menu art made from your ROM (and, optionally, your own artwork: a layered,
  transparent, stereoscopic banner).

What's left is in [HANDOFF.md](HANDOFF.md) (speed on an original 3DS, a few 3D features, wipes in
widened scenes, smaller gaps).

## What's in this repository, and what isn't

Nothing of the game: no ROM, no game data, graphics, music or text; screenshots and builds are
git-ignored. You need your own SoulSilver ROM and save. The repository holds the port's own code,
build scripts and a patch to the decomp. A few headers in `port/include` are modified copies of the
decomp project's reconstructed SDK headers. No code was taken from other ports (e.g. pspoke, GPL).
The cheat database, artwork and the 3DS's DSP firmware are yours to supply. Builds contain code
compiled from the decomp and data read from your ROM at run time: keep them to yourself, don't
distribute CIAs.

## Build

Needs Docker. One-time setup:

    git clone https://github.com/pret/pokeheartgold.git
    (cd pokeheartgold && git apply ../port/patches/pokeheartgold.patch)
    docker build -t ss3ds-dev docker
    # the decomp's generated headers (msg ids, archive indexes), without its Metrowerks compiler:
    docker run --rm -v "$PWD":/w -w /w/pokeheartgold -e PATH=/opt/devkitpro/devkitARM/bin:/usr/bin:/bin ss3ds-dev sh -c \
      'python3 tools/scripts/lsfgen.py main.lsf.in --prefix-dir main.nef.p/ --dest-lsf main.lsf --dest-c src/meson.build --dest-asm asm/meson.build &&
       meson setup /w/build-gen --wrap-mode=nopromote --cross-file /w/tools/fakemw/cross.ini --native-file meson/native.ini -Dgame_version=SOULSILVER &&
       ninja -C /w/build-gen -k 0'

Then:

    docker run --rm -v "$PWD":/w -w /w ss3ds-dev python3 port/build.py [--release]
    port/package.sh        # build-3ds/soulsilver.cia (builds the ss3ds-cia image from docker/cia the first time: makerom, bannertool)

A debug build is the default. `--release` makes the build to play or share: it ignores `debug.txt`,
sends no Wi-Fi log and starts with the frame counter off (it stays in the settings menu). Both keep
the log file, the freeze watchdog and crash reporting. The build ID (first log line) ends in
`-debug` or `-release`.

## Run

Put your SoulSilver (US, IPGE) ROM at `sdmc:/3ds/soulsilver-3ds/soulsilver.nds` and install the CIA.
The bottom screen and `sdmc:/3ds/soulsilver-3ds/log.txt` show what it's doing. Saves go to
`sdmc:/3ds/soulsilver-3ds/soulsilver.sav`, in the raw format TWiLight Menu++ uses.

## Display

The top screen shows the DS picture with its exact pixels (1:1); in the overworld the 3D world fills
the whole wide screen. In the **ZL+ZR** menu (also **L+R+SELECT**), *Top screen* switches to
*fill*: the picture fills the screen's height at 1.25x, which no exact scaling can (pixels stay
even, edges slightly soft), and *Sides* chooses black sides or the picture's edges continued. The
bottom screen always fills (1.25x). The choices are kept in `settings.txt`.

**Stereoscopic 3D** follows the 3D slider in 1:1 mode: the 2D layers sit at the screen, the 3D
world goes into it (battles stay flat: their Pokémon stand on 2D platforms). While it's in use the
bottom screen is drawn at 1x (still filling) to leave VRAM for the second eye. It costs some CPU
time per frame; the menu's *3D* row turns it off entirely (also `3d off` in `settings.txt`, or
build with `-DPORT_NO_STEREO` to start with it off).

## Cheats

Put a `usrcheat.dat` (the R4 / TWiLight Menu++ format, e.g. DeadSkullzJr's database) at
`sdmc:/3ds/soulsilver-3ds/usrcheat.dat` (or leave it in TWiLight's `sd:/_nds/TWiLightMenu/extras/`).
In game, **ZL+ZR** (or **L+R+SELECT**) opens the menu on the bottom screen: A opens a folder
or turns a code on/off, B closes. Choices are kept in `cheats.txt`. Codes that change game data
work (money, items, steps, Pokemon...); codes that patch the DS's machine code can't apply to this
build and do nothing.

## How it fits together

| Piece | Where |
|---|---|
| Metrowerks → GCC: compiler flags, MW assembly dialect → GNU as | `port/compile_game.py`, `tools/fakemw/mwasm2gas.py` |
| DS memory map moved into the 3DS heap region (shadow I/O, VRAM, palettes, OAM) | `port/include/nitro/hw/ARM9/mmap_global.h`, `port/runtime/ds_mem.c` |
| SDK files with MW inline assembly: cut out, rewritten in C | `port/sdkfix.py`, `port/runtime/sdk_asm.c` |
| OS (frame sync, ticks, alarms, locks), card/ROM/save, overlays, MI, divider, clock, input | `port/runtime/` |
| Overlays all resident; "loading" one resets its data and runs its initialisers | `port/gen_ld.py` |
| Wi-Fi, wireless, GBA slot: stubs (generated at link time) | `build-3ds/gen/stubs_auto.c` |

## License

The port's own code is under the MIT License; the decompilation it builds on, and the game itself,
are not covered. Unofficial fan project, not affiliated with Nintendo, Game Freak, Creatures Inc. or
The Pokémon Company. See [LICENSE](LICENSE).
