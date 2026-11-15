"""Metrowerks ARM assembler syntax (as pokeheartgold writes it) to GNU as syntax."""
import os
import re

_SECTIONS = {".rodata": ".section .rodata", ".bss": ".section .bss", ".sbss": ".section .sbss",
             ".ctor": ".section .ctor,\"aw\"", ".init": ".section .init,\"ax\""}
_POOL_WORD = re.compile(r"^\s*\w+:\s*\.word\b")
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
        elif _POOL_WORD.match(line):  # (a literal pool entry: word-aligned, as ldr needs, after a rewrite made code longer)
            line = "\t.balign 4\n" + line
        lines.append(line)
    if move_ds_addresses:
        _move_io_base_immediates(lines)
        _move_mem_immediates(lines)
        _move_derived_immediates(lines)
        _shifted_io_literals(lines)
    return "\n".join(lines) + "\n"


_LSL26 = re.compile(r"\s*lsls?\s+(r\d),\s*(r\d),\s*#0x1a\b")


_MOV_IMM = re.compile(r"^(\s*movs?\s+)(r\d)(,\s*#)(0x[0-9a-fA-F]+|\d+)\s*$")
_LSL_IMM = re.compile(r"^\s*lsls?\s+(r\d),\s*(r\d),\s*#(0x[0-9a-fA-F]+|\d+)\s*$")
_MEM_FUNC = re.compile(r"^(MIi?_(Cpu|Dma)\w*|DC_\w+)$")


def _move_mem_immediates(lines):
    """Thumb code also builds the DS's palette, VRAM and OAM addresses (0x05000000-0x070007FF) as
    `mov rN, #imm` then `lsl rN, rN, #sh` (e.g. 6 << 24 = 0x06000000 for MIi_CpuClear32, clearing
    VRAM when a battle loads). Where the value is then used as an address (a load/store base,
    address arithmetic, or an argument of a memory function: MI(i)_Cpu*/Dma*, DC_*), the mov's
    immediate becomes the moved address's (+ PORT_DS_DELTA, 0x08000000): same instructions, same
    size, when it still fits; flags and other values stay."""
    for i, l in enumerate(lines):
        m = _LSL_IMM.match(l)
        if not m or m.group(1) != m.group(2):
            continue
        reg, sh = m.group(1), int(m.group(3), 0)
        mov = None
        for k in range(1, 6):
            if i - k < 0:
                break
            mm = _MOV_IMM.match(lines[i - k])
            if mm and mm.group(2) == reg:
                mov = (i - k, mm)
                break
            if re.search(rf"\b{reg}\b", lines[i - k]):
                break
        if mov is None:
            continue
        value = int(mov[1].group(4), 0) << sh
        if not 0x05000000 <= value < 0x07000800:
            continue
        moved = value + 0x08000000
        if moved & ((1 << sh) - 1) or (moved >> sh) > 255:
            continue
        address = False
        for k in range(1, 9):
            if i + k >= len(lines):
                break
            t = lines[i + k].strip()
            if re.search(rf"\[{reg}\b", t) or re.match(rf"(add|sub)s?\s+\w+,\s*{reg}\b|(add|sub)s?\s+{reg},", t):
                address = True
                break
            b = re.match(r"blx?\s+(\w+)", t)
            if b:  # (a call: r0-r3 are its arguments, and clobbered after)
                address = reg in ("r0", "r1", "r2", "r3") and bool(_MEM_FUNC.match(b.group(1)))
                break
            if re.match(rf"\w+\s+{reg}\b", t):  # (redefined)
                break
        if address:
            k, mm = mov
            lines[k] = f"{mm.group(1)}{mm.group(2)}{mm.group(3)}{hex(moved >> sh)}"


