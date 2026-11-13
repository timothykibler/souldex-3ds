# SoulSilver 3DS port: handoff (shelved 2026-11-13)

Native Nintendo 3DS build of Pokémon SoulSilver from pret/pokeheartgold. The game's C (~36% of
functions) and its remaining ARM/Thumb assembly (~64%) are compiled for the 3DS's ARM11 with
devkitARM GCC and run natively; `port/runtime/` replaces the DS hardware and the hardware-bound
parts of NitroSDK. Game files are read at runtime from the player's own ROM. Read `README.md` for
the build; this file is the state of play and how to pick it up.

## Where it stands (2026-11-13, second session)

| Phase | State |
|---|---|
| 0: boots | Done. Existing saves load (the main menu's Continue); wild battles work. |
| 1: 2D | Done. Pixel-identical to the software reference renderer (0 differing pixels on 2D frames). Shown 1:1. |
| 2: 3D | Works in the intro, overworld and battles: geometry engine + PICA renderer, edge marking, top screen widened to 400x240 when the 3D covers the DS frame (overworld). |
| 3: sound | Not started. Sound commands are acknowledged instantly (`PXI_SendWordByFifo` in `devices.c`). |
| Speed | Azahar (whose ticks are, as far as known, original-3DS speed): battle 59 fps; overworld ~30 fps, CPU-bound (game code ~16 ms incl. the geometry engine ~6 ms, frame building ~6 ms). Not yet measured on hardware. |

Fixed this session (all in git history): VTX_DIFF scale; u64/s64 aligned to 4 like Metrowerks (`port/include/nitro/types.h`, saves and asm struct offsets); HW_BUTTON_XY_BUF layout (A/B never worked); WVR start-up and an emulated WM (`wm.c`); VBlank interrupt during card reads (field texture-load tasks overflowed the VBlank queue); literal DS VRAM addresses in `BattleSystem_SetBackground` (decomp patch); GPU vertex buffers flushed from the CPU cache (needed on hardware, invisible in Azahar).

## Open items, in order

1. **Hardware test** of the CIA (speed, and the cache flushes: the GPU path has only run in Azahar).
2. **Overworld speed on an original 3DS**: run the frame building (BG planes, sprites, texture
   decode, command lists) on the second core in parallel with the game, double-buffering what it
   reads; and make the geometry engine's vertex path cheaper (strip assembly copies, texcoord maths).
3. **3D features not done**: anti-aliasing (DISP3DCNT bit 4, the field sets it), shadow polygons,
   wireframe, toon/highlight, fog, w-buffering, translucent Y-sorting, rear-plane, display capture.
   Edge marking uses one edge colour (HGSS sets all eight alike).
4. **Assembly that writes 3D command registers directly** (`sub_02015E64`, `sub_020161CC`,
   `ov41_022465D8`; overlay 41 only): see the git history of this file for the decoded plan.
5. **Cheats** (done, `runtime/cheats.c`, `port/cheatmap.py`): verified in Azahar that Max Money
   writes the save's money field. The menu (ZL+ZR / L+R+SELECT) is untested on hardware. Codes
   whose addresses fall in unnamed BSS gaps or overlay data don't translate (skipped); `cheatlog`
   in debug.txt logs every write and the value it replaced.
6. **Phase 3, sound**: run the decomp's ARM7 sound driver (`libsnd_arm7`) on a second core, mix the
   16 channels, output through NDSP.
7. Smaller: threads (`OS_CreateThread` is fatal; overlays 13/112), soft reset is fatal, `.3dsx`
   packing, H-blank DMA effects, mosaic, bitmap sprites, microphone, RTC uses UTC, VRAM bank
   remapping isn't modelled (each window is its own memory; a first attempt broke battles).

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
- **CIA**: `port/package.sh` (builds the `ss3ds-cia` image from `docker/cia/Dockerfile` the first
  time: devkitARM + makerom + bannertool, the latter compiled from source).
- **One-shot test runs**: `AZAHAR=/path/Azahar.app tools/azrun.sh <frame> [timeout] < debug-lines`
  (writes debug.txt, launches, waits for the capture, kills Azahar, converts to `captures/run/`,
  prints the log). Captures are `shot_N` (DS resolution, + software renderer at N+1) and `full_N`
  (both 3DS screens). Reaching the overworld from a save: `mash 300 600 30 START` + `press 700 A`
  (save at Azahar's `sdmc/3ds/soulsilver-3ds/soulsilver.sav`); a wild battle: then
  `mash 840 880 8 RIGHT` and alternate DOWN/UP holds in the grass.
- **debug.txt keys**: `dump N`, `press F KEY`, `mash F T P KEY`, `trace3d F`, `wide 0|1|2`,
  `edge 0|1|2` (2 shows the polygon-ID image), `cheatlog`. The log also lists failed GF_ASSERTs with their
  caller (look up in `build-3ds/soulsilver.map`) and every 600 frames a time breakdown.
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
