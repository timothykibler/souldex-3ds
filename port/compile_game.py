#!/usr/bin/env python3
"""Compiles pokeheartgold's game code (SoulSilver) with devkitARM GCC for the 3DS's ARM11.

Reads the decomp's own compile commands (build-gen, made with tools/fakemw), keeps its include
paths and defines, and swaps the Metrowerks flags for GCC ones. Objects go in build-3ds/obj.
Runs inside the ss3ds-dev Docker image, from /w.
    python3 port/compile_game.py [--only c|s] [--jobs N] [pattern]
"""
import json, os, re, subprocess, sys
from concurrent.futures import ThreadPoolExecutor

ROOT = "/w"
GEN = f"{ROOT}/build-gen"
OUT = f"{ROOT}/build-3ds/obj"
CC = "/opt/devkitpro/devkitARM/bin/arm-none-eabi-gcc"

ARCH = ["-march=armv6k", "-mtune=mpcore", "-mfloat-abi=hard", "-mtp=soft", "-marm"]
CFLAGS = ARCH + ["-std=gnu99", "-O2", "-fsigned-char", "-fno-short-enums", "-fno-strict-aliasing",
                 "-fwrapv", "-fno-delete-null-pointer-checks", "-ffunction-sections", "-fdata-sections",
                 "-fcommon", "-w", "-fno-builtin", "-fpermissive",
                 "-Wno-implicit-function-declaration", "-Wno-int-conversion",
                 "-Wno-incompatible-pointer-types", "-D__3DS_PORT__", "-include", f"{ROOT}/port/include/port_prelude.h"]


def translate(cmd):
    toks = cmd.split()
    keep = [f"-I{ROOT}/port/include"]
    for t in toks:
        if t.startswith("-I"):
            p = t[2:]
            keep.append("-I" + (p if p.startswith("/") else os.path.normpath(os.path.join(GEN, p))))
        elif t.startswith("-D") and not t.startswith("-D_MSL") and t != "-DSDK_CW_FORCE_EXPORT_SUPPORT":
            keep.append(t)
    return keep


ASFLAGS = ["-march=armv6k", "-mfloat-abi=hard", "-x", "assembler-with-cpp", "-w"]


def assemble(src, obj, incs):
    """The decomp's remaining assembly (Metrowerks syntax) through mwasm2gas, then GNU as."""
    sys.path.insert(0, f"{ROOT}/tools/fakemw")
    from mwasm2gas import convert, inline_includes
    dirs = [i[2:] for i in incs if i.startswith("-I")]
    flat = obj + ".flat.s"
    with open(flat, "w") as f:
        f.write(inline_includes(src, dirs))
    defs = ["-DSDK_ASM", "-DPM_ASM", "-DSDK_ARM9", "-DSDK_CODE_ARM", "-DSDK_FINALROM", "-DSOULSILVER"]
    pp = subprocess.run([CC, "-E", "-P", "-x", "assembler-with-cpp"] + [f"-I{os.path.dirname(src)}"] + incs + defs + [flat],
                        capture_output=True, text=True)
    if pp.returncode:
        return pp.returncode, pp.stderr
    tmp = obj + ".gas.s"
    with open(tmp, "w") as f:
        f.write(convert(pp.stdout, move_ds_addresses=True))
    asinc = [i.replace("-I", "-Wa,-I", 1) for i in incs if i.startswith("-I")]
    r = subprocess.run([CC, "-c", "-march=armv6k", "-mfloat-abi=hard", "-x", "assembler"] + asinc +
                       [f"-Wa,-I{os.path.dirname(src)}", tmp, "-o", obj], capture_output=True, text=True)
    return r.returncode, r.stderr


def main():
    args = sys.argv[1:]
    only = None
    target = "main.nef"
    jobs = os.cpu_count() or 4
    pat = None
    while args:
        a = args.pop(0)
        if a == "--only":
            only = args.pop(0)
        elif a == "--target":
            target = args.pop(0)
        elif a == "--jobs":
            jobs = int(args.pop(0))
        else:
            pat = a
    cc = json.load(open(f"{GEN}/compile_commands.json"))
    ext = ".s" if only == "s" else ".c"
    units = [c for c in cc if c["output"].startswith(target) and ".p/" in c["output"] and
             c["file"].endswith(ext) and "_arm7" not in c["output"] and "arm7" not in c["file"]]
    if pat:
        units = [u for u in units if re.search(pat, u["file"])]
    os.makedirs(OUT, exist_ok=True)

    def one(u):
        src = os.path.normpath(os.path.join(GEN, u["file"]))
        tname = u["output"].split(".p/")[0]
        odir = OUT if tname == "main.nef" else os.path.join(OUT, "lib", os.path.basename(tname))
        os.makedirs(odir, exist_ok=True)
        obj = os.path.join(odir, os.path.basename(u["output"]))
        if ext == ".c":
            # (a patched copy from port/sdkfix.py, when there is one)
            sub = os.path.join(ROOT, "pokeheartgold", "subprojects")
            rel = os.path.relpath(src, sub) if src.startswith(sub) else os.path.relpath(src, os.path.join(ROOT, "pokeheartgold"))
            fixed = os.path.join(ROOT, "build-3ds", "sdkfix", rel)
            extra = []
            if os.path.exists(fixed):
                extra = [f"-I{os.path.dirname(src)}"]
                src = fixed
            r = subprocess.run([CC, "-c"] + CFLAGS + translate(u["command"]) + extra + [src, "-o", obj],
                               capture_output=True, text=True)
            return src, r.returncode, r.stderr
        return src, *assemble(src, obj, translate(u["command"]))

    bad = 0
    tag = target.replace("/", "_").replace("subprojects_", "")
    with open(f"{ROOT}/build-3ds/compile_{tag}_{ext[1:]}.log", "w") as log:
        with ThreadPoolExecutor(jobs) as ex:
            for src, rc, err in ex.map(one, units):
                if rc:
                    bad += 1
                    log.write(f"### {src}\n{err}\n")
    print(f"{target}: {len(units) - bad}/{len(units)} {ext} files built (build-3ds/compile_{tag}_{ext[1:]}.log)")


if __name__ == "__main__":
    main()
