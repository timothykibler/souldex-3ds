#!/usr/bin/env python3
"""Builds the SoulSilver 3DS port (phase 0) from pokeheartgold. Runs in the ss3ds-dev image:

    docker run --rm -v "$PWD":/w -w /w ss3ds-dev python3 port/build.py [--skip-game] [--release]

Debug (the default) or --release: only the port's runtime differs (PORT_DEBUG=1/0, port/runtime/port.h),
so switching with --skip-game is safe. A release build ignores debug.txt, sends no Wi-Fi log and starts
with the frame counter off; the log file, the watchdog and crash reporting are in both.

Needs build-gen (the decomp's generated headers: see README). Output: build-3ds/soulsilver.3dsx
"""
import glob
import os
import re
import subprocess
import sys

ROOT = "/w"
B = f"{ROOT}/build-3ds"
DKA = "/opt/devkitpro/devkitARM/bin/arm-none-eabi-"
CC = DKA + "gcc"
sys.path.insert(0, f"{ROOT}/port")
import compile_game as cg  # noqa: E402

RELEASE = "--release" in sys.argv
VARIANT = "release" if RELEASE else "debug"

TARGETS = [("main.nef", "c"), ("main.nef", "s"),
           ("subprojects/NitroSDK-4.2.30001/libraries", "c"),
           ("subprojects/NitroSystem-071126.1/libraries", "c"),
           ("lib/spl", "c")]

# SDK libraries (or single objects) the port replaces or leaves out.
EXCLUDE_LIBS = {"libmi.a", "libcard.a", "libpxi.a", "libinit.a", "libstubscw.a", "libstubsisd.a",
                "libutest.a", "libenv.a", "libmb.a", "libwm.a", "libwvr.a", "libwbt.a", "libwfs.a",
                "libspi.a", "libctrdg.a", "libsyscall.a", "libnnsmcs.a", "libext.a", "libprc.a"}
EXCLUDE_OBJS = {"src_os_tick.c.o", "src_fs_overlay.c.o", "src_os_timer.c.o", "src_os_valarm.c.o",
                "src_gxasm.c.o", "src_internal.c.o", "src_external.c.o", "src_g2di_SplitChar.c.o", "src_sndarc_stream.c.o"}


def run(cmd, **kw):
    r = subprocess.run(cmd, **kw)
    if r.returncode:
        sys.exit(r.returncode)
    return r


# The ARM7's sound driver (NitroSDK's libsnd_arm7: sequencer, voices, channel programming), run
# by the port on another core (runtime/snd.c). Compiled for SDK_ARM7 with its I/O registers
# pointed at an array (port/include_arm7), linked into one object whose symbols all get an a7_
# prefix (the ARM9's sound library has the same names), and whose ARM7 OS needs (a message
# queue, alarms, interrupt masking, PXI, power control) go to port_a7_* (snd.c).
A7_EXTERN = {
    "OS_InitMessageQueue": "port_a7_InitMessageQueue", "OS_SendMessage": "port_a7_SendMessage",
    "OS_ReceiveMessage": "port_a7_ReceiveMessage", "OS_DisableInterrupts": "port_a7_DisableInterrupts",
    "OS_RestoreInterrupts": "port_a7_RestoreInterrupts", "OS_CreateAlarm": "port_a7_CreateAlarm",
    "OS_SetAlarm": "port_a7_SetAlarm", "OS_SetPeriodicAlarm": "port_a7_SetPeriodicAlarm",
    "OS_CancelAlarm": "port_a7_CancelAlarm", "OS_GetTick": "port_a7_GetTick", "PXI_SendWordByFifo": "port_a7_PXI_SendWordByFifo",
    "PXI_SetFifoRecvCallback": "port_a7_PXI_SetFifoRecvCallback", "PMi_SetControl": "port_a7_PMi_SetControl",
    "PMi_ResetControl": "port_a7_PMi_ResetControl", "SDK_ASSERT": "port_a7_Assert",
    "SDK_MINMAX_ASSERT": "port_a7_MinMaxAssert", "SVC_ResetSoundBias": "port_a7_ResetSoundBias",
    "SVC_SetSoundBias": "port_a7_SetSoundBias",
}


