// The 3DS program: sets up the DS address space and the player's ROM, then runs the game's own
// NitroMain (pokeheartgold's src/main.c), which never returns.
#include <3ds.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include "port_ds.h"
#include "port.h"

#define PORT_DIR "sdmc:/3ds/soulsilver-3ds"

static FILE *gLog;
uint32_t port_trace3d;

int port_log_screen = 1;  // (on the bottom screen until the GPU has it: then the file only)
void port_log(const char *fmt, ...) {
    va_list ap;
    if (port_log_screen) {
        va_start(ap, fmt);
        vprintf(fmt, ap);
        va_end(ap);
    }
    char buf[512];
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (n > (int)sizeof(buf) - 1) n = sizeof(buf) - 1;
    if (gLog) {
        fputs(buf, gLog);
        fflush(gLog);
    }
    void port_netlog_send(const char *s, int n);
    port_netlog_send(buf, n);  // (to a computer over Wi-Fi, if netlog.txt names one: netlog.c)
}

void port_fatal(const char *fmt, ...) {
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    consoleInit(GFX_BOTTOM, NULL);  // (the log on the bottom screen from here on)
    port_log("\nFATAL: %s\n", buf);
    port_log("Press START to exit.\n");
    while (aptMainLoop()) {
        hidScanInput();
        if (hidKeysDown() & KEY_START) break;
        gspWaitForVBlank();
    }
    gfxExit();
    exit(1);
}

// The ARM9 binary's startup work the port does instead of crt0: the ROM header copy and the
// boot information the SDK reads from the system area at the top of main memory.
static void setup_system_area(void) {
    // HW_ROM_HEADER_BUF (0x027FFE00) and HW_CARD_ROM_HEADER (0x027FFA80): the cartridge header
    u8 header[0x160];
    port_rom_read(0, header, sizeof(header));
    memcpy((void *)(0x027FFE00 + PORT_DS_DELTA), header, sizeof(header));
    memcpy((void *)(0x027FFA80 + PORT_DS_DELTA), header, sizeof(header));
    // No buttons held yet (they're active low: an all-zero register is every button down, which
    // includes the START+SELECT+L+R soft reset).
    *(volatile u16 *)(0x04000130 + PORT_DS_DELTA) = 0x03FF;
    *(volatile u16 *)(0x027FFFA8 + PORT_DS_DELTA) = 0x0C00;  // (HW_BUTTON_XY_BUF: X, Y up, hinge open)
}

extern void NitroMain(void);
extern void port_run_main_sinit(void);

// debug.txt "warp FRAME MAP X Z" (any number of them, in frame order): from that frame, once the
// field runs no task (a map's entry script, a fade), the game's own warp (as a script's) to there,
// facing north, for testing a place without walking to it. (The field system kept from Continue.)
// "warpmark": each warp also prints "maptour: map MAP" to the debugger, so Azahar's log marks where
// each map starts among its unmapped accesses (tools/maptour.py).
typedef struct { int frame, map, x, z; } Warp;
static Warp sWarps[640];
static int sNumWarps, sNextWarp, sWarpMark;
static void *sFieldSystem;
void *__real_CallFieldTask_ContinueGame_Normal(void *fieldSystem);
void *__wrap_CallFieldTask_ContinueGame_Normal(void *fieldSystem) {
    sFieldSystem = fieldSystem;
    return __real_CallFieldTask_ContinueGame_Normal(fieldSystem);
}
void *sub_020538C0(void *fieldSystem, u32 mapId, int warpId, int x, int y, int direction);
int FieldSystem_TaskIsRunning(void *fieldSystem);
void port_debug_warp_frame(void) {
    if (sNextWarp >= sNumWarps || !sFieldSystem) return;
    const Warp *w = &sWarps[sNextWarp];
    if (port_frame_count < (u32)w->frame || FieldSystem_TaskIsRunning(sFieldSystem)) return;
    sNextWarp++;
    port_log("warp: map %d (%d, %d) at frame %u%s\n", w->map, w->x, w->z, (unsigned)port_frame_count, sWarpMark ? ", marked" : "");
    if (sWarpMark) {
        char mark[32];
        int len = snprintf(mark, sizeof(mark), "maptour: map %d", w->map);
        svcOutputDebugString(mark, len);
    }
    sub_020538C0(sFieldSystem, (u32)w->map, -1, w->x, w->z, 0);
}

