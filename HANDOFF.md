# SoulSilver 3DS port: handoff (shelved 2026-11-13)

Native Nintendo 3DS build of Pokémon SoulSilver from pret/pokeheartgold. The game's C (~36% of
functions) and its remaining ARM/Thumb assembly (~64%) are compiled for the 3DS's ARM11 with
devkitARM GCC and run natively; `port/runtime/` replaces the DS hardware and the hardware-bound
parts of NitroSDK. Game files are read at runtime from the player's own ROM. Read `README.md` for
the build; this file is the state of play and how to pick it up.

## Where it stands

| Phase | State |
|---|---|
| 0: boots | Done. Boots, loads overlays, runs the intro → title → attract loop; got through character creation on real hardware (user's 3DS, older software-renderer CIA). |
| 1: 2D | Done. GPU compositor (`port/runtime/gpu2d.c`, citro3d) is pixel-identical to the software reference renderer (`render2d.c`) across the intro, within ±1/32 rounding on blends/fades. 0.7–2.6 ms CPU per frame. |
| 2: 3D | In progress. Geometry engine + PICA renderer work: the intro's Ecruteak scene (frame 2100) draws correctly. Open questions below. |
| 3: sound | Not started. Sound commands are acknowledged instantly (`PXI_SendWordByFifo` in `devices.c`) so nothing waits; nothing plays. |

## Open items, in order

1. **Ecruteak frame "spikes"** (`captures/intro-ecruteak-frame2100-x4.png`): long orange triangles
   over a brown board, and a dark diagonal line from the top-left building. Vertex maths checked
   (no bad triangles); possibly real banners. Was waiting for the user to compare with the real
   intro. The diagonal could be shadow polygons (mode 3, not drawn yet) or DS line-polygons
   (zero-area triangles the DS draws as lines; the PICA draws nothing).
2. **Assembly that writes 3D command registers directly** (writes are lost): `sub_02015E64` and
   `sub_020161CC` (asm/unk_02015DD8.s, 3D sprites, called from overlay 41 only) and
   `ov41_022465D8`. Plan: rewrite in C calling `port_g3_write`, and have `tools/fakemw/mwasm2gas.py`
   rename the asm labels so the C versions win. `sub_020161CC` was fully read and decoded (push;
   DIF_AMB = 0x7FFF8000|u16@0x3c; SPE_EMI 0x4210; TEXIMAGE built from the tex-info at +0x24 and +0x28;
   PLTT_BASE; POLYGON_ATTR (u8@0x3e)<<16|0xC0; TRANS; G3_RotZ from FX_SinCosTable_[(+0x18>>4)];
   SCALE (+0xc, +0x10, 1.0); TRANS back; NNS_G2dDrawSpriteFast(0,0,0,s16@8,s16@0xa,U0,V0,U1,V1);
   pop). The other 31 functions only write SWAP_BUFFERS (0x540) / VIEWPORT (0x580), which
   `port_g3_vblank` already picks up from shadow memory.
3. **3D features not done**: shadow polygons, wireframe (alpha 0), toon/highlight tables, edge
   marking, fog, anti-aliasing, w-buffering, translucent Y-sorting, rear-plane, 1-dot/far-plane
   rules, display capture. Title screen's 3D model not yet checked since the culling fix.
4. **Frame dips**: background texture rebuilds in `gpu2d.c` (`text_plane`) cost 27–48 ms when a
   whole 512×512 plane changes (scene changes, palette fades: 213 rebuilds/10 s at the title). A
   per-tile incremental rebuild is in place but didn't help in Azahar (the user said shelve: may be
   smoother on hardware). Ideas: per-tile caches keyed by (tile, palette row); spread rebuilds over
   frames; decode on the second core.
5. **Phase 3, sound**: the NitroSDK decomp includes the ARM7 sound driver (`libsnd_arm7`:
   SND_SeqMain, SND_ExChannelMain …). Plan: run it on the 3DS (a thread on core 1/2), mix the 16
   DS channels, output through NDSP. NitroSystem's `player.c` / `sndarc_stream.c` are excluded
   today (MW-isms) and stubbed (`build-3ds/gen/stubs_auto.c`).
6. Smaller: threads (`OS_CreateThread` is fatal; only overlays 13/112 use it), soft reset is fatal,
   `.3dsx` packing fails on an unaligned relocation (CIA from the ELF is fine), H-blank DMA effects,
   mosaic, bitmap sprites, microphone, RTC uses UTC.

