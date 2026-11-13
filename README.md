# SoulSilver 3DS (phase 0)

A native Nintendo 3DS build of Pokémon SoulSilver from the [pret/pokeheartgold](https://github.com/pret/pokeheartgold)
decompilation: the game's code (its C, and the ~64% still in ARM/Thumb assembly, which the 3DS's
ARM11 runs as it is) compiled with devkitARM GCC, on a runtime that stands in for the DS hardware
and the parts of Nintendo's SDK that drove it. The game's files are read from your own ROM.

**Phase 0** is the go/no-go: the game builds, links and boots as a 3DS program. It draws nothing yet
(the renderer is phase 1) and makes no sound.

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
    port/package.sh        # build-3ds/soulsilver.cia (needs the fulldex-3ds-builder image: makerom, bannertool)

## Run

Put your SoulSilver (US, IPGE) ROM at `sdmc:/3ds/soulsilver-3ds/soulsilver.nds` and install the CIA.
The bottom screen and `sdmc:/3ds/soulsilver-3ds/log.txt` show what it's doing. Saves go to
`sdmc:/3ds/soulsilver-3ds/soulsilver.sav`, in the raw format TWiLight Menu++ uses.

## How it fits together

| Piece | Where |
|---|---|
| Metrowerks → GCC: compiler flags, MW assembly dialect → GNU as | `port/compile_game.py`, `tools/fakemw/mwasm2gas.py` |
| DS memory map moved into the 3DS heap region (shadow I/O, VRAM, palettes, OAM) | `port/include/nitro/hw/ARM9/mmap_global.h`, `port/runtime/ds_mem.c` |
| SDK files with MW inline assembly: cut out, rewritten in C | `port/sdkfix.py`, `port/runtime/sdk_asm.c` |
| OS (frame sync, ticks, alarms, locks), card/ROM/save, overlays, MI, divider, clock, input | `port/runtime/` |
| Overlays all resident; "loading" one resets its data and runs its initialisers | `port/gen_ld.py` |
| Wi-Fi, wireless, GBA slot: stubs (generated at link time) | `build-3ds/gen/stubs_auto.c` |
