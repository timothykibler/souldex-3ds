#!/usr/bin/env python3
"""Stand-in for the Metrowerks tools, so pokeheartgold's meson build can configure and run its
data generators. Data sources (.s / .c from jsonproc templates) are compiled for real with
devkitARM GCC; game code and SDK libraries are not built here (the port builds them with GCC
itself), so for those it only creates the output file."""
import os, subprocess, sys
args = sys.argv[1:]
tool = os.path.basename(sys.argv[0])
if any(a in ("--version", "-version", "-v") for a in args):
    print("Metrowerks C/C++ Compiler for Embedded ARM.\nVersion 4.0 build 1018")
    sys.exit(0)
out = None
for i, a in enumerate(args):
    if a == "-o" and i + 1 < len(args):
        out = args[i + 1]
srcs = [a for a in args if a.endswith((".s", ".c")) and os.path.exists(a)]
datasrc = srcs and tool != "mwldarm" and out and (".narc.p/" in out or "/files/" in out or out.startswith("files/"))
if datasrc and out and out.endswith(".o"):
    keep = []
    skip = False
    for i, a in enumerate(args):
        if skip:
            skip = False
            continue
        if a in ("-I", "-i", "-D", "-include"):
            keep += [("-I" if a in ("-I", "-i") else a), args[i + 1]]
            skip = True
        elif a.startswith(("-I", "-D", "-U")) and not a.startswith("-Ir"):
            keep.append(a)
    base = ["arm-none-eabi-gcc", "-w", "-mthumb-interwork", "-fsigned-char", "-fno-short-enums",
            "-mcpu=arm946e-s"] + keep
    if srcs[0].endswith(".s"):
        # Metrowerks assembler syntax -> GNU as: preprocess, then fix the dialect
        pp = subprocess.run(base + ["-E", "-P", "-x", "assembler-with-cpp", srcs[0]],
                            capture_output=True, text=True)
        if pp.returncode:
            sys.stderr.write(pp.stderr)
            sys.exit(pp.returncode)
        sys.path.insert(0, os.path.dirname(os.path.realpath(__file__)))
        from mwasm2gas import convert
        tmp = out + ".gas.s"
        with open(tmp, "w") as f:
            f.write(convert(pp.stdout))
        r = subprocess.run(base + ["-c", "-x", "assembler", tmp, "-o", out])
        # (gone afterwards: beside a NARC member's object, in its .narc.p folder, the packer counted
        # it as a member too, doubling the zone_event and mmodel indices: no map's warps were its own)
        os.remove(tmp)
    else:
        r = subprocess.run(base + ["-c", "-x", "c", "-std=gnu99", srcs[0], "-o", out])
    if r.returncode == 0 and "-MD" in args:
        # (mwasm -gccdep -MD writes <obj>.d beside the object, minus its extension)
        open(os.path.splitext(out)[0] + ".d", "w").write(out + ":\n")
    sys.exit(r.returncode)
if out:
    os.makedirs(os.path.dirname(out) or ".", exist_ok=True)
    open(out, "wb").close()
sys.exit(0)
