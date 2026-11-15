# Project status: what's built, what's native, what's next

*As of 2026-11-14 (commit `055128a`, plus uncommitted geometry-worker work).*

This is the overview for someone new to the project. [README.md](README.md) covers building and
features, [GETTING_STARTED.md](GETTING_STARTED.md) covers installing and playing, and
[HANDOFF.md](HANDOFF.md) holds the detailed engineering notes: profiling numbers, debugging
workflow, and the conventions we learned the hard way. Parts of HANDOFF were written on
2026-11-13 and are out of date. Where the two disagree, this file is newer.

---

## 1. The idea in one paragraph

Pokémon SoulSilver was written for the Nintendo DS. [pret/pokeheartgold](https://github.com/pret/pokeheartgold)
reconstructed its source: about 36% of functions are C, and the remaining ~64% are still ARM/Thumb
assembly. The DS's main CPU (ARM9) and the 3DS's CPU (ARM11) run the same instruction set, so we
**compile all of that code with GCC (devkitARM) and run it directly on the 3DS**. Nothing is
interpreted. Under the game sits a **runtime** (`port/runtime/`) that does what the DS hardware and
Nintendo's DS SDK (NitroSDK) did. The game's assets are read at run time from the player's own
ROM, and the repository contains no game data.

So the game *logic* is native. The *hardware* it expects is partly provided by native 3DS code
and partly modelled. Most of the remaining work is replacing those models with native code.

---

## 2. What we've done so far

The project took 45 commits over 2026-11-13 and 2026-11-14. In order:

| Area | What exists now |
|---|---|
| **Toolchain** | The decomp's Metrowerks build was converted to GCC. Compiler flags are set in `port/compile_game.py`. MW assembly is translated to GNU as by `tools/fakemw/mwasm2gas.py`, which also rewrites DS hardware addresses. SDK files containing MW inline asm have those parts cut out (`port/sdkfix.py`) and rewritten in C (`runtime/sdk_asm.c`). MW ABI quirks are reproduced: u64 aligned to 4, and soft-float compares that return their result in CPU flags (`runtime/mwrt.S`). |
| **Boot and save** | Boots and loads existing TWiLight Menu++ `.sav` files. The main menu, Continue and saving all work. ROM reads go through a 2 MB block cache. |
| **Overlays** | Every overlay stays resident in memory. "Loading" one resets its data. Calls into memory that several DS overlays share go through 360 generated trampolines (`port/ovdispatch.py`), which dispatch to whichever overlay is "loaded". |
| **2D graphics** | Both DS 2D engines are composited on the 3DS GPU (PICA200 via citro3d). The output is pixel-identical to a software reference renderer (`render2d.c`). Display options: 1:1 (default) or a 1.25x fill, with optional side fill. The bottom screen is composed at DS size and scaled once. |
| **3D graphics** | A CPU geometry engine (`gx3d.c`) feeds a PICA renderer. Supported: edge marking, W-buffering, seam and hole fill, widescreen 3D (400x240 in the overworld), and stereoscopic 3D. |
| **Sound** | The DS's own ARM7 sound driver runs on a second core. A 16-channel software mixer sends its output to NDSP. Music, sound effects, cries and stream alarms all work. |
| **Input** | Buttons, Circle Pad (works as the D-pad) and touch screen. |
| **Speed** | Each frame is built on a render core while the game runs the next one. Frames are paced to the real 59.83 Hz VBlank, and a "Speed: double" option (60 fps) is available. On New 3DS models the 804 MHz mode is enabled. |
| **Extras** | Action Replay cheats from `usrcheat.dat` with an in-game menu (ZL+ZR). The CIA gets HOME Menu art made from the player's ROM, with an optional layered stereoscopic banner. |
| **Hardware bring-up** | The game runs on a real New 3DS and a 3DS XL. Fixed hardware-only crashes: unmoved DS addresses in assembly, VRAM clears, sound sample addresses, calls into shared overlay memory, the GPU drawing vertices the game was rewriting, and a freeze where the VBlank event stopped arriving. |
| **Tooling** | `tools/azrun.sh` runs one-shot Azahar tests and counts unmapped accesses, each of which is a future hardware crash. `debug.txt` scripting supports press/mash, warp, dumps and traces. Logs can be sent over Wi-Fi (`tools/recv_log.py`), the CIA installs over Wi-Fi (`tools/send_cia.py`), and a watchdog catches freezes. |

**Verified working:** intro, title screen, overworld, entering buildings and warps, wild and
trainer battles, the party screen and switching Pokémon, menus, saving, sound and cheats.

---

## 3. What's native and what isn't

There are four layers, from most native to least:

```
  ┌─────────────────────────────────────────────────────────────┐
  │ Game code (C + ARM/Thumb asm)                  NATIVE       │  runs as compiled ARM11 code
  │ NitroSystem / NitroSDK C (heaps, FS, G2D, sound player…)    │
  ├─────────────────────────────────────────────────────────────┤
  │ Runtime: SDK functions rewritten for the 3DS   NATIVE       │  plain C on libctru
  ├─────────────────────────────────────────────────────────────┤
  │ Runtime: DS hardware models                    EMULATED     │  game writes DS registers/VRAM,
  │ (2D compositor, geometry engine, sound mixer…)              │  we interpret them every frame
  ├─────────────────────────────────────────────────────────────┤
  │ Glue that keeps DS assumptions true            WORKAROUND   │  address shifting, trampolines…
  └─────────────────────────────────────────────────────────────┘
```

### 3a. Native: runs as real 3DS code, with no model of the DS in between

| Piece | Notes |
|---|---|
| All game logic, C and assembly | Compiled for the ARM11 and run directly. The assembly works as it is, but see 3c. |
| NitroSystem / NitroSDK code that doesn't touch hardware | OS heaps and arenas, mutexes, messages, FS, the NNS sound *player*, G2D data handling. Built from the decomp unchanged. |
| ROM and save access (`card.c`) | Reads from the `.nds` file through a block cache. The save is a raw `.sav` file. |
| Memory copies, DMA, decompression (`mi.c`) | Plain C. A DMA transfer has finished by the time the call returns. |
| Divider and square root (`cp.c`), BIOS SVCs and matrix maths (`sdk_asm.c`) | Software implementations of the same results. |
| Input, clock, power (`devices.c`) | Read from libctru's hid and the system clock. |
| Frame pacing, threads and cores, VBlank (`ctr.c`) | Real GSP VBlank events, with the render core and sound core on 3DS threads. |
| Logging, net log, cheats menu, settings, CIA packaging | Port-only features. |

### 3b. Emulated: the game still drives DS hardware, and we model that hardware

The game still writes to DS I/O registers, VRAM, OAM and palettes, and sends DS 3D command
lists. The runtime reads those back each frame and works out what the DS would have shown or
played. Everything here works, but it is not the native approach, and it is where our frame time
goes.

| Model | File(s) | What it does | Cost / problem |
|---|---|---|---|
| **DS address space** | `ds_mem.c`, `port/include/nitro/hw/ARM9/mmap_global.h` | Shadow copies of main RAM, I/O, VRAM, palettes and OAM, at DS address + `0x08000000`. | Uses ~19 MB of the 64 MB on an original 3DS. Every DS address in the code must be moved (see 3c). |
| **3D geometry engine** | `gx3d.c` | Runs the DS's 3D command stream on the CPU in DS fixed-point maths: matrices, lighting, clipping and polygon assembly. Hands transformed triangles to the PICA. | **The biggest cost:** ~8 ms per overworld frame (Azahar timing), about 9.4k commands per frame. |
| **2D engines** | `gpu2d.c` (GPU), `render2d.c` (software reference) | Reconstructs BG layers, sprites, windows and blending from DS registers and VRAM every frame. Uses hashing to skip unchanged textures. | ~4 ms per frame of CPU frame building on a New 3DS (hardware log). Has no H-blank effects, mosaic or bitmap sprites. |
| **Sound hardware** | `snd.c` | The ARM7 driver, compiled natively, programs 16 *modelled* DS channels. Our mixer renders them in software to NDSP. | Runs on its own core, so it costs the game little. Capture and reverb are not modelled. |
| **Interrupts** | `os.c` | No real interrupts. The game's VBlank handler runs from `OS_WaitIrq` once per frame. | Single game thread. `OS_CreateThread` and soft reset are fatal. |
| **Wireless** | `wm.c` | A radio with nobody in range, so the game's background wireless code never fails. | No Wi-Fi, wireless trading or Pokéwalker. |
| **ARM7 devices** | `devices.c` | The button and touch registers, plus the PXI FIFO between the game and the sound driver. | Thin, but still register-shaped. |

### 3c. Workarounds: glue that keeps DS assumptions true

These exist only because the game was built for a DS memory map and an MW toolchain. They are
the main source of hardware-only bugs.

| Workaround | Why it's fragile |
|---|---|
| **Address shifting in assembly** (`mwasm2gas.py`): data words, `mov`+`lsl` constants, and derived I/O bases are moved by `PORT_DS_DELTA`. | Pattern-based. Any address it misses works in Azahar and crashes on hardware. One known leftover: overlay 7 derives bits from WINOUT's address (`lsr r1, r0, #13`). |
| **Overlay trampolines** (`ovdispatch.py` and `overlay.c`) for the 360 targets in shared overlay memory. | Depends on the decomp's naming. A missed call runs the wrong overlay's code. |
| **Cheat address translation** (`cheatmap.py`) from DS addresses to GCC's layout. | Codes that point into unnamed BSS or overlay data are skipped. |
| **MW ABI emulation**: u64 aligned to 4, flag-returning float compares, and the decibel-table trailing zero. | Needed until the code is fully C and rebuilt with GCC's own ABI. |
| **Decomp patch** (`port/patches/pokeheartgold.patch`): literal DS VRAM addresses in three files. | Must be reapplied when the decomp updates. |
| **Assembly that writes 3D command registers directly** (`sub_02015E64`, `sub_020161CC`, `ov41_022465D8`). | Not yet routed through the port's command path. |

---

## 4. What needs to be rewritten to be native

The order follows payoff, meaning frame time saved and bug classes removed, against effort.
Each rewrite moves the boundary up a level: instead of modelling the DS hardware, the runtime
takes over the SDK or library call that would have driven it.

### 4.1 3D: replace the CPU geometry engine with a G3D-level PICA renderer (highest payoff)

- **Now:** the game's NNS G3D library builds DS command lists. `gx3d.c` interprets them on the
  CPU into screen-space triangles, which the PICA then draws.
- **Native:** hook at the NitroSystem G3D level (`NNS_G3dDraw`, the render object and material
  setup). Upload each model's vertices to the GPU once. Pass the matrices as vertex-shader
  uniforms, and let the PICA do the transform and lighting the way 3DS games do.
- **Smaller intermediate steps:**
  1. Finish the core-2 geometry worker (T1 in section 5).
  2. Cache display lists whose contents and state repeat. pspoke, a PSP port of the same game,
     cut outdoor 3D from 19 to 5 ms this way.
  3. Transform on the PICA while keeping the command stream as input.
- **Watch out for:** the game reads matrices back (clip matrix, position and box tests), so those
  answers must stay exact in DS fixed point.

### 4.2 2D: drive the GPU from the SDK's 2D calls, not from VRAM

- **Now:** each frame `gpu2d.c` reads DS BG and OBJ registers, VRAM, OAM and palettes, then
  rebuilds textures and sprite sheets from them.
- **Native:** catch the writes at the source (BgConfig / `GX_LoadBG*`, `GX_LoadOBJ`, palette
  loads, NNS G2D cell and OAM managers). Update GPU textures when those writes happen, and keep
  the sprite list as GPU state.
- **Benefits:** this removes most per-frame decoding and hashing, and makes H-blank effects,
  mosaic and bitmap sprites easier to add.
- **Watch out for:** the game also pokes VRAM directly with memcpy and DMA, so a shadow-VRAM
  fallback is still needed.

### 4.3 Assembly → C (removes the address-shifting layer)

- About 64% of functions are still assembly. They run natively, but every DS address inside them
  has to be found and moved by pattern rules. That is where most hardware-only crashes came from.
- Converting them to C lets the compiler handle addresses and removes `mwasm2gas`'s rewrite
  rules. `ctrl-cheeb-del/pokeheartgold` is a 100%-C fork that can serve as a reference or source
  (check its license first). Upstream decomp progress also helps over time.
- This also unlocks dropping the MW ABI emulation (3c), and later a smaller DS shadow memory.

### 4.4 Overlays as real code, not resident images plus trampolines

- Once calls into shared overlay memory resolve by symbol rather than address, the trampoline
  table and the "reset on load" trick can go.
- This mostly follows from 4.3. It can also be done by teaching the linker which overlay a call
  means at each call site.

### 4.5 Sound: map DS channels onto NDSP channels (low priority)

- **Now:** a software mixer renders the 16 DS channels and feeds the result to NDSP.
- **Native option:** give each DS channel its own NDSP channel. NDSP handles PCM8, PCM16 and
  DSP-ADPCM, but not IMA-ADPCM, PSG or noise, so some synthesis would remain.
- It already runs on its own core and works, so this is lowest on the list.

### 4.6 OS: real threads and interrupts

- Implement `OS_CreateThread` with 3DS threads (overlays 13 and 112 need it), add soft reset,
  and move VBlank work from "called in `OS_WaitIrq`" to a real event-driven path.

### 4.7 Wireless (optional, large)

- Today `wm.c` reports an empty radio. Native wireless would mean mapping WM onto 3DS local
  wireless (UDS). It is a separate project, so we only mention it here.

---

## 5. Task backlog

A menu of tasks to pick from. Each one has an ID to name when picking it up ("let's do T3a").

### How to read the estimates

Estimates are **Claude working time**: how long Claude spends actively on the task (reading,
building, running Azahar tests, fixing), not your time. They come from this project's own
history so far, for example:

| Past work | Claude time (from commit times) |
|---|---|
| The whole sound system: ARM7 driver on its own core, mixer, NDSP, loudness fix | ~1 h |
| Stereoscopic 3D, menu toggle included | ~20 min |
| A hardware-only crash, found from a log and fixed | ~30 min each |
| Cheats: usrcheat.dat, address translation, menu | ~2–3 h |
| The render core and pacing to VBlank | ~30 min, plus ~8 h of follow-up pacing and speed work |
| The geometry worker (T1) so far | ~45 min, unfinished |

- **Hardware checks are extra, and they're your time.** Anything that changes speed or GPU
  behaviour needs a run on a real 3DS, usually 15–30 minutes with the frame counter or the
  Wi-Fi log. Waiting on those adds wall-clock time, not Claude time.
- **One session** here means a few hours of continuous Claude work.
- **Tasks marked ⚠ can overrun:** an earlier attempt failed, or the design isn't settled.
- Sizes: **S** under 1 h, **M** 1–4 h, **L** 4–12 h (one or two sessions), **XL** more than
  12 h (several sessions).

### Summary

| ID | Task | Size | Claude time | Depends on |
|---|---|---|---|---|
| **T1** | Fix the New 3DS geometry worker's sprite bug | S–M | 30–90 min | — |
| **T2** | Measure speed on both 3DS models | S | ~15 min (plus your time at the 3DS) | T1 helps but isn't required |
| **T3a** | 3D display-list caching | M | 2–4 h | — |
| **T3b** | Native 3D prototype: the map drawn through the game's 3D library | L ⚠ | 4–8 h | — (T3a's findings help) |
| **T3c** | Native 3D for everything: characters, battles, effects | XL ⚠ | 15–30 h | T3b |
| **T4a** | Edge colours per polygon ID | S | 15–30 min | — |
| **T4b** | Fog | M | 1–2 h | — |
| **T4c** | Toon and highlight shading | S | 30–60 min | — |
| **T4d** | Anti-aliasing | M | 1–2 h | — |
| **T4e** | Translucent polygon sorting | M | 1–2 h | — |
| **T4f** | Rear-plane (a bitmap behind the 3D) | S | 30–60 min | — |
| **T4g** | Shadow polygons | M ⚠ | 2–4 h | — |
| **T4h** | Display capture (3D into VRAM, used for effects) | M ⚠ | 2–4 h | — |
| **T5** | Route the assembly's direct 3D writes through the command path | M | 1–2 h | — |
| **T6a** | Threads (`OS_CreateThread`) | M | 2–4 h | — |
| **T6b** | Soft reset | S–M | 30 min–2 h | — |
| **T6c** | Real interrupts (VBlank as an event, not a call in `OS_WaitIrq`) | L ⚠ | 4–8 h | T6a |
| **T7a** | Overlay 7's WINOUT address leftover | S | 15–45 min | — |
| **T7b** | Mosaic | M | 1–2 h | — |
| **T7c** | Bitmap sprites | S | 30–60 min | — |
| **T7d** | H-blank (raster) effects | M | 2–4 h | — |
| **T7e** | VRAM bank remapping | M ⚠ | 2–4 h | — |
| **T8** | Native 2D: GPU updated from the SDK's 2D calls, not rebuilt from VRAM | XL ⚠ | 8–20 h | — |
| **T9a** | Assembly → C, using the all-C fork as a reference (license check first) | XL ⚠ | 15–40 h | — |
| **T9b** | Overlays as real code: no trampolines, no "reset on load" | L | 3–6 h | T9a |
| **T9c** | Drop the Metrowerks ABI emulation | M | 2–4 h | T9a |
| **T10** | Sound: one 3DS sound channel per DS channel | L | 4–8 h | — |
| **T11** | Wireless over 3DS local wireless (UDS) | XL ⚠ | 40 h or more | T6a |
| **T12a** | RTC in local time | S | ~10 min | — |
| **T12b** | Microphone | M | 1–2 h | — |
| **T12c** | `.3dsx` build | S | 30–60 min | — |
| **T12d** | Cheat codes in unmapped regions | M | 1–2 h | — |
| **T13** | Docs: fold HANDOFF.md's open items into this file | S | 15–30 min | — |
| **T14** | ~~Bottom screen's blended layers drawn solid~~ fixed in Azahar; check on hardware | S | ~5 min (your time at the 3DS) | — |

