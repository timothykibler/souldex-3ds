"""Metrowerks ARM assembler syntax (as pokeheartgold writes it) to GNU as syntax."""
import os
import re

_SECTIONS = {".rodata": ".section .rodata", ".bss": ".section .bss", ".sbss": ".section .sbss",
             ".ctor": ".section .ctor,\"aw\"", ".init": ".section .init,\"ax\""}
_DATA_WORD = re.compile(r"^(\s*)\.(word|short)\b")
_DATA_LIST = re.compile(r"^\s*\.(byte|short|word|2byte|4byte|hword)\b")
_INCLUDE = re.compile(r'^\s*\.include\s+"([^"]+)"')


def inline_includes(path, dirs, seen=None):
    """The file with its .include lines replaced by the files' text (each file once), so the
    preprocessor and the converter see everything (GNU as would read .include files unconverted)."""
    seen = set() if seen is None else seen
    out = []
    here = os.path.dirname(path)
    for line in open(path, encoding="utf-8", errors="replace"):
        m = _INCLUDE.match(line)
        if m:
            name = m.group(1)
            for d in [here] + dirs:
                cand = os.path.normpath(os.path.join(d, name))
                if os.path.exists(cand):
                    if cand not in seen:
                        seen.add(cand)
                        text = inline_includes(cand, dirs, seen)
                        out.append(text if text.endswith("\n") else text + "\n")
                    break
            else:
                out.append(line)  # (generated .inc not found: leave it to as)
            continue
        if line.strip() == "#pragma once":
            continue
        out.append(line)
    return "".join(out)


def _strip_comment(line):
    # ';' starts a comment in mwasm (outside strings); in GNU as it separates statements.
    out, q = [], None
    for ch in line:
        if q:
            out.append(ch)
            if ch == q:
                q = None
        elif ch in "\"'":
            q = ch
            out.append(ch)
        elif ch == ";":
            break
        else:
            out.append(ch)
    return "".join(out)


_SECTION_ALIGN = re.compile(r"^(\s*)\.section\s+([.\w$]+)\s*,\s*(\d+)(?:\s*,.*)?$")


# DS hardware addresses the game's assembly uses as raw literals, and which the 3DS port moves by
# PORT_DS_DELTA (port/include/port_ds.h): DTCM and the system area at the top of main memory, I/O
# registers, palettes, VRAM and OAM. Other literals in these ranges are numbers, not addresses.
PORT_DS_DELTA = 0x08000000
_DS_RANGES = [(0x027E0000, 0x02800000), (0x04000000, 0x04002000), (0x04100000, 0x04101000),
              (0x05000000, 0x05000800), (0x06000000, 0x068A4000), (0x07000000, 0x07000800)]
_HEX = re.compile(r"\b0x([0-9a-fA-F]{8})\b")


def _move(m):
    v = int(m.group(1), 16)
    if any(lo <= v < hi for lo, hi in _DS_RANGES):
        return f"0x{v + PORT_DS_DELTA:08X}"
    return m.group(0)


def convert(text, move_ds_addresses=False, phase=None):
    """phase: {section: k}, a section that started k bytes past a word boundary on the DS
    (its labels at DS addresses = k mod 4 from their offsets): it starts so here too."""
    phase = phase or {}
    lines = []
    aligned = set()
    in_code = True
    for line in text.splitlines():
        line = _strip_comment(line).rstrip()
        # A trailing comma after data (".byte 0xA8, 0x00,"): nothing to Metrowerks, one more zero
        # to GNU as, which shifts every table after it off its DS address (and alignment).
        if line.endswith(",") and _DATA_LIST.match(line):
            line = line.rstrip(",").rstrip()
        stripped = line.strip()
        if stripped in _SECTIONS or stripped in (".data", ".text"):
            in_code = stripped in (".text", ".init")
            line = _SECTIONS.get(stripped, stripped)
            # Metrowerks starts a file's sections word-aligned; GNU as gives data sections an
            # alignment of 1, so the linker could put a table at an odd address and the game's
            # word loads from it (ldm, ldr) fault on the 3DS. Once per section and file: later
            # returns to it continue its layout as on the DS. (Some files' data started 2 bytes
            # past a word on the DS: phase, measured by port/compile_game.py, puts it there.)
            if stripped not in aligned:
                aligned.add(stripped)
                line += "\n\t.balign 4" + (f"\n\t.space {phase[stripped]}" if phase.get(stripped) else "")
        elif stripped.startswith(".public "):
            line = line.replace(".public", ".global", 1)
        else:
            m = _SECTION_ALIGN.match(line)
            if m:
                in_code = m.group(2).startswith(".text")
                flags = '"ax"' if in_code else '"aw"'
                line = f"{m.group(1)}.section {m.group(2)},{flags}\n{m.group(1)}.balign {m.group(3)}"
        line = re.sub(r"@(object|function)\b", r"%\1", line)
        if move_ds_addresses and (".word" in line or "=" in line):
            line = _HEX.sub(_move, line)
        # In data, GNU as pads before .word and .short to align them; Metrowerks packs data as
        # written. .4byte / .2byte are the same values, never padded: the DS layout, byte for byte.
        if not in_code:
            line = _DATA_WORD.sub(lambda d: d.group(1) + (".4byte" if d.group(2) == "word" else ".2byte"), line)
        lines.append(line)
    if move_ds_addresses:
        _move_io_base_immediates(lines)
    return "\n".join(lines) + "\n"


_LSL26 = re.compile(r"\s*lsls?\s+(r\d),\s*(r\d),\s*#0x1a\b")


def _move_io_base_immediates(lines):
    """Thumb code builds the I/O base 0x04000000 as `mov rN, #1` ... `lsl rD, rN, #26`. Where the
    result is then used as an address (a load/store base, or address arithmetic), make it
    3 << 26 = 0x0C000000, the moved base: same instructions, same size. (Where 1 << 26 is a bit
    mask, orr/tst/and, it stays.)"""
    for i, l in enumerate(lines):
        m = _LSL26.match(l)
        if not m:
            continue
        dst, src = m.groups()
        mov = None
        for k in range(1, 6):
            if i - k < 0:
                break
            t = lines[i - k]
            if re.match(rf"\s*movs?\s+{src},\s*#1\s*$", t):
                mov = i - k
                break
            if re.search(rf"\b{src}\b", t) and not t.strip().startswith(("str", "cmp")):
                break
        if mov is None:
            continue
        address = False
        for k in range(1, 8):
            if i + k >= len(lines):
                break
            t = lines[i + k].strip()
            if re.search(rf"\[{dst}\b", t):
                address = True
                break
            if re.match(rf"(add|sub)s?\s+\w+,\s*{dst}\b|(add|sub)s?\s+{dst},", t):
                address = True
                break
            if re.search(rf"\b{dst}\b", t):
                break
        if address:
            lines[mov] = re.sub(r"#1\s*$", "#3", lines[mov])
