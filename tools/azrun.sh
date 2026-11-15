#!/bin/sh
# One-shot test in Azahar: writes debug.txt, launches the ELF, waits for shot_<FRAME>.bmp (or a
# timeout), quits Azahar, converts the shots to PNG under captures/run/ and prints the log tail.
#   AZAHAR=/path/Azahar.app tools/azrun.sh <frame> [timeout_s] < debug-lines
# e.g. printf 'dump 300\n' | tools/azrun.sh 2100 120
set -e
# (stops early on a crash: Azahar's exception dump, a FATAL in the game's log, or Azahar exiting)
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
# (instant_debug_log: written as it happens; buffered, the log lost its end, unmapped accesses too)
for k in check_for_update_on_start=false firstStart=false confirmClose=false use_gdbstub=false instant_debug_log=true "log_filter=*:Info Debug.Emulated:Debug"; do
  key=${k%%=*}; sed -i '' "s/^$key=.*/$k/; s/^$key\\\\default=.*/$key\\\\default=false/" "$CFG"
done
TMP=$(mktemp -d)/game.elf
cp "$ROOT/build-3ds/soulsilver.elf" "$TMP"
open -n -a "$AZAHAR" --args -w "$TMP"
t=0
AZLOG="$AZ/log/azahar_log.txt"  # (Azahar starts it afresh at each launch; a crash dumps registers there)
sleep 3
CRASH=
while [ ! -f "$SD/shot_$FRAME.bmp" ] && [ $t -lt "$TIMEOUT" ]; do
  sleep 1; t=$((t + 1))
  if grep -aq FATAL "$SD/log.txt" 2>/dev/null; then break; fi
  if grep -aq "Exception Type" "$AZLOG" 2>/dev/null; then CRASH=1; break; fi
  if ! pgrep -f "MacOS/azahar" >/dev/null; then echo "azrun: Azahar exited"; break; fi
done
if [ -n "$CRASH" ]; then
  echo "azrun: CRASH after ${t}s: $(grep -a "Exception Type" "$AZLOG" | tail -1)"
  PC=$(grep -a " PC " "$AZLOG" | tail -1 | sed -E 's/.*PC +(0x[0-9A-Fa-f]+).*/\1/')
  LR=$(grep -a " LR " "$AZLOG" | tail -1 | sed -E 's/.*LR +(0x[0-9A-Fa-f]+).*/\1/')
  echo "azrun: PC $PC LR $LR:"
  docker run --rm -v "$ROOT":/w -w /w ss3ds-dev /opt/devkitpro/devkitARM/bin/arm-none-eabi-addr2line -f -i -p -e build-3ds/soulsilver.elf "$PC" "$LR" 2>/dev/null | sed 's/^/  /'
  echo "azrun: last game log lines:"; tail -4 "$SD/log.txt" | sed 's/^/  /'
fi
sleep 2
# (quit, not kill: Azahar buffers its log, and -9 loses the end of it: unmapped accesses too)
osascript -e "tell application id \"$(defaults read "$AZAHAR/Contents/Info" CFBundleIdentifier)\" to quit" >/dev/null 2>&1 || pkill -TERM -f "MacOS/azahar" 2>/dev/null || true
for i in 1 2 3 4 5 6 7 8 9 10; do pgrep -f "MacOS/azahar" >/dev/null || break; sleep 0.5; done
pkill -9 -f "MacOS/azahar" 2>/dev/null || true
OUT="$ROOT/captures/run"; rm -rf "$OUT"; mkdir -p "$OUT"
for f in "$SD"/shot_*.bmp "$SD"/full_*.bmp; do [ -f "$f" ] && sips -s format png "$f" --out "$OUT/$(basename "${f%.bmp}").png" >/dev/null; done
ls "$OUT"
grep -aE "FATAL|fps|differ|wide" "$SD/log.txt" | tail -15
# (bad addresses: a crash on hardware, only a logged unmapped access in Azahar)
N=$(grep -a -ic "unmapped" "$AZ/log/azahar_log.txt" 2>/dev/null || true)
echo "azrun: unmapped memory accesses: ${N:-0}"
[ "${N:-0}" != "0" ] && grep -a -i "unmapped" "$AZ/log/azahar_log.txt" | head -3