**Quick wins (under an hour each):** T2, T4a, T4c, T4f, T7a, T7c, T12a, T12c, T13, and T1 if
the state diff finds the bug quickly.

### Native 3D: how the 3D tasks relate

**T3 is the native 3D work.** Today the game's 3D library builds DS command lists, and the port
interprets them on the CPU. The goal is to hook the library itself (`NNS_G3dDraw`, render
objects, materials), upload each model's vertices once, and give the 3DS GPU the matrices as
shader uniforms, the way 3DS games draw.

The other 3D tasks relate to it like this:

| Task | Relation to native 3D |
|---|---|
| **T1** geometry worker | **A stopgap.** Same emulated engine, moved to another core on a New 3DS. Gives speed now; mostly made unnecessary by T3c. |
| **T2** measurement | **Decides priority.** If the New 3DS already holds 60 fps after T1, native 3D matters mostly for the original 3DS XL. If not, T3 moves up. |
| **T3a** caching | **The first step of T3.** Cheap and proven: pspoke, a PSP port of the same game, cut outdoor 3D from 19 to 5 ms this way. Its lessons about which state repeats carry into T3b. |
| **T4a–h** effects | **Needed either way.** They're drawing features, so whichever path feeds the GPU needs them. Fog and toon shading become shader work in T3. Shadow polygons stay stencil tricks in both paths. Doing them now isn't wasted. |
| **T5** direct writes | **Why the old engine stays.** This assembly sends raw 3D commands past the 3D library, so T3 never sees it. Overlay 41 and two helper functions keep needing the emulated engine. |

