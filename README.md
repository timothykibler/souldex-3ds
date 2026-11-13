# SoulSilver 3DS

A native Nintendo 3DS build of Pokémon SoulSilver from the [pret/pokeheartgold](https://github.com/pret/pokeheartgold)
decompilation: the game's code (its C, and the ~64% still in ARM/Thumb assembly, which the 3DS's
ARM11 runs as it is) compiled with devkitARM GCC, on a runtime that stands in for the DS hardware
and the parts of Nintendo's SDK that drove it. The game's files are read from your own ROM.

It boots, loads saves, and plays the overworld and battles with the DS picture shown 1:1 (the
overworld's 3D widened to fill the 400x240 top screen). No sound yet. See HANDOFF.md.

Nothing of the game is in this repository. Builds contain code compiled from the decomp: keep them
to yourself.

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

    docker run --rm -v "$PWD":/w -w /w ss3ds-dev python3 port/build.py
    port/package.sh        # build-3ds/soulsilver.cia (builds the ss3ds-cia image from docker/cia the first time: makerom, bannertool)

## Run

Put your SoulSilver (US, IPGE) ROM at `sdmc:/3ds/soulsilver-3ds/soulsilver.nds` and install the CIA.
The bottom screen and `sdmc:/3ds/soulsilver-3ds/log.txt` show what it's doing. Saves go to
`sdmc:/3ds/soulsilver-3ds/soulsilver.sav`, in the raw format TWiLight Menu++ uses.

## Display

The DS picture is shown with its exact pixels (1:1) in the middle of each screen; in the overworld
the 3D world fills the whole top screen. In the **ZL+ZR** menu (also **L+R+SELECT**), *Screen*
switches to *fill*: the picture fills each screen's height at 1.25x, which no exact scaling can
(pixels stay even, edges slightly soft), and *Sides* chooses black sides or the picture's edges
continued. The choice is kept in `settings.txt`.

**Stereoscopic 3D** follows the 3D slider in 1:1 mode: the 2D layers sit at the screen, the 3D
world goes into it (battles stay flat: their Pokémon stand on 2D platforms). It costs some CPU
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
