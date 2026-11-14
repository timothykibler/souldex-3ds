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
  1. Finish the core-2 geometry worker (see 5.1).
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

## 5. Next tasks

In priority order.

1. **Finish the New 3DS geometry worker (uncommitted).** Run `gx3d.c` on core 2 so the overworld
   reaches 60 fps at "Speed: double".
   - **Status:** it renders the map correctly with the same command stream, but the NPC and
     player billboard sprites (the frame's last polygons, attr `001F8081`) get w ≤ 0 for every
     vertex.
   - **Next step:** log the position matrix, projection, matrix mode and position stack pointer
     at the first sprite's BEGIN with `g3thread 0` and `g3thread 1`, and diff them.
   - **Constraints:** it must stay New 3DS-only (`APT_CheckNew3DS`) and run inline on the
     original 3DS XL. It is defaulted off (`port_g3_thread = 0`) until the sprites are fixed.
2. **Measure on hardware after the pacing changes.** The last log saved off the SD card (build
   `d4ac67e`, New 3DS) read about 57 fps with ~4 ms of 2D frame building. Measure overworld and
   battles at normal and double speed on both a New 3DS and the original 3DS XL. The SD card is
   no longer connected, so read the frame counter or use the Wi-Fi net log.
3. **Start the native 3D path (4.1).** Begin with display-list caching, since it is cheap and
   proven by pspoke. Then prototype G3D-level drawing for one model type (the map) behind a
   `debug.txt` switch, compared pixel-for-pixel against the current path.
4. **Missing 3D features:** anti-aliasing (the field sets DISP3DCNT bit 4), shadow polygons,
   fog, toon and highlight shading, translucent Y-sorting, rear-plane, display capture, and
   per-polygon-ID edge colours.
5. **Route direct 3D register writes from assembly** (overlay 41, and `sub_02015E64` and
   `sub_020161CC`) through the command path.
6. **OS gaps:** threads (`OS_CreateThread`, overlays 13 and 112) and soft reset, both fatal
   today.
7. **2D gaps:** H-blank (raster) effects, mosaic, bitmap sprites, VRAM bank remapping, and the
   overlay 7 WINOUT address bit leftover.
8. **Smaller:** the RTC uses UTC instead of local time, there is no microphone, there is no
   `.3dsx` build, and cheat codes in unmapped regions are skipped.
9. **Docs:** fold the still-valid parts of HANDOFF.md's "Open items" into this file and mark the
   rest as history.

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