**A constraint for every 3D path.** The game reads some results back from the DS's 3D hardware,
such as the clip matrix and position and box tests, and expects exact DS fixed-point answers. A
native path still has to compute those on the CPU, even when the GPU does the drawing.

### Task details

**T1: Fix the New 3DS geometry worker (uncommitted)**
- **Goal:** run `gx3d.c` on core 2 so the overworld reaches 60 fps at "Speed: double".
- **Status:** the command stream is identical with the worker on, and the map renders correctly.
  But the NPC and player billboard sprites (the frame's last polygons, attr `001F8081`) get
  w ≤ 0 for every vertex, so they're invisible.
- **Ruled out:** floating-point settings (FPSCR) and the sync points (the sync counters read 0).
- **Next step:** log the position matrix, projection, matrix mode and position stack pointer at
  the first sprite's BEGIN with `g3thread 0` and `g3thread 1`, and diff the two.
- **Constraints:** must stay New 3DS-only (`APT_CheckNew3DS`) and run inline on the original
  3DS XL. Off by default (`port_g3_thread = 0`) until the sprites are fixed.
- **Note:** Azahar runs all cores in turn, so only hardware shows the speed gain.

**T2: Measure on hardware (needs you at the 3DS)**
- The last log saved off the SD card (build `d4ac67e`, New 3DS, before the pacing changes) read
  about 57 fps, with ~4 ms of 2D frame building.
- Measure overworld and battles at normal and double speed, on both a New 3DS and the original
  3DS XL.
- The SD card is no longer connected, so read the on-screen frame counter or use the Wi-Fi log.

**T3a: Display-list caching**
- When a display list's contents and the 3D state it starts from repeat, reuse last frame's
  transformed triangles instead of running the geometry engine again.
- **The hard part** is knowing exactly which state a list depends on: matrices, lights,
  materials, and anything the game reads back.
- Verify pixel-for-pixel against the current path with `debug.txt` dumps.

**T3b: Native 3D prototype for the map ⚠**
- Draw the map's models through the game's 3D library, behind a `debug.txt` switch, and compare
  against the current path.
- **Watch out for:** readbacks (see the constraint above), widescreen and stereo hooks, seam
  fill and edge marking, all of which currently depend on screen-space triangles.

**T3c: Native 3D for everything ⚠**
- Characters and billboards, battle Pokémon and backgrounds, effects, animated textures and
  materials.
- Keep the emulated engine as the fallback for T5's assembly and for anything not yet moved.

**T4a–h: Missing 3D effects**
- **T4a, edge colours:** today one edge colour is used for all polygon IDs. HGSS sets all eight
  alike, so the visible gain is small.
- **T4b, fog:** fog colour and density table, per-polygon fog flag.
- **T4c, toon and highlight shading:** a table lookup on vertex colour.
- **T4d, anti-aliasing:** the field turns it on (DISP3DCNT bit 4). The DS smooths polygon edges.
  The GPU can approximate this by supersampling the 3D layer.
- **T4e, translucent sorting:** the DS's Y-sort and manual-sort modes for translucent polygons.
- **T4f, rear-plane:** a bitmap behind the 3D instead of a flat clear colour.
- **T4g, shadow polygons ⚠:** the DS's two-pass stencil shadows, mapped onto the GPU's stencil.
- **T4h, display capture ⚠:** some effects capture the 3D picture into VRAM and draw it again.
  It needs a GPU-to-shadow-VRAM path.

**T5: Direct 3D writes from assembly**
- `sub_02015E64`, `sub_020161CC` and `ov41_022465D8` write 3D command registers directly. Route
  them through the port's command path.
- HANDOFF.md's git history has a decoded plan for them.

**T6a–c: OS**
- **T6a, threads:** implement `OS_CreateThread` and the scheduler calls with 3DS
  threads. Overlays 13 and 112 need it; today it's fatal.
- **T6b, soft reset:** L+R+Start+Select is fatal today. The simplest fix relaunches the
  title; a true in-process reset needs all state reinitialised.
- **T6c, real interrupts ⚠:** VBlank as a real event rather than a call inside
  `OS_WaitIrq`. A design change with timing risk.

**T7a–e: 2D gaps**
- **T7a, WINOUT leftover:** overlay 7 derives bits from WINOUT's moved address
  (`lsr r1, r0, #13`) and gets different bits than on the DS.
- **T7b, mosaic.**
- **T7c, bitmap sprites.**
- **T7d, H-blank effects:** per-line register changes, such as wavy screens.
- **T7e, VRAM bank remapping ⚠:** today each VRAM window is its own memory. A first
  attempt to model the banks broke battles.

**T8: Native 2D ⚠**
- Update GPU textures when the game loads graphics (BgConfig, `GX_LoadBG*`, `GX_LoadOBJ`,
  palette loads, the 2D library's cell and sprite managers), instead of rebuilding everything
  from VRAM every frame.
- Keep a shadow-VRAM fallback, since the game also writes VRAM directly.

**T9a–c: Assembly → C and what it unlocks**
- **T9a ⚠:** removes the address-rewriting layer behind most hardware-only crashes.
  Check the all-C fork's license first; the fork's C may not match our build exactly.
- **T9b:** once calls resolve by name, the overlay trampolines can go.
- **T9c:** drop the copied Metrowerks quirks (u64 alignment, flag-returning float
  compares).

**T10: Native sound channels**
- Give each DS channel its own 3DS sound channel. 3DS hardware plays PCM8, PCM16 and its own
  ADPCM, but not the DS's ADPCM, square waves or noise, so some software synthesis stays.
- Today's mixer already works on its own core, so this is low priority.

**T11: Wireless ⚠**
- Map the DS wireless manager onto 3DS local wireless. A separate project.

**T12a–d: Smaller items**
- **T12a, RTC:** the clock uses UTC; switch to local time.
- **T12b, microphone.**
- **T12c, `.3dsx` build.**
- **T12d, cheat regions:** codes pointing into unnamed memory are skipped today.

**T13: Docs**
- Fold the still-valid parts of HANDOFF.md's "Open items" into this file and mark the rest as
  history.

**T14: Bottom screen's blended layers drawn solid (fixed; check on hardware)**
- **Was:** in battle, the move panels behind FIGHT (alpha-blended into the background on the DS)
  came out solid, in Azahar and on hardware, whatever the display options.
- **Cause:** blending and windows mark pixels in the stencil buffer, and the bottom screen's
  target (`sBottom`) has none. At 1:1 the layers were drawn straight onto it. Only the 1.25x path
  went through the DS-size intermediate (`sDsTarget`), which has a stencil.
- **Fix:** the bottom screen is always composed in `sDsTarget`, then copied at 1x or 1.25x.
  Azahar's GPU-vs-software check now reads 0 differing bottom pixels at 1:1 in battle.
- **Left:** look at the battle menu on hardware at 1:1 and at fill.

---

## 6. Where to look

| You want to… | Start at |
|---|---|
| Build, package, install | `README.md`, `GETTING_STARTED.md`, `port/build.py`, `port/package.sh` |
| Understand a runtime piece | The header comment at the top of each `port/runtime/*.c`. Each one explains what DS piece it replaces. |
| Debug in an emulator | HANDOFF.md, "Workflow that works": `tools/azrun.sh` and the `debug.txt` keys |
| Debug on hardware | HANDOFF.md, "Testing on a 3DS without the SD card": `tools/recv_log.py` and `tools/send_cia.py` |
| Find a crash address | `build-3ds/elves/<build ID>.elf` (the build ID is the first log line) and `build-3ds/soulsilver.map` |
| Avoid known traps | HANDOFF.md, "Conventions learned the hard way" and "Hardware-only bugs" |
