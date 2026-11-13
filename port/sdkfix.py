#!/usr/bin/env python3
"""Patched copies of the few NitroSDK / NitroSystem sources GCC can't take as they are.

Most of the SDK builds unchanged. These files mix C with Metrowerks inline-assembly functions or
Metrowerks-only C: the assembly functions are cut out (port/runtime/sdk_asm.c has them in C) and
the rest is fixed by small text replacements. The copies go in build-3ds/sdkfix/<same path>;
port/compile_game.py compiles a copy instead of the original when there is one.

    python3 port/sdkfix.py
"""
import os
import re

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SUB = os.path.join(ROOT, "pokeheartgold", "subprojects")
SDK = os.path.join(SUB, "NitroSDK-4.2.30001", "libraries")
NNS = os.path.join(SUB, "NitroSystem-071126.1", "libraries")
OUT = os.path.join(ROOT, "build-3ds", "sdkfix")
PHG = os.path.join(ROOT, "pokeheartgold")
INC_OUT = os.path.join(ROOT, "port", "include")

# 3D commands: every assignment to a DS 3D register (reg_G3_..., reg_G3X_...) becomes a call into the
# port's geometry engine (port/runtime/gx3d.c), which sees each write in order, as the hardware
# would. (A plain store into shadow memory would be lost.)
G3_ASSIGN = (r"\breg_(G3X?_\w+)\s*=(?!=)\s*([^;]+);", r"port_g3_write((u32)&reg_\1, (u32)(\2));")
G3_SOURCES = [
    f"{SDK}/gx/src/g3imm.c", f"{SDK}/gx/src/g3x.c", f"{SDK}/gx/src/gx_vramcnt.c",
    f"{NNS}/g3d/src/gecom.c", f"{NNS}/g3d/src/util.c", f"{NNS}/g3d/src/sbc.c",
    f"{NNS}/g2d/src/g2d_OamSoftwareSpriteDraw.c",
    f"{PHG}/src/field/fog.c",
]
G3_HEADERS = ["g3imm.h", "g3x.h", "g3.h"]

GXASM = ('#include "../include/gxasm.h"', '#include <port_gxasm.h>')
MUL64 = ("inline fx32 mul64", "static inline fx32 mul64")

FIXES = {
    f"{SDK}/fx/src/fx_mtx22.c": dict(strip_asm=True, sub=[MUL64]),
    f"{SDK}/fx/src/fx_mtx33.c": dict(strip_asm=True, sub=[MUL64]),
    f"{SDK}/fx/src/fx_mtx43.c": dict(strip_asm=True),
    f"{SDK}/fx/src/fx_mtx44.c": dict(strip_asm=True, sub=[MUL64]),
    f"{SDK}/gx/src/g3b.c": dict(sub=[GXASM]),
    f"{SDK}/gx/src/g3c.c": dict(sub=[GXASM]),
    f"{SDK}/gx/src/g3imm.c": dict(sub=[GXASM]),
    f"{SDK}/gx/src/g3x.c": dict(strip_asm=True, sub=[
        ('#include "gxasm.h"', '#include <port_gxasm.h>'),
        ("static asm void GXi_NopClearFifo128_(register void *pDest);",
         "static void GXi_NopClearFifo128_(void *pDest) { (void)pDest; for (int i = 0; i < 32; i++) port_gx_fifo_write(0); }"),
    ]),
    # The game asks for volume 128 (field_warp_tasks.c, field_control.c: "back to full"), one past
    # this 128-entry table. On the DS the next word in the ARM9 binary is 0 (0 dB: full volume);
    # in the port it was other data (+229 dB: every channel at maximum, clipping). So: one more 0.
    f"{SDK}/snd/src/snd_util.c": dict(sub=[
        ("const s16 SNDi_DecibelTable[SND_DECIBEL_TABLE_SIZE] = {",
         'const s16 port_SNDi_DecibelTable[SND_DECIBEL_TABLE_SIZE + 1] __asm__("SNDi_DecibelTable") = {'),
        ("-5, -4, -3, -3, -2, -1, -1, 0\n};", "-5, -4, -3, -3, -2, -1, -1, 0,\n    0\n};"),
    ]),
    f"{SDK}/math/src/math.c": dict(sub=[("defined(SDK_ARM9) && (defined(SDK_CW) || defined(__MWERKS__))", "0")]),
    f"{SDK}/math/src/checksum.c": dict(regex=[
        # Metrowerks lets a cast be incremented: *((T *)p)++ reads a T and moves p on by one.
        (r"\*\(\((u8|u16|u32) \*\)(\w+)\)\+\+", r"(*(((\2) = (\1 *)(\2) + 1), (\1 *)(\2) - 1))"),
    ]),
}


def strip_asm_functions(text):
    """Removes `asm <type> name(...) { ... }` definitions (Metrowerks inline-assembly functions)."""
    out = []
    i = 0
    pat = re.compile(r"^[ \t]*(?:static[ \t]+)?asm[ \t]+[^\n;{]*\([^;{]*\)\s*\{", re.M)
    while True:
        m = pat.search(text, i)
        if not m:
            out.append(text[i:])
            break
        out.append(text[i:m.start()])
        depth = 0
        j = m.end() - 1
        while j < len(text):
            if text[j] == "{":
                depth += 1
            elif text[j] == "}":
                depth -= 1
                if depth == 0:
                    break
            j += 1
        out.append(f"/* (asm function removed for the 3DS port: {m.group(0).strip()[:-1].strip()}) */\n")
        i = j + 1
    return "".join(out)


def main():
    n = 0
    for src in G3_SOURCES:  # (merged with any other fix for the same file)
        fx = FIXES.setdefault(src, {})
        fx.setdefault("regex", []).append(G3_ASSIGN)
    for h in G3_HEADERS:
        src = os.path.join(SUB, "NitroSDK-4.2.30001", "include", "nitro", "gx", h)
        text = open(src, encoding="latin-1").read()
        text, k = re.subn(G3_ASSIGN[0], G3_ASSIGN[1], text)
        text = "void port_g3_write(unsigned long addr, unsigned long value);\n" + text
        dst = os.path.join(INC_OUT, "nitro", "gx", h)
        os.makedirs(os.path.dirname(dst), exist_ok=True)
        with open(dst, "w", encoding="latin-1") as f:
            f.write(f"/* NitroSDK's nitro/gx/{h}, 3D register writes routed to the port (port/sdkfix.py) */\n" + text)
        print(f"sdkfix: {h}: {k} register writes")
    for src, fx in FIXES.items():
        text = open(src, encoding="latin-1").read()
        if fx.get("strip_asm"):
            text = strip_asm_functions(text)
        for a, b in fx.get("sub", []):
            if a not in text:
                raise SystemExit(f"sdkfix: '{a}' not found in {src}")
            text = text.replace(a, b)
        for a, b in fx.get("regex", []):
            text, k = re.subn(a, b, text)
            if not k:
                raise SystemExit(f"sdkfix: /{a}/ not found in {src}")
        dst = os.path.join(OUT, os.path.relpath(src, SUB) if src.startswith(SUB) else os.path.relpath(src, PHG))
        if src in G3_SOURCES:
            text = "void port_g3_write(unsigned long addr, unsigned long value);\n" + text
        os.makedirs(os.path.dirname(dst), exist_ok=True)
        with open(dst, "w", encoding="latin-1") as f:
            f.write(f'#line 1 "{src}"\n' + text)
        n += 1
    print(f"sdkfix: {n} files in {os.path.relpath(OUT, ROOT)}")


if __name__ == "__main__":
    main()