def compile_arm7_sound(flags):
    sdk = f"{ROOT}/pokeheartgold/subprojects/NitroSDK-4.2.30001/libraries/snd/src"
    # (the channel layer through the port's wrapper: port/arm7/snd_channel_port.c)
    srcs = [s for s in sorted(glob.glob(f"{sdk}/ARM7/*.c")) if not s.endswith("snd_channel.c")] + \
           [f"{ROOT}/port/arm7/snd_channel_port.c", f"{ROOT}/port/arm7/snd_diag.c"] + [f"{sdk}/{f}" for f in
            ("snd_command.c", "snd_work.c", "snd_util.c", "snd_bank.c", "snd_alarm.c", "snd_data.c")]
    out = f"{B}/obj/a7"
    os.makedirs(out, exist_ok=True)
    # (the register override first: ahead of the SDK's own ARM7 memory map)
    flags7 = [f"-I{ROOT}/port/include_arm7"] + [f for f in flags if f != "-DSDK_ARM9"] + [
              "-USDK_ARM9", "-DSDK_ARM7", "-DSDK_NOINIT", "-fno-common"]
    objs = []
    for s in srcs:
        o = f"{out}/{os.path.basename(s)}.o"
        extra = ["-USDK_TS"] if s.endswith("snd_util.c") else []  # (the SDK's own pitch/volume tables, not the ARM7 BIOS's)
        if s.endswith("snd_channel_port.c"):
            extra = [f"-I{sdk}/ARM7"]
        run([CC, "-c"] + cg.CFLAGS + ["-w"] + flags7 + extra + [s, "-o", o])
        objs.append(o)
    ld = CC.replace("gcc", "ld")
    nm = CC.replace("gcc", "nm")
    merged = f"{out}/../a7link/snd_arm7_r.o"
    os.makedirs(f"{B}/obj/a7link", exist_ok=True)
    run([ld, "-r", "-o", merged] + objs)
    defined = subprocess.run([nm, "-g", "--defined-only", merged], capture_output=True, text=True).stdout.split()
    undefined = subprocess.run([nm, "-u", merged], capture_output=True, text=True).stdout.split()
    names = {defined[i + 2] for i in range(0, len(defined) - 2, 3)}
    with open(f"{B}/gen/snd_arm7.syms", "w") as f:
        for n in sorted(names):
            f.write(f"{n} a7_{n}\n")
        for n in undefined:
            if n == "U":
                continue
            if n in A7_EXTERN:
                f.write(f"{n} {A7_EXTERN[n]}\n")
            elif n.startswith(("SND_", "SNDi_")):  # (ARM7 sound functions not built here: snd.c)
                f.write(f"{n} a7_{n}\n")
    final = f"{B}/obj/a7link/snd_arm7.o"  # (not in obj/ itself: the game objects are globbed from there)
    run([CC.replace("gcc", "objcopy"), f"--redefine-syms={B}/gen/snd_arm7.syms", merged, final])
    return final