def _move_derived_immediates(lines):
    """A DS I/O, palette, VRAM or OAM address built from another register's constant: `mov rS,
    #imm` (rS also used as itself) ... `lsl rD, rS, #sh` (rD != rS), e.g. 0x20 << 21 = 0x04000000.
    The mov can't change; where rD is then used as an address (a load/store base, address maths,
    a memory function's argument), the lsl becomes `mov rD, #imm2` + `lsl rD, rD, #sh2` making
    the moved address (+ PORT_DS_DELTA). Two bytes longer: the assembler places what follows."""
    out = []
    for i, l in enumerate(lines):
        m = _LSL_IMM.match(l)
        new = None
        if m and m.group(1) != m.group(2):
            dst, src, sh = m.group(1), m.group(2), int(m.group(3), 0)
            value = None
            for k in range(1, 15):
                if i - k < 0 or re.match(r"\s*(\w+:|blx?\s|bx\s|b\s|pop\b)", lines[i - k]):
                    break  # (a label, call or return between: not this mov's value)
                mm = _MOV_IMM.match(lines[i - k])
                if mm and mm.group(2) == src:
                    value = int(mm.group(4), 0) << sh
                    break
                if re.match(rf"\s*\w+\s+{src}\b", lines[i - k]) and not re.match(rf"\s*(str|cmp|tst)", lines[i - k].strip()):
                    break  # (src redefined)
            if value is not None and 0x04000000 <= value < 0x07000800:
                address = False
                for k in range(1, 9):
                    if i + k >= len(lines):
                        break
                    t = lines[i + k].strip()
                    if re.search(rf"\[{dst}\b", t) or re.match(rf"(add|sub)s?\s+\w+,\s*{dst}\b|(add|sub)s?\s+{dst},", t):
                        address = True
                        break
                    b = re.match(r"blx?\s+(\w+)", t)
                    if b:
                        address = dst in ("r0", "r1", "r2", "r3") and bool(_MEM_FUNC.match(b.group(1)))
                        break
                    if re.match(rf"\w+\s+{dst}\b", t):
                        break
                if address:
                    moved = value + 0x08000000
                    for s2 in range(0, 32):
                        if moved % (1 << s2) == 0 and (moved >> s2) <= 255 and (moved >> s2) == moved / (1 << s2):
                            pass
                    s2 = max(k for k in range(0, 32) if moved % (1 << k) == 0)
                    imm = moved >> s2
                    if imm <= 255:
                        ind = re.match(r"^(\s*)", l).group(1)
                        new = f"{ind}mov {dst}, #{hex(imm)}\n{ind}lsl {dst}, {dst}, #{hex(s2)}"
        out.append(new if new else l)
    lines[:] = out


_LSR_IMM = re.compile(r"^(\s*)lsrs?\s+(r\d),\s*(r\d),\s*#(0x[0-9a-fA-F]+|\d+)\s*$")
_LDR_LABEL = re.compile(r"^\s*ldr\s+(r\d),\s*(\w+)\s*$")
_POOL_DEF = re.compile(r"^\s*(\w+):\s*\.(?:word|4byte)\s+0x([0-9a-fA-F]{8})\s*$")


def _shifted_io_literals(lines):
    """Code that reuses an I/O address literal as a bit constant: `ldr r2, =0x04000304` (POWCNT)
    then `lsr r0, r2, #11` = 0x8000; `ldr r0, =0x04000006` then `lsr r0, r0, #13` = 0x2000 (DISPCNT's
    window 0 bit, the trainer eye-contact effect). With the literal moved (+0x08000000) the shift
    gives other bits. `lsl rD, rS, #5` + `lsr rD, rD, #(n + 5)` gives the DS's: the delta (bit 27)
    falls off the top. Two bytes longer: the assembler places what follows."""
    pool = {}
    for l in lines:
        for part in l.split("\n"):  # (a pool entry carries its `.balign 4` in the same line)
            m = _POOL_DEF.match(part)
            if m:
                pool[m.group(1)] = int(m.group(2), 16)
    moved = [(lo + PORT_DS_DELTA, hi + PORT_DS_DELTA) for lo, hi in _DS_RANGES if lo >= 0x04000000]
    for i, l in enumerate(lines):
        m = _LSR_IMM.match(l)
        if not m:
            continue
        ind, dst, src, n = m.group(1), m.group(2), m.group(3), int(m.group(4), 0)
        j, steps = i - 1, 0
        while j >= 0 and steps < 12:
            t = lines[j]
            j, steps = j - 1, steps + 1
            lab = re.match(r"^(\w+):\s*$", t)
            if lab:  # (a branch target: go on from the one earlier branch to it, if there is one)
                br = [x for x in range(max(0, j - 12), j + 1) if re.match(rf"\s*b\w*\s+{lab.group(1)}\s*$", lines[x])]
                if br:
                    j = br[-1] - 1
                continue
            lm = _LDR_LABEL.match(t)
            if lm and lm.group(1) == src:
                v = pool.get(lm.group(2))
                if v is not None and any(lo <= v < hi for lo, hi in moved) and n + 5 < 32:
                    lines[i] = f"{ind}lsl {dst}, {src}, #5\n{ind}lsr {dst}, {dst}, #{hex(n + 5)}"
                break
            # (a call or return between: stop)
            if re.match(r"\s*(blx?\s|bx\s|pop\b)", t) or re.match(rf"\s*\w+\s+{src}\b", t) and not re.match(r"\s*(str|cmp|tst)", t):
                break


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
