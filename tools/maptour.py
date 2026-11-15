#!/usr/bin/env python3
"""Map tour: warps through every map in Azahar and reports, per map, the unmapped memory accesses
(each one a crash on hardware) and crashes, so bugs show up without playing to each place.

    AZAHAR=/path/Azahar.app tools/maptour.py [--start MAP] [--count N] [--batch 40] [--maps 34,29]

Each Azahar run boots the save (Continue), then warps to BATCH maps, SPACING frames apart (a warp
waits until the field runs no task). debug.txt `warpmark` makes each warp print "maptour: map N"
to the debugger: in Azahar's log (tools/azrun.sh turns on instant logging and that category), the
unmapped accesses after a marker belong to that map.
A crash, or a map whose field never goes idle (later warps never run), ends the run; the next run
starts after that map. The report: captures/maptour.txt (appended), PCs resolved to functions.
Needs a debug build (build-3ds/soulsilver.elf) and the overworld save (see HANDOFF.md).
"""
import argparse
import json
import os
import re
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DECOMP = os.path.join(ROOT, "pokeheartgold")
AZ = os.path.expanduser("~/Library/Application Support/Azahar")
AZLOG = os.path.join(AZ, "log", "azahar_log.txt")
GAMELOG = os.path.join(AZ, "sdmc", "3ds", "soulsilver-3ds", "log.txt")
REPORT = os.path.join(ROOT, "captures", "maptour.txt")
BOOT = "mash 300 600 30 START\npress 700 A\n"  # (intro, title, Continue)
FIRST_WARP = 900


def map_list():
    """[(map id, name, x, z)]: each map with a place to stand (its first warp, else first object)."""
    ids = {}
    for m in re.finditer(r"#define\s+(MAP_\w+)\s+(\d+)", open(f"{DECOMP}/include/constants/maps.h").read()):
        ids[m.group(1)] = int(m.group(2))
    headers = open(f"{DECOMP}/src/data/map_headers.h").read()
    events = dict(re.findall(r"\[(MAP_\w+)\]\s*=\s*\{.*?\.eventsBank\s*=\s*NARC_zone_event_(\w+)_bin", headers, re.S))
    out = []
    for name, ev in events.items():
        if name not in ids or ev.endswith("DUMMY"):
            continue
        path = f"{DECOMP}/files/fielddata/eventdata/zone_event/{ev}.json"
        if not os.path.exists(path):
            continue
        d = json.load(open(path))
        spot = (d.get("warps") or d.get("objects") or [None])[0]
        if spot:
            out.append((ids[name], name, spot["x"], spot["z"]))
    return sorted(out)


def symbolize(pcs):
    if not pcs:
        return {}
    r = subprocess.run(["docker", "run", "--rm", "-v", f"{ROOT}:/w", "-w", "/w", "ss3ds-dev",
                        "/opt/devkitpro/devkitARM/bin/arm-none-eabi-addr2line", "-f", "-e",
                        "build-3ds/soulsilver.elf", *pcs], capture_output=True, text=True)
    lines = r.stdout.split("\n")
    return {pc: lines[2 * i] for i, pc in enumerate(pcs) if 2 * i < len(lines)}


def run_batch(batch, spacing):
    lines = [BOOT, "warpmark\n"]
    for k, (mid, _, x, z) in enumerate(batch):
        lines.append(f"warp {FIRST_WARP + k * spacing} {mid} {x} {z}\n")
    end = FIRST_WARP + len(batch) * spacing + 200
    lines.append(f"dump {end}\n")  # (azrun waits for this frame's shot)
    timeout = end // 25 + 60
    env = dict(os.environ)
    p = subprocess.run([f"{ROOT}/tools/azrun.sh", str(end), str(timeout)], input="".join(lines),
                       capture_output=True, text=True, env=env, cwd=ROOT)
    crashed = "CRASH" in p.stdout or "exited" in p.stdout
    azlog = open(AZLOG, errors="replace").read() if os.path.exists(AZLOG) else ""
    gamelog = open(GAMELOG, errors="replace").read() if os.path.exists(GAMELOG) else ""
    warped = [int(m) for m in re.findall(r"^warp: map (\d+)", gamelog, re.M)]
    per_map, cur = {}, None
    for l in azlog.split("\n"):
        mk = re.search(r"maptour: map (\d+)", l)
        if mk:
            cur = int(mk.group(1))
            per_map.setdefault(cur, {})
            continue
        m = re.search(r"unmapped (\w+) (?:0x[0-9A-Fa-f]+ )?@ (0x[0-9A-Fa-f]+) at PC (0x[0-9A-Fa-f]+)", l)
        if not m:
            continue
        key = (m.group(1), m.group(3))
        bucket = per_map.setdefault(cur, {})
        bucket[key] = bucket.get(key, 0) + 1
    fatal = re.findall(r"FATAL.*", gamelog)
    return warped, per_map, crashed, p.stdout, fatal


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--start", type=int, default=0, help="first map id")
    ap.add_argument("--count", type=int, default=10000, help="maps to visit")
    ap.add_argument("--batch", type=int, default=40)
    ap.add_argument("--spacing", type=int, default=200, help="frames between warps")
    ap.add_argument("--maps", help="only these map ids, comma-separated")
    a = ap.parse_args()
    if not os.environ.get("AZAHAR"):
        sys.exit("set AZAHAR to Azahar.app")
    maps = map_list()
    if a.maps:
        want = {int(x) for x in a.maps.split(",")}
        maps = [m for m in maps if m[0] in want]
    else:
        maps = [m for m in maps if m[0] >= a.start][:a.count]
    names = {m[0]: m[1] for m in maps}
    os.makedirs(os.path.dirname(REPORT), exist_ok=True)
    rep = open(REPORT, "a")
    i = 0
    while i < len(maps):
        batch = maps[i:i + a.batch]
        print(f"maptour: maps {batch[0][0]}..{batch[-1][0]} ({len(batch)})", flush=True)
        warped, per_map, crashed, out, fatal = run_batch(batch, a.spacing)
        pcs = sorted({pc for b in per_map.values() for (_, pc) in b})
        fn = symbolize(pcs)
        done = 0
        for mid, *_ in batch:
            if mid not in warped:
                break
            done += 1
            issues = per_map.get(mid, {})
            if issues:
                desc = ", ".join(f"{n}x {kind} at {pc} {fn.get(pc, '?')}" for (kind, pc), n in sorted(issues.items()))
                line = f"map {mid} {names.get(mid, '')}: {desc}"
                print("  " + line, flush=True)
                rep.write(line + "\n")
        last = warped[-1] if warped else None
        if done < len(batch):
            why = "crash" if crashed else ("FATAL " + fatal[-1] if fatal else "field never idle (or the run timed out)")
            blame = last if last is not None else batch[0][0]
            line = f"map {blame} {names.get(blame, '')}: run ended: {why}"
            if crashed:
                line += " | " + " ".join(l.strip() for l in out.split("\n") if "PC" in l or " at " in l)[:300]
            print("  " + line, flush=True)
            rep.write(line + "\n")
            # (resume after the map the run ended on; none warped: skip the first, it can't be reached)
            done = max(done, 1)
        rep.flush()
        i += done
    print(f"maptour: report in {REPORT}")


if __name__ == "__main__":
    main()