def compile_runtime():
    cc = __import__("json").load(open(f"{ROOT}/build-gen/compile_commands.json"))
    flags = cg.translate([c for c in cc if c["output"] == "main.nef.p/src_main.c.o"][0]["command"])
    out = f"{B}/obj/port"
    os.makedirs(out, exist_ok=True)
    # the GPU compositor's vertex shader, assembled and embedded
    for sh in ("quad", "poly", "polyid", "edge"):
        run(["/opt/devkitpro/tools/bin/picasso", "-o", f"{B}/gen/{sh}.shbin", f"{ROOT}/port/shaders/{sh}.v.pica"])
        with open(f"{B}/gen/{sh}_shbin.s", "w") as f:
            run(["/opt/devkitpro/tools/bin/bin2s", f"{B}/gen/{sh}.shbin"], stdout=f)
    # calls into memory several overlays share, dispatched to the one loaded (port/ovdispatch.py)
    global OVDISPATCH_WRAPS
    sys.path.insert(0, f"{ROOT}/port")
    import ovdispatch
    OVDISPATCH_WRAPS = ovdispatch.generate(f"{B}/obj", f"{B}/gen/ovdispatch.c")
    srcs = glob.glob(f"{ROOT}/port/runtime/*.c") + glob.glob(f"{ROOT}/port/runtime/*.S") + [f"{B}/gen/overlays.c", f"{B}/gen/ovdispatch.c", f"{B}/gen/quad_shbin.s", f"{B}/gen/poly_shbin.s", f"{B}/gen/edge_shbin.s", f"{B}/gen/polyid_shbin.s"]
    # the build's ID (commit + time), logged at start-up and kept with a copy of the ELF
    # (port/package.sh: build-3ds/elves/), so a crash dump can be matched to its exact program
    head = open(f"{ROOT}/.git/HEAD").read().strip()
    sha = open(f"{ROOT}/.git/" + head[5:]).read().strip() if head.startswith("ref: ") and os.path.exists(f"{ROOT}/.git/" + head[5:]) else head
    import time
    build_id = f"{sha[:7]}-{time.strftime('%Y%m%d-%H%M%S')}-{VARIANT}"
    os.makedirs(f"{B}/gen", exist_ok=True)
    open(f"{B}/gen/build_id.txt", "w").write(build_id + "\n")
    objs = []
    for s in srcs:
        o = f"{out}/{os.path.basename(s)}.o"
        if s.endswith((".S", ".s")):
            cmd = [CC, "-c", "-march=armv6k", "-mfloat-abi=hard", s, "-o", o]
        else:
            uses_ctru = "<3ds.h>" in open(s).read()
            cmd = [CC, "-c"] + cg.CFLAGS + ["-w", f"-I{ROOT}/port/runtime", f"-DPORT_DEBUG={0 if RELEASE else 1}"]
            cmd += (["-I/opt/devkitpro/libctru/include", "-D__3DS__", f"-I{ROOT}/port/include"] if uses_ctru else flags)
            if s.endswith("main.c"):
                cmd += [f'-DPORT_BUILD_ID="{build_id}"']
            cmd += [s, "-o", o]
        run(cmd)
        objs.append(o)
    objs.append(compile_arm7_sound(flags))
    return objs


def archives():
    libs = []
    for d in sorted(glob.glob(f"{B}/obj/lib/*")):
        name = os.path.basename(d)
        if name in EXCLUDE_LIBS:
            continue
        objs = [o for o in glob.glob(f"{d}/*.o") if os.path.basename(o) not in EXCLUDE_OBJS]
        a = f"{B}/lib/{name}"
        os.makedirs(f"{B}/lib", exist_ok=True)
        if os.path.exists(a):
            os.remove(a)
        run([DKA + "ar", "rcs", a] + objs)
        libs.append(a)
    return libs


OVDISPATCH_WRAPS = []


def link(objs, libs, stubs):
    elf = f"{B}/soulsilver.elf"
    cmd = [CC, "-specs=" + f"{B}/port.specs", "-march=armv6k", "-mtune=mpcore", "-mfloat-abi=hard",
           "-mtp=soft", "-L/opt/devkitpro/libctru/lib", "-Wl,-Map," + f"{B}/soulsilver.map", "-Wl,--wrap=GF_AssertFail,--wrap=G3X_GetClipMtx,--wrap=G3X_GetVectorMtx,--wrap=Handle3DBufferSwapRequest,--wrap=CallFieldTask_ContinueGame_Normal,--wrap=SysTask_CreateOnVBlankQueue",
           ] + [f"-Wl,--wrap={w}" for w in OVDISPATCH_WRAPS] + [
           "-o", elf] + objs + ([stubs] if stubs else []) + ["-Wl,--start-group"] + libs + \
          ["-lcitro3d", "-lctru", "-lm", "-Wl,--end-group"]
    return subprocess.run(cmd, capture_output=True, text=True), elf


