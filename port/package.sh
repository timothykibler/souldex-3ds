#!/usr/bin/env bash
# Makes build-3ds/soulsilver.cia from build-3ds/soulsilver.elf (after port/build.py).
# Uses the fulldex-3ds-builder image (devkitARM + makerom + bannertool).
set -euo pipefail
cd "$(dirname "$0")/.."
docker run --rm -v "$PWD":/w -w /w/build-3ds fulldex-3ds-builder sh -c '
  set -e
  bannertool makesmdh -s "SoulSilver 3DS" -l "SoulSilver 3DS port, phase 0" -p "personal build" -i /w/port/icon.png -o soulsilver.smdh >/dev/null
  bannertool makebanner -i /w/port/icon-banner.png -a /w/port/silence.wav -o soulsilver.bnr >/dev/null
  arm-none-eabi-strip -o soulsilver-stripped.elf soulsilver.elf 2>/dev/null || /opt/devkitpro/devkitARM/bin/arm-none-eabi-strip -o soulsilver-stripped.elf soulsilver.elf
  makerom -f cia -o soulsilver.cia -rsf /w/port/cia.rsf -target t -exefslogo -elf soulsilver-stripped.elf -icon soulsilver.smdh -banner soulsilver.bnr
'
ls -la build-3ds/soulsilver.cia