int main(void) {
    gfxInitDefault();
    osSetSpeedupEnable(true);  // (New 3DS: 804 MHz and the L2 cache)
    mkdir("sdmc:/3ds", 0777);
    mkdir(PORT_DIR, 0777);
    remove(PORT_DIR "/log.prev.txt");
    rename(PORT_DIR "/log.txt", PORT_DIR "/log.prev.txt");  // (the previous run's, crash or not)
    gLog = fopen(PORT_DIR "/log.txt", "w");
#ifndef PORT_BUILD_ID
#define PORT_BUILD_ID "unknown"
#endif
    { void port_netlog_init(const char *build); port_netlog_init(PORT_BUILD_ID); }
    {  // (the CPU's clock, measured: 10 M turns of a two-instruction loop, about 2 cycles each on the ARM11)
        u64 t0 = svcGetSystemTick();
        u32 n = 10000000;
        __asm__ volatile("1: subs %0, %0, #1\n bne 1b" : "+r"(n) : : "cc");
        u64 dt = svcGetSystemTick() - t0;
        extern unsigned port_cpu_mhz;
        port_cpu_mhz = (unsigned)(20000000ull * SYSCLOCK_ARM11 / dt / 1000000);
        bool n3ds = false;
        APT_CheckNew3DS(&n3ds);
        port_log("cpu: about %u MHz (New 3DS: %s)\n", port_cpu_mhz, n3ds ? "yes" : "no");
    }
    port_log("SoulSilver 3DS, build %s (%s)\n", PORT_BUILD_ID, PORT_DEBUG ? "debug" : "release");  // (build-3ds/elves/<this>.elf is its program: for crash dumps)
    extern bool port_gpu2d_init(void);
    if (!port_gpu2d_init()) { consoleInit(GFX_BOTTOM, NULL); port_fatal("could not start the GPU"); }
    port_log_screen = 0;  // (the console would draw over the GPU's bottom screen, in the wrong format: static)
    { void port_watchdog_start(void); port_watchdog_start(); }

    if (!port_ds_mem_init()) port_fatal("could not set up the DS memory window");
    if (!port_rom_open()) port_fatal("no SoulSilver ROM: put it at " PORT_DIR "/soulsilver.nds");
    setup_system_area();
    port_overlays_init();
    // debug.txt on the SD card: "dump N" saves both screens every N frames (shot_*.bmp)
    // (release build: ignored, so a leftover debug.txt can't script input, warp or dump)
    FILE *dbg = PORT_DEBUG ? fopen(PORT_DIR "/debug.txt", "r") : NULL;
    if (dbg) {
        extern uint32_t port_dump_every;
        char line[64];
        while (fgets(line, sizeof(line), dbg)) {
            unsigned n, m, per, q;
            char key[16];
            if (sscanf(line, "dump %u", &n) == 1) port_dump_every = n;
            else if (!strncmp(line, "wavdump", 7)) {  // ("wavdump [F]": 30 s of sound from frame F)
                extern int port_snd_wavdump;
                port_snd_wavdump = sscanf(line, "wavdump %u", &n) == 1 ? (int)n + 1 : 1;
            }
            else if (sscanf(line, "menu %u", &n) == 1) { void port_cheats_script(uint32_t f, const char *k); const char *k = strchr(line + 5, ' '); if (k) port_cheats_script(n, k + 1); }
            else if (sscanf(line, "g3prof %u", &n) == 1) { extern int port_g3_prof; port_g3_prof = (int)n; }
            else if (!strncmp(line, "render inline", 13)) { extern int port_render_inline; port_render_inline = 1; }
            else if (!strncmp(line, "texwhole", 8)) { extern int port_tex_whole; port_tex_whole = 1; }
            else if (!strncmp(line, "stereo ", 7)) { extern float port_stereo_force; float v; if (sscanf(line + 7, "%f", &v) == 1) port_stereo_force = v; }
            else if (sscanf(line, "strmtest %u", &n) == 1) { extern int port_strmtest_frame_no; port_strmtest_frame_no = (int)n; }
            else if (!strncmp(line, "cheatlog", 8)) { extern int port_cheat_log; port_cheat_log = 1; }
            else if (sscanf(line, "edge %u", &n) == 1) { extern int port_edge_mode; port_edge_mode = (int)n; }
            else if (sscanf(line, "fog %u", &n) == 1) { extern int port_fog_mode; port_fog_mode = (int)n; }
            else if (sscanf(line, "border %u", &n) == 1) { extern int port_border; port_border = (int)n; }
            else if (!strncmp(line, "scale1", 6)) { extern int port_scale1; port_scale1 = 1; }
            else if (sscanf(line, "snap %u", &n) == 1) { extern int port_g3_snap; port_g3_snap = (int)n; }
            else if (sscanf(line, "holes %u", &n) == 1) { extern int port_hole_fill; port_hole_fill = (int)n; }
            else if (sscanf(line, "warp %u %u %u %u", &n, &m, &per, &q) == 4) { if (sNumWarps < (int)(sizeof(sWarps) / sizeof(sWarps[0]))) sWarps[sNumWarps++] = (Warp){ (int)n, (int)m, (int)per, (int)q }; }
            else if (!strncmp(line, "warpmark", 8)) sWarpMark = 1;
            else if (sscanf(line, "dlcache %u", &n) == 1) { extern int port_dl_cache; port_dl_cache = (int)n; }
            else if (sscanf(line, "g3thread %u", &n) == 1) { extern int port_g3_thread; port_g3_thread = (int)n; }
            else if (sscanf(line, "double %u", &n) == 1) { extern int port_double_speed; port_double_speed = (int)n; }
            else if (sscanf(line, "wide %u", &n) == 1) { extern int port_wide_mode; port_wide_mode = (int)n; }
            else if (sscanf(line, "trace3d %u", &n) == 1) { extern uint32_t port_trace3d; port_trace3d = n; }
            else if (sscanf(line, "mash %u %u %u %15s", &n, &m, &per, key) == 4) {
                void port_input_mash(uint32_t frame, uint32_t until, uint32_t period, const char *name);
                port_input_mash(n, m, per, key);
            }
            else if (sscanf(line, "press %u %15s", &n, key) == 2) {
                void port_input_script(uint32_t frame, const char *name);
                port_input_script(n, key);
            }
        }
        fclose(dbg);
    }
    extern void port_snd_init(void);
    port_snd_init();  // (the ARM7's sound driver: before the game sends it anything)
    extern void port_cheats_init(void);
    port_cheats_init();
    port_run_main_sinit();
    port_log("starting the game\n");
    NitroMain();
    port_fatal("NitroMain returned");
}