def make_stubs(undefined):
    """Weak stand-ins for whatever is still missing, so the program links and says, when one is
    reached, what it was. Data symbols get a zeroed block."""
    path = f"{B}/gen/stubs_auto.c"
    lines = ["// Generated by port/build.py: everything still missing at link time.",
             "void port_stub_hit(const char *name);", ""]
    for name in sorted(undefined):
        if re.match(r"^_+[0-9A-Fa-f]{6,}$|^__global_destructor_chain$|^_version_|^OSi_IrqThreadQueue$", name):
            lines.append(f"__attribute__((weak, aligned(8))) char {name}[256];")
        else:
            lines.append(f'__attribute__((weak)) int {name}(void) {{ port_stub_hit("{name}"); return 0; }}')
    with open(path, "w") as f:
        f.write("\n".join(lines) + "\n")
    o = f"{B}/obj/port/stubs_auto.o"
    run([CC, "-c", "-march=armv6k", "-mfloat-abi=hard", "-marm", "-w", path, "-o", o])
    return o


def main():
    print(f"build: {VARIANT}")
    if "--skip-game" not in sys.argv:
        run(["python3", f"{ROOT}/port/sdkfix.py"])
        for target, kind in TARGETS:
            run(["python3", f"{ROOT}/port/compile_game.py", "--target", target, "--only", kind])
    os.makedirs(f"{B}/gen", exist_ok=True)
    run(["python3", f"{ROOT}/port/gen_ld.py", f"{ROOT}/pokeheartgold/main.lsf",
         "/opt/devkitpro/devkitARM/arm-none-eabi/lib/3dsx.ld", f"{B}/port.ld", f"{B}/gen/overlays.c"])
    specs = open("/opt/devkitpro/devkitARM/arm-none-eabi/lib/3dsx.specs").read()
    specs = specs.replace("-T 3dsx.ld%s", f"-T {B}/port.ld")
    open(f"{B}/port.specs", "w").write(specs)

    objs = sorted(glob.glob(f"{B}/obj/*.o")) + compile_runtime()
    libs = archives()
    stubs = None
    for attempt in range(2):
        r, elf = link(objs, libs, stubs)
        undefined = set(re.findall(r"undefined reference to `([^']+)'", r.stderr))
        if r.returncode == 0:
            break
        other = [l for l in r.stderr.splitlines() if "undefined reference" not in l and "error" in l.lower()]
        if other and not undefined:
            open(f"{B}/link.log", "w").write(r.stdout + r.stderr)
            print("\n".join(l for l in r.stderr.splitlines() if "warning:" not in l and "NOTE:" not in l)[-6000:])
            sys.exit(1)
        print(f"link: {len(undefined)} symbols missing -> stubs (build-3ds/gen/stubs_auto.c)")
        with open(f"{B}/missing_at_link.txt", "w") as f:
            f.write("\n".join(sorted(undefined)) + "\n")
        stubs = make_stubs(undefined)
    if r.returncode:  # (the errors, not the flood of enum-size warnings; all of it in link.log)
        open(f"{B}/link.log", "w").write(r.stdout + r.stderr)
        print("\n".join(l for l in r.stderr.splitlines() if "warning:" not in l and "NOTE:" not in l)[-6000:])
        sys.exit(1)
    run(["python3", f"{ROOT}/port/cheatmap.py", elf])  # (the cheat engine's address table, into the ELF)
    run([DKA.replace("arm-none-eabi-", "") + "../../tools/bin/smdhtool", "--create", "SoulSilver 3DS",
         "Phase 0 port (pokeheartgold)", "personal build", f"{ROOT}/port/icon.png", f"{B}/soulsilver.smdh"],
        stdout=subprocess.DEVNULL)
    # (the .3dsx is optional, for the Homebrew Launcher: 3dsxtool refuses this ELF, "Unaligned
    # relocation!", and the CIA, made from the ELF by port/package.sh, doesn't need it)
    if subprocess.run(["/opt/devkitpro/tools/bin/3dsxtool", elf, f"{B}/soulsilver.3dsx", f"--smdh={B}/soulsilver.smdh"]).returncode:
        print("no .3dsx (3dsxtool refused the ELF); the ELF is built: port/package.sh makes the CIA")
    else:
        print("built build-3ds/soulsilver.3dsx")


if __name__ == "__main__":
    main()
