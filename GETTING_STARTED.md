# Getting started

How to build the port, put it on your 3DS and play. This repository contains no part of the game:
you need your own copy of Pokémon SoulSilver (the US release), and everything you build from it
is for your own use only. Don't share ROMs, saves made from them, or the CIA you build.

## What you need

- **Your own SoulSilver**: a ROM file dumped from your own cartridge (US version, game code IPGE).
- **A Mac or Linux PC with Docker** (Windows works through WSL), for building.
- **A 3DS that can install CIAs.** That means custom firmware, which is your choice and your
  responsibility; follow a reputable, up-to-date guide for your model. A New 3DS or New 3DS XL is
  recommended: the overworld runs fastest there.
- **About 100 MB free** on the 3DS's SD card.

## 1. Build it

From this repository's folder, once:

    git clone https://github.com/pret/pokeheartgold.git
    (cd pokeheartgold && git apply ../port/patches/pokeheartgold.patch)
    docker build -t ss3ds-dev docker

then follow the "Build" section of [README.md](README.md) (one command generates the decomp's
headers). Then, and after any change:

    docker run --rm -v "$PWD":/w -w /w ss3ds-dev python3 port/build.py
    SS3DS_ROM=/path/to/your/soulsilver.nds port/package.sh

That makes `build-3ds/soulsilver.cia`. The HOME Menu icon, banner and title are made from your
ROM while packaging. (Optional: your own artwork for a layered 3D banner, see `port/package.sh`.)

## 2. Put the files on the SD card

Make the folder `sd:/3ds/soulsilver-3ds/` and copy into it:

| File | What it is |
|---|---|
| `soulsilver.nds` | Your ROM (required: the game reads its graphics, maps, music and text from it). |
| `soulsilver.sav` | Optional: your existing save, in the raw `.sav` format that flashcards and TWiLight Menu++ use. Without one, the game starts fresh. |
| `usrcheat.dat` | Optional: a cheat database in the R4 / TWiLight Menu++ format, for the cheat menu. |

**Sound** needs the 3DS's DSP firmware at `sd:/3ds/dspfirm.cdc`. If you've ever played homebrew
with sound you probably have it; if not, make it on your own console with the DSP1 homebrew app.
Without it the game plays silently.

## 3. Install and play

Install `soulsilver.cia` with a CIA installer (e.g. FBI) and start it from the HOME Menu.

**Controls** are the DS's: A, B, X, Y, L, R, Start, Select, the D-pad or Circle Pad, and the touch
screen on the bottom.

**The menu**: press **ZL+ZR** (or **L+R+Select**) at any time. The bottom screen shows:

- *Top screen*: *1:1* shows the DS picture with exact pixels (the overworld's 3D fills the whole
  wide screen); *fill* stretches it to the screen's height at 1.25x.
- *Sides (fill)*: in fill mode, black sides or the picture's edges continued.
- *3D*: stereoscopic 3D on or off. With it on, the 3D slider adds depth to the overworld (top
  screen, 1:1 mode); battles stay flat by design. Turn it off if the game feels slower.
- Your cheat codes, by folder: A turns one on or off, B closes the menu.

Choices are saved (`settings.txt`, `cheats.txt` in the same folder). The bottom screen always
fills its screen; while the 3D slider is up it's drawn more simply (a few doubled pixel rows), to
leave the 3DS's video memory for the second eye.

**Saving** works as in the game (Save from the start menu): the save goes to
`sd:/3ds/soulsilver-3ds/soulsilver.sav`. Back it up now and then.

## If something goes wrong

- **It stops on a text screen** at start-up: the message says what's missing (usually the ROM in
  the wrong place or the wrong version).
- **A crash or freeze**: the log is at `sd:/3ds/soulsilver-3ds/log.txt`; the last lines say what it
  was doing. Luma3DS's crash screen (if one appears) is useful too: a photo of it helps.
- **Known gaps**: soft reset (L+R+Start+Select) stops the game with an error instead of restarting it (quit and start it again); Wi-Fi,
  wireless and the Pokéwalker don't connect; a few 3D effects (anti-aliasing, fog) aren't drawn.
  See [HANDOFF.md](HANDOFF.md) for the full list.
