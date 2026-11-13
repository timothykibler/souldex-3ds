#!/bin/sh
# One-shot test in Azahar: writes debug.txt, launches the ELF, waits for shot_<FRAME>.bmp (or a
# timeout), quits Azahar, converts the shots to PNG under captures/run/ and prints the log tail.
#   AZAHAR=/path/Azahar.app tools/azrun.sh <frame> [timeout_s] < debug-lines
# e.g. printf 'dump 300\n' | tools/azrun.sh 2100 120
set -e
FRAME=$(printf '%05d' "$1"); TIMEOUT=${2:-120}
ROOT=$(cd "$(dirname "$0")/.." && pwd)
AZ="$HOME/Library/Application Support/Azahar"
SD="$AZ/sdmc/3ds/soulsilver-3ds"
CFG="$AZ/config/qt-config.ini"
: "${AZAHAR:?set AZAHAR to Azahar.app}"
pkill -9 -f "MacOS/azahar" 2>/dev/null || true
sleep 1
cat > "$SD/debug.txt"
rm -f "$SD"/shot_*.bmp "$SD"/full_*.bmp
for k in check_for_update_on_start=false firstStart=false confirmClose=false use_gdbstub=false; do
  key=${k%%=*}; sed -i '' "s/^$key=.*/$k/" "$CFG"
done
TMP=$(mktemp -d)/game.elf
cp "$ROOT/build-3ds/soulsilver.elf" "$TMP"
open -n -a "$AZAHAR" --args -w "$TMP"
t=0
while [ ! -f "$SD/shot_$FRAME.bmp" ] && [ $t -lt "$TIMEOUT" ]; do sleep 2; t=$((t + 2)); if grep -aq FATAL "$SD/log.txt" 2>/dev/null; then break; fi; done
sleep 2
pkill -9 -f "MacOS/azahar" 2>/dev/null || true
OUT="$ROOT/captures/run"; rm -rf "$OUT"; mkdir -p "$OUT"
for f in "$SD"/shot_*.bmp "$SD"/full_*.bmp; do [ -f "$f" ] && sips -s format png "$f" --out "$OUT/$(basename "${f%.bmp}").png" >/dev/null; done
ls "$OUT"
grep -aE "FATAL|fps|differ|wide" "$SD/log.txt" | tail -15