## Reference work found (2026-11-13)

- **pspoke** (github.com/IbrahimIrfan/pspoke, GPL-3.0): SoulSilver + Platinum natively on PSP at
  ~30 fps with sound. Its `NOTES.md` is a goldmine of hardware lessons (ROM file-handle limits,
  asserts that retail strips, 3D display-list caching that cut outdoor 3D from 19 to 5 ms, translucent
  draw ordering, texture dirty tracking). Its GPU 2D compositor `port/native-stack-render/render-gu2d.cpp`
  and 3D `g3_handler.cpp` are the closest prior art. Copying its code would make this project GPL-3.0.
- **libntr** (github.com/cybervisi0n/libntr, MIT): NitroSDK replacement for PC (OpenGL 4.2 3D).
- **ctrl-cheeb-del/pokeheartgold**: a 100% C fork of the decomp (useful to look up C for an asm
  function, e.g. item 2).

## Workflow that works

- **Build** (in Docker, image `ss3ds-dev`, Docker Desktop must be running):
  `docker run --rm -v "$PWD":/w -w /w ss3ds-dev python3 port/build.py [--skip-game]`
  (`--skip-game` relinks the runtime only; a full build recompiles the game and SDK, ~5 min).
  Then `find -L build-3ds -name "* [0-9].*" -delete` (iCloud conflict copies, see below).
- **CIA**: `port/package.sh` (needs the `fulldex-3ds-builder` image from the pokemon-fulldex repo's
  `gba/3ds/Dockerfile`: makerom + bannertool).
- **Test in Azahar** (downloaded to the old session's scratch folder; re-download
  `azahar-macos-arm64-2126.2.zip` from github.com/azahar-emu/azahar/releases if gone):
  - Launch through the bundle (`open -n -a Azahar.app --args -w game.elf`): run directly, Azahar
    stops at a "run via the .app" warning dialog.
  - Force `check_for_update_on_start=false`, `firstStart=false`, `confirmClose=false` in
    `~/Library/Application Support/Azahar/config/qt-config.ini` before each launch (an update
    dialog blocks start; Azahar rewrites its config on clean exit).
  - Copy the ELF out of iCloud folders before launching.
  - The ROM goes in Azahar's `sdmc/3ds/soulsilver-3ds/soulsilver.nds`; the log is `log.txt` there.
  - `debug.txt` next to it (one per line): `dump N` saves both screens every N frames as
    `shot_*.bmp`, plus the software renderer's version of the same frame (`shot_N+1`) and logs how
    many pixels differ; `press <frame> <A|B|START|…>` scripted input; `test3d` draws a red square
    in the 3D layer's top-left (orientation check); `trace3d <frame>` logs 3D command counts,
    the projection and off-screen triangles for that frame, then the next 400 commands.
  - **Debugger**: set `use_gdbstub=true` in the config (emulation then waits for a debugger),
    launch via `open`, then from Docker: `arm-none-eabi-gdb -ex 'target remote host.docker.internal:24689'`
    (lldb on the Mac can't drive Azahar's stub). Set it back to false afterwards.
- **iCloud**: `~/Documents` syncs to iCloud and makes "… 2" copies of build files. The big folders
  are `*.nosync` (iCloud skips those) with symlinks under the plain names.

## Conventions learned the hard way

- DS address A lives at A + 0x08000000 (`PORT_DS_DELTA`); asm literals and `mov #1; lsl #26` are
  rewritten by the converter; C uses `port/include/nitro/hw/ARM9/mmap_global.h`.
- PICA texture memory: 8×8 Morton tiles, tile rows bottom-up in memory, but **sampled with v = 0 at
  the image's top row** (`gpu2d.c` `tex_tile`). Screen targets: draw with `Mtx_OrthoTilt`.
- Render-to-texture (the 3D layer): the viewport's y counts from the top; vertices go in with y
  negated (found with the `test3d` square).
- Stencil: a pass must not change bits a later pass of the same layer tests (three-pass blending in
  `draw_layer`).
- DS front faces are clockwise on screen with y down: positive area in y-up clip space.
- Metrowerks soft-float compare helpers return results in CPU flags: `vcmp` + `vmrs` reproduces them.
- Render targets must be in VRAM (`C3D_TexInitVRAM`); read back with `C3D_SyncTextureCopy`.
