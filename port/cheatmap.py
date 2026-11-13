"""Fills the cheat engine's DS -> port address table (port_cheat_map, runtime/cheats.c) in the
linked ELF, in place (nothing else moves).

Cheat codes write DS addresses. The port's game data is laid out by GCC, elsewhere, but the decomp
names much of it after its DS address (_021D10A8, s_021D10E8, ...): each such symbol is an
anchor. A DS address translates when it lies inside an anchor's object (its size), or between two
anchors whose spacing is the same in the port as on the DS (the memory between is laid out alike).

Table: u32 count, then count x (DS address, port address, length), sorted by DS address.

    python3 port/cheatmap.py build-3ds/soulsilver.elf
"""
import re
import struct
import subprocess
import sys

NM = "/opt/devkitpro/devkitARM/bin/arm-none-eabi-nm"
CAP = 8192
NAMED = re.compile(r"(?:^|_)(02[0-9A-F]{6})(?:\.\d+)?$")


def symbols(elf):
    out = subprocess.run([NM, "-n", "-S", elf], capture_output=True, text=True, check=True).stdout
    syms = []
    for line in out.splitlines():
        p = line.split()
        if len(p) == 4:
            syms.append((int(p[0], 16), int(p[1], 16), p[2], p[3]))
        elif len(p) == 3:
            syms.append((int(p[0], 16), 0, p[1], p[2]))
    return syms


def table(syms):
    ports = sorted({s[0] for s in syms})
    import bisect
    anchors = {}
    for addr, size, kind, name in syms:
        if kind not in "bBdDrR":
            continue
        m = NAMED.search(name)
        if not m:
            continue
        ds = int(m.group(1), 16)
        if not 0x02000000 <= ds < 0x02400000:
            continue
        if size == 0:  # (an assembly label: up to the next symbol)
            i = bisect.bisect_right(ports, addr)
            size = ports[i] - addr if i < len(ports) else 0
        anchors[ds] = (addr, size)
    rows = []
    items = sorted(anchors.items())
    for i, (ds, (port, size)) in enumerate(items):
        length = size
        if i + 1 < len(items):
            nds, (nport, _) = items[i + 1]
            if nport - port == nds - ds:
                length = nds - ds  # (laid out alike up to the next anchor)
            else:
                length = min(length, nds - ds)
        if length > 0:
            rows.append((ds, port, length))
    return rows


def patch(elf, addr, data):
    b = bytearray(open(elf, "rb").read())
    shoff, = struct.unpack_from("<I", b, 0x20)
    shentsize, shnum = struct.unpack_from("<HH", b, 0x2E)
    for i in range(shnum):
        _, stype, _, saddr, soff, ssize = struct.unpack_from("<IIIIII", b, shoff + i * shentsize)
        if stype != 8 and saddr <= addr < saddr + ssize:  # (not NOBITS)
            o = soff + addr - saddr
            b[o:o + len(data)] = data
            open(elf, "wb").write(b)
            return
    raise SystemExit("cheatmap: port_cheat_map not in a file section")


def main():
    elf = sys.argv[1]
    syms = symbols(elf)
    at = [s for s in syms if s[3] == "port_cheat_map"]
    if not at:
        raise SystemExit("cheatmap: no port_cheat_map")
    rows = table(syms)[:CAP]
    data = struct.pack("<I", len(rows)) + b"".join(struct.pack("<III", *r) for r in rows)
    if len(data) > at[0][1]:
        raise SystemExit("cheatmap: table too big")
    patch(elf, at[0][0], data)
    print(f"cheatmap: {len(rows)} DS address ranges")


if __name__ == "__main__":
    main()
