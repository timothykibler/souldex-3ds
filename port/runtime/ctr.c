// Everything that calls the 3DS (libctru). The files that implement NitroSDK functions can't
// include <3ds.h> (the two define u32 and friends differently), so they come through here.
#include <3ds.h>
#include <stdlib.h>
#include <string.h>
#include "port.h"

// The game's frames paced to the screen's own refresh (59.83 Hz, the DS's too), as the DS's are to
// its VBlank. (By the system timer at 60.00 Hz before: the pictures swap on the real refresh, so
// the two drifted apart and frames were lost, 57 a second on a New 3DS.) A thread waits for each
// top-screen VBlank and counts it; a frame waits for the count to pass the last frame's, catching up
// at most one late frame (a frame that ran late costs only its overrun, no hurrying after a long
// one). If the VBlank event stops coming (on a 3DS XL it did, once, during a battle's start), the
// wait times out after 20 ms and the frame goes on by the timer.
static LightEvent sVbEvent;
static volatile u32 sVbCount;
static Thread sVbThread;
uint32_t gFramesLate, gFramesTimedOut;  // (the log's: frames that found their VBlank passed, and waits that timed out)
static void vblank_main(void *arg) {
    (void)arg;
    for (;;) {
        gspWaitForEvent(GSPGPU_EVENT_VBlank0, true);
        sVbCount++;
        LightEvent_Signal(&sVbEvent);
    }
}
bool port_ctr_frame_wait(void) {
    static u32 target;
    static u64 next;  // (the timer's deadline, when the VBlank thread isn't there)
    const u64 period = SYSCLOCK_ARM11 / 60;
    if (!sVbThread) {
        static int tried;
        if (!tried) {
            tried = 1;
            LightEvent_Init(&sVbEvent, RESET_ONESHOT);
            sVbThread = threadCreate(vblank_main, NULL, 4096, 0x18, 0, true);  // (the game's core, above it: it only waits, and wakes at once)
            extern void port_log(const char *fmt, ...);
            port_log(sVbThread ? "frames: paced by the screen's VBlank\n" : "frames: paced by the timer\n");
            target = sVbCount;
        }
    }
    if (sVbThread) {
        target++;
        u32 c = sVbCount;
        if ((s32)(c - target) > 0) { gFramesLate++; target = c; }  // (late by a frame or more: no catching up)
        else if ((s32)(c - target) < 0) {
            while ((s32)(sVbCount - target) < 0) {
                if (LightEvent_WaitTimeout(&sVbEvent, 20000000LL)) { gFramesTimedOut++; break; }
            }
        }
        return aptMainLoop();
    }
    u64 now = svcGetSystemTick();
    if (next && now < next) svcSleepThread((s64)((next - now) * 1000000000ULL / SYSCLOCK_ARM11));
    now = svcGetSystemTick();
    next = (!next || now > next + period) ? now + period : next + period;
    return aptMainLoop();
}

void port_ctr_exit(int code) {
    gfxExit();
    exit(code);
}

uint64_t port_ctr_ticks(void) { return svcGetSystemTick(); }
void *port_linear_alloc(size_t bytes) { return linearAlloc(bytes); }  // (memory the GPU can read)
void port_ctr_sleep_ns(int64_t ns) { svcSleepThread(ns); }

// DS buttons from the 3DS's: REG_KEYINPUT bits (A, B, SELECT, START, right, left, up, down, R, L)
// and the ARM7's extra word (X, Y, pen down), both active low.
void port_ctr_buttons(uint16_t *keyinput, uint16_t *xy) {
    hidScanInput();
    u32 k = hidKeysHeld();
    static const u32 map[10] = { KEY_A, KEY_B, KEY_SELECT, KEY_START, KEY_RIGHT, KEY_LEFT,  // (D-pad or Circle Pad)
                                 KEY_UP, KEY_DOWN, KEY_R, KEY_L };
    uint16_t in = 0x03FF;
    for (int i = 0; i < 10; i++) {
        if (k & map[i]) in &= (uint16_t)~(1u << i);
    }
    // HW_BUTTON_XY_BUF as the SDK's ARM7 leaves it: already in PAD_Read's bit positions (X 0x400,
    // Y 0x800, active low; hinge closed 0x8000), since PAD_Read ORs it straight into KEYINPUT.
    uint16_t x = 0x0C00;  // X and Y up, hinge open
    if (k & KEY_X) x &= (uint16_t)~0x0400;
    if (k & KEY_Y) x &= (uint16_t)~0x0800;
    *keyinput = in;
    *xy = x;
}

