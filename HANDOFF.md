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
| 1: 2D | Done. Pixel-identical to the software reference renderer (`scale1` mode). Shown 1:1 by default; a menu setting fills the screens' height at 1.25x (composed at 2x and box-filtered down), with optional side fill. |
| 2: 3D | Works in the intro, overworld and battles: geometry engine + PICA renderer, edge marking, top screen widened to 400x240 when the 3D covers the DS frame (overworld). |
| 3: sound | Working: the SDK's ARM7 driver (`libsnd_arm7`, symbols `a7_`-prefixed) runs on core 2, ticked every 5.2 ms of output; a 16-channel mixer (PCM8/16, ADPCM, PSG, noise) feeds NDSP at 32768 Hz (`port/runtime/snd.c`). Music, effects and cries verified in the intro, field and a battle. Sound alarms (`NNS_SndStrm` refills; only the Wi-Fi voice chat, overlay 0, streams) run on the output's clock and reach the game each frame; `debug.txt` `strmtest F` checks them with a 440 Hz stream. Not done: capture/reverb (never started by the game). `debug.txt` `wavdump [F]` records 30 s to `sound.wav`. |
| Speed | Azahar (whose ticks are, as far as known, original-3DS speed): battle 59 fps; overworld ~30 fps, CPU-bound (game code ~16 ms incl. the geometry engine ~6 ms, frame building ~6 ms). Not yet measured on hardware. |

Fixed this session (all in git history): VTX_DIFF scale; u64/s64 aligned to 4 like Metrowerks (`port/include/nitro/types.h`, saves and asm struct offsets); HW_BUTTON_XY_BUF layout (A/B never worked); WVR start-up and an emulated WM (`wm.c`); VBlank interrupt during card reads (field texture-load tasks overflowed the VBlank queue); literal DS VRAM addresses in `BattleSystem_SetBackground` (decomp patch); GPU vertex buffers flushed from the CPU cache (needed on hardware, invisible in Azahar).

## Open items, in order

1. **Hardware test** of the CIA (speed, and the cache flushes: the GPU path has only run in Azahar).
2. **Speed**: since 2026-11-14 the frame is built on a render core (ctr.c "the render core": New
   3DS core 2, inline elsewhere) while the game runs its next frame, and the game thread paces
   itself to the VBlank (gspWaitForEvent) instead of citro3d's SYNCDRAW: field 39 -> 59 fps in
   Azahar, pixel-identical (`debug.txt` `render inline` to compare; the log's "waiting for the
   render core" is how much frame building is still the limit). Not yet measured on hardware.
   Earlier notes (before the render core), kept for the next step:
   Plan when picked up: a **30 / 60 fps mode** setting. 30 fps (the default) keeps everything
   (seam fill, edge marking, stereo); 60 fps turns off what costs per frame (seam fill, stereo,
   possibly edge marking) and accepts its artifacts.
   Field, Azahar timing, per frame: game code ~9.5 ms, of which the geometry engine (gx3d.c,
   the DS 3D emulated on the CPU) ~8.4 ms; frame building ~8.4 ms (3D layer 5.7 ms with seam
   fill, 4.3 without; BG planes 1.1 ms). The geometry engine by ablation (`debug.txt`
   `g3prof N`: 1 no polygon assembly, 2 no vertex work, 3 decode only): polygon assembly 1.7 ms,
   per-vertex work 1.3 ms, other commands (matrices, lighting, texcoords) 2.4 ms, and 3.0 ms
   with commands decoded but not run. Decoding display lists in place (port_gx_fifo_send, no
   per-word call or copy) changed nothing (8.38 ms either way), so those 3 ms aren't the
   per-word plumbing: profile with finer tools before more guesses (the per-command counts
   printed every 600 frames read 0: sHist is reset somewhere, unfixed).
   Ideas: cache display lists whose content and state repeat (pspoke: outdoor 3D 19 -> 5 ms);
   transform on the PICA instead of the CPU (as native games do: model-space vertices,
   matrices as uniforms); frame building on the second core. A New 3DS runs the ARM11 at
   804 MHz with L2 cache, roughly 3x Azahar's timing, so hardware numbers may differ a lot:
   measure there first (the log prints this breakdown every 600 frames).
3. **3D features not done**: anti-aliasing (DISP3DCNT bit 4, the field sets it), shadow polygons,
   wireframe, toon/highlight, fog, w-buffering, translucent Y-sorting, rear-plane, display capture.
   Edge marking uses one edge colour (HGSS sets all eight alike).
4. **Assembly that writes 3D command registers directly** (`sub_02015E64`, `sub_020161CC`,
   `ov41_022465D8`; overlay 41 only): see the git history of this file for the decoded plan.
5. **Cheats** (done, `runtime/cheats.c`, `port/cheatmap.py`): verified in Azahar that Max Money
   writes the save's money field. The menu (ZL+ZR / L+R+SELECT) is untested on hardware. Codes
   whose addresses fall in unnamed BSS gaps or overlay data don't translate (skipped); `cheatlog`
   in debug.txt logs every write and the value it replaced.
6. **Phase 3, sound**: done (see the table). Lesson: the game asks for volume 128, one past `SNDi_DecibelTable`; the DS reads 0 there, so
   `port/sdkfix.py` gives the table a trailing 0 (without it every channel ran at +229 dB).
7. **Stereoscopic 3D**: done (`gpu2d.c`, "stereoscopic 3D"). With the slider up, in 1:1 mode, the
   top screen is composed per eye: 2D at the screen plane, the 3D layer shifted by its z/w,
   clamped to the frame's on-screen range (nearest at the screen, farthest 3.5 px per eye behind
   at full slider); orthographic scenes (battles: 2D platforms) stay flat. The right eye needs
   ~2.3 MB of VRAM, so not in the 1.25x fill mode. Cost: the 3D layer's command building twice
   (+2.5 ms a frame in the field, Azahar's timing); only with the slider up, and the menu's *3D* row
   (`settings.txt` `3d off`, or `-DPORT_NO_STEREO`) turns it off entirely. `debug.txt`
   `stereo X` sets the slider; dumps add `full_N_r` (the right eye).
8. **Wipes in widened scenes**: 2D effects reaching the DS picture's edge (the encounter
   transition's stripes) aren't carried over the widened world's sides, as fulldex does.
9. Smaller: threads (`OS_CreateThread` is fatal; overlays 13/112), soft reset is fatal, `.3dsx`
   packing, H-blank DMA effects, mosaic, bitmap sprites, microphone, RTC uses UTC, VRAM bank
   remapping isn't modelled (each window is its own memory; a first attempt broke battles).

## Hardware-only bugs, and catching them in Azahar

The 3DS faults on bad addresses that Azahar quietly tolerates, but Azahar logs each one
("unmapped Read32 @ ... at PC ..."), and `tools/azrun.sh` now prints their count after every run:
anything non-zero is a hardware crash waiting (resolve the PC against build-3ds/soulsilver.elf).
Classes found and fixed so far:
- DS addresses the port didn't move: data words (mwasm2gas), Thumb `mov`+`lsl` constants (three
  rules in mwasm2gas: same register, derived register, I/O base), and the SDK's sound command
  packing sample addresses into 27 bits (snd_channel_port.c restores them).
