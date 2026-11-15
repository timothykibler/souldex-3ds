#!/usr/bin/env bash
# Makes build-3ds/soulsilver.cia from build-3ds/soulsilver.elf (after port/build.py).
# Uses the ss3ds-cia image (devkitARM + makerom + bannertool + Pillow: docker/cia/Dockerfile), built here if missing.
# HOME Menu art (icon, banner, titles) comes from your ROM (tools/cia_art.py): SS3DS_ROM=path/to/rom.nds,
# or the copy Azahar uses; without one, the plain placeholders in port/. Your own artwork for the banner
# and icon (kept out of git): SS3DS_KEYART=image, or build-3ds/keyart.(webp|png|jpg): a layered,
# transparent, stereoscopic banner (tools/cgfx_banner.py).
set -euo pipefail
cd "$(dirname "$0")/.."
docker image inspect ss3ds-cia >/dev/null 2>&1 || docker build -q -t ss3ds-cia docker/cia >/dev/null
ROM="${SS3DS_ROM:-$HOME/Library/Application Support/Azahar/sdmc/3ds/soulsilver-3ds/soulsilver.nds}"
ROMARG=()
if [ -f "$ROM" ]; then ROMARG=(-v "$ROM":/rom.nds:ro); else echo "package: no ROM at $ROM: placeholder art"; fi
KEYART="${SS3DS_KEYART:-$(ls build-3ds/keyart.* 2>/dev/null | head -1 || true)}"
if [ -n "$KEYART" ] && [ -f "$KEYART" ]; then ROMARG+=(-v "$(cd "$(dirname "$KEYART")" && pwd)/$(basename "$KEYART")":/keyart:ro); echo "package: banner and icon from $KEYART"; fi
docker run --rm -v "$PWD":/w "${ROMARG[@]}" -w /w/build-3ds ss3ds-cia sh -c '
  set -e
  mkdir -p art
  if [ -f /rom.nds ]; then
    if [ -f /keyart ]; then python3 /w/tools/cia_art.py /rom.nds art --keyart /keyart; else python3 /w/tools/cia_art.py /rom.nds art; fi
    ICON=art/icon.png; BANNER=art/banner.png
    SHORT=$(sed -n 1p art/titles.txt); LONG=$(sed -n 2p art/titles.txt); PUB=$(sed -n 3p art/titles.txt)
  else
    ICON=/w/port/icon.png; BANNER=/w/port/icon-banner.png
    SHORT="SoulSilver 3DS"; LONG="SoulSilver 3DS port"; PUB="personal build"
  fi
  bannertool makesmdh -s "$SHORT" -l "$LONG" -p "$PUB" -i "$ICON" -o soulsilver.smdh >/dev/null
  bannertool makebanner -i "$BANNER" -a /w/port/silence.wav -o soulsilver.bnr >/dev/null
  if [ -f art/banner_front.png ] && [ -f art/banner_back.png ]; then  # (layered: stereoscopic, full colour)
    python3 /w/tools/cgfx_banner.py soulsilver.bnr art/banner_front.png art/banner_back.png art/banner.cgfx
    bannertool makebanner -ci art/banner.cgfx -a /w/port/silence.wav -o soulsilver.bnr >/dev/null
  fi
  arm-none-eabi-strip -o soulsilver-stripped.elf soulsilver.elf 2>/dev/null || /opt/devkitpro/devkitARM/bin/arm-none-eabi-strip -o soulsilver-stripped.elf soulsilver.elf
  RSF=/w/port/cia.rsf  # (port/cia.local.rsf instead, if present: a personal build'"'"'s own settings, git-ignored)
  if [ -f /w/port/cia.local.rsf ]; then RSF=/w/port/cia.local.rsf; echo "package: using port/cia.local.rsf"; fi
  makerom -f cia -o soulsilver.cia -rsf "$RSF" -target t -exefslogo -elf soulsilver-stripped.elf -icon soulsilver.smdh -banner soulsilver.bnr
'
# (the ELF kept under the build's ID, which the game logs at start-up: crash dumps resolve against it)
if [ -f build-3ds/gen/build_id.txt ]; then
  mkdir -p build-3ds/elves && cp build-3ds/soulsilver.elf "build-3ds/elves/$(cat build-3ds/gen/build_id.txt).elf"
  echo "package: build $(cat build-3ds/gen/build_id.txt)"
fi
ls -la build-3ds/soulsilver.cia