// The stylus on the bottom screen, in DS touch-screen pixels (the 256x192 DS screen sits in the
// middle of the 320x240 3DS one). False when it's up or outside that area.
bool port_ctr_touch(uint16_t *x, uint16_t *y) {
    if (!(hidKeysHeld() & KEY_TOUCH)) return false;
    touchPosition t;
    hidTouchRead(&t);
    // (the DS screen 1:1 in the bottom screen's middle, or filling it at 1.25x)
    extern int port_bottom_scaled(void);
    int fill = port_bottom_scaled();
    int dx = fill ? (int)t.px * 4 / 5 : (int)t.px - 32, dy = fill ? (int)t.py * 4 / 5 : (int)t.py - 24;
    if (dx < 0 || dy < 0 || dx >= 256 || dy >= 192) return false;
    *x = (uint16_t)dx;
    *y = (uint16_t)dy;
    return true;
}

// An automatic stub (build-3ds/gen/stubs_auto.c) was reached: say which, once.
void port_stub_hit(const char *name) {
    static const char *seen[256];
    static int n;
    for (int i = 0; i < n; i++) if (seen[i] == name) return;
    if (n < 256) seen[n++] = name;
    port_log("stub: %s\n", name);
}

// ---------------------------------------------------------------- the screens
// Each DS screen 1:1 in the middle of its 3DS screen (256x192 in 400x240 on top, 320x240 below),
// as RGB565. The 3DS framebuffers are stored rotated: column by column, bottom to top.
static uint16_t sLut[32768];

void port_ctr_screens_init(void) {
    gfxSetScreenFormat(GFX_TOP, GSP_RGB565_OES);
    gfxSetScreenFormat(GFX_BOTTOM, GSP_RGB565_OES);
    gfxSetDoubleBuffering(GFX_TOP, true);
    gfxSetDoubleBuffering(GFX_BOTTOM, true);
    for (int c = 0; c < 32768; c++) {
        int r = c & 31, g = (c >> 5) & 31, b = (c >> 10) & 31;
        sLut[c] = (uint16_t)(r << 11 | (g << 1 | g >> 4) << 5 | b);
    }
    for (int s = 0; s < 2; s++) {  // black borders in both buffers
        for (int k = 0; k < 2; k++) {
            u16 w, h;
            u8 *fb = gfxGetFramebuffer(s ? GFX_BOTTOM : GFX_TOP, GFX_LEFT, &w, &h);
            memset(fb, 0, (size_t)w * h * 2);
            gfxSwapBuffers();
        }
    }
}

static void blit(gfxScreen_t screen, const uint16_t *src) {
    u16 w, h;  // (w = 240, h = 400 or 320: the buffer's own orientation)
    uint16_t *fb = (uint16_t *)gfxGetFramebuffer(screen, GFX_LEFT, &w, &h);
    int ox = (h - 256) / 2, oy = (240 - 192) / 2;
    for (int x = 0; x < 256; x++) {
        uint16_t *col = fb + (x + ox) * 240 + (239 - oy);
        const uint16_t *s = src + x;
        for (int y = 0; y < 192; y++) col[-y] = sLut[s[y * 256] & 0x7FFF];
    }
}

void port_ctr_present(const uint16_t *top, const uint16_t *bottom) {
    blit(GFX_TOP, top);
    blit(GFX_BOTTOM, bottom);
    gfxFlushBuffers();
    gfxSwapBuffers();
}

// ---------------------------------------------------------------- a second core
// The New 3DS's third core is the application's own; on an original 3DS, core 1 is shared with
// the system and the application gets a share of it.
int port_ctr_thread_start(void (*fn)(void *), int stack) {
    Thread t = threadCreate(fn, NULL, stack, 0x30, 2, true);
    if (t) { port_log("render thread on core 2\n"); return 1; }
    APT_SetAppCpuTimeLimit(80);
    t = threadCreate(fn, NULL, stack, 0x30, 1, true);
    if (t) { port_log("render thread on core 1\n"); return 1; }
    return 0;
}