- Calls into memory several overlays share (7, 8 and 10 at 0x0221BE20): the decomp names one
  occupant, the DS runs whichever is loaded. port/ovdispatch.py generates trampolines for the 360
  such targets, dispatched by runtime/overlay.c to the loaded overlay (logged: "ovdispatch:").
- Known leftover: code deriving values from a moved I/O address (overlay 7: `lsr r1, r0, #13` of
  WINOUT's address) gets different bits than on the DS.

## Testing on a 3DS without the SD card

- Install over Wi-Fi: FBI -> Remote Install -> Receive URLs over the network, then
  `python3 tools/send_cia.py <3DS IP>`.
- Log over Wi-Fi: `sdmc:/3ds/soulsilver-3ds/netlog.txt` holding this computer's address, then
  `python3 tools/recv_log.py` (prints and saves to captures/netlog.txt). Each start-up also sends
  the previous run's last lines (log.prev.txt) and any new Luma crash dump's registers; resolve
  them against `build-3ds/elves/<build ID>.elf`.

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
  `docker run --rm -v "$PWD":/w -w /w ss3ds-dev python3 port/build.py [--skip-game] [--release]`
  (`--skip-game` relinks the runtime only; a full build recompiles the game and SDK, ~5 min).
  Debug is the default (`PORT_DEBUG=1`, `port/runtime/port.h`); `--release` turns off `debug.txt`,
  the Wi-Fi log and the default-on frame counter. Only the runtime differs, so switching variants
  with `--skip-game` is safe. `tools/azrun.sh` needs a debug build (it drives the game through debug.txt).
  Then `find -L build-3ds -name "* [0-9].*" -delete` (iCloud conflict copies, see below).
- **CIA**: `port/package.sh` (builds the `ss3ds-cia` image from `docker/cia/Dockerfile` the first
  time: devkitARM + makerom + bannertool, the latter compiled from source).
- **Map tour**: `AZAHAR=... tools/maptour.py [--start MAP] [--count N]` warps through every map
  (479, about 40 min; 40 per Azahar run) and writes each map's unmapped accesses and crashes to
  `captures/maptour.txt`. debug.txt takes any number of `warp` lines; `warpmark` tags each in
  Azahar's log. Azahar doesn't log every unmapped access (a null-page read in port code went
  unlogged), so a clean map is a good sign, not proof.
- **One-shot test runs**: `AZAHAR=/path/Azahar.app tools/azrun.sh <frame> [timeout] < debug-lines`
  (writes debug.txt, launches, waits for the capture, kills Azahar, converts to `captures/run/`,
  prints the log). Captures are `shot_N` (DS resolution, + software renderer at N+1) and `full_N`
  (both 3DS screens). Reaching the overworld from a save: `mash 300 600 30 START` + `press 700 A`
  (save at Azahar's `sdmc/3ds/soulsilver-3ds/soulsilver.sav`); a wild battle: then
  `mash 840 880 8 RIGHT` and alternate DOWN/UP holds in the grass.
- **debug.txt keys**: `scale1` (DS picture 1:1, for the pixel comparison; at 2x composition one
  animated intro frame (1200) can compare against the frame next to it: a capture-timing quirk),
  `dump N`, `press F KEY`, `mash F T P KEY`, `trace3d F`, `wide 0|1|2`,
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