void port_ctr_wait_signal(volatile int *flag, int value) {
    while (*flag != value) svcSleepThread(100000);
}

// ---------------------------------------------------------------- the render core
// The frame's picture (port_gpu2d_frame: BG planes, sprites, the 3D layer, the GPU's command
// lists) is built on another core while the game runs its next frame: a frame then costs the
// longer of the two, not their sum (field: ~9.5 ms of game, ~8.5 ms of frame building). It starts
// right after the VBlank handler's transfers (the state the DS would scan out) and must finish
// before anything changes video memory again: the next frame's VBlank handler, a VBlank during
// loading, a debug dump, the cheat menu. port_render_wait() is called before each. On the New
// 3DS: core 2, below the sound thread's priority; elsewhere (no spare core) inline, as before.
// debug.txt "render inline" turns it off.
int port_render_inline;
static Thread sRenderThread;
static LightEvent sRenderGo, sRenderDone;
static void (*volatile sRenderFn)(void);
static volatile int sRenderBusy;
uint64_t gRenderWaitTicks;  // (the game waiting for the render core: frame building is the limit)

static void render_main(void *arg) {
    (void)arg;
    for (;;) {
        LightEvent_Wait(&sRenderGo);
        void (*fn)(void) = sRenderFn;
        if (!fn) break;
        fn();
        sRenderBusy = 0;
        LightEvent_Signal(&sRenderDone);
    }
}

static int render_thread_up(void) {
    static int tried;
    if (sRenderThread || tried) return sRenderThread != NULL;
    tried = 1;
    LightEvent_Init(&sRenderGo, RESET_ONESHOT);
    LightEvent_Init(&sRenderDone, RESET_ONESHOT);
    s32 prio = 0x30;
    svcGetThreadPriority(&prio, CUR_THREAD_HANDLE);
    sRenderThread = threadCreate(render_main, NULL, 128 * 1024, prio + 1, 2, false);
    extern void port_log(const char *fmt, ...);
    port_log(sRenderThread ? "render: on core 2\n" : "render: inline (no core 2)\n");
    return sRenderThread != NULL;
}

void port_render_start(void (*fn)(void)) {
    if (port_render_inline || !render_thread_up()) { fn(); return; }
    sRenderFn = fn;
    sRenderBusy = 1;
    LightEvent_Signal(&sRenderGo);
}

void port_render_wait(void) {
    if (!sRenderBusy) return;
    uint64_t t = svcGetSystemTick();
    while (sRenderBusy) LightEvent_Wait(&sRenderDone);
    gRenderWaitTicks += svcGetSystemTick() - t;
}

// ---------------------------------------------------------------- the watchdog
// A freeze leaves no crash dump and no last log line (the log stops with the frame). So a thread
// that wakes twice a second logs, when no frame has finished for 3 s, where the game is stuck:
// the stage (port_stage, set at the frame's steps) and the frame. Above the game thread's
// priority on its core: it runs even if the game spins.
volatile const char *port_stage = "start";
static void watchdog_main(void *arg) {
    (void)arg;
    extern volatile uint32_t port_frame_count;
    extern void port_log(const char *fmt, ...);
    uint32_t last = 0, still = 0, reported = 0;
    for (;;) {
        svcSleepThread(500000000LL);
        uint32_t f = port_frame_count;
        if (f != last) { last = f; still = 0; reported = 0; continue; }
        if (++still >= 6 && !reported) {
            reported = 1;
            port_log("WATCHDOG: no frame for 3 s at frame %lu, stage: %s\n", (unsigned long)f, (const char *)port_stage);
        }
    }
}
void port_watchdog_start(void) {
    s32 prio = 0x30;
    svcGetThreadPriority(&prio, CUR_THREAD_HANDLE);
    if (!threadCreate(watchdog_main, NULL, 8 * 1024, prio - 1, -2, true)) port_log("watchdog: not started\n");
}
