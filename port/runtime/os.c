// NitroSDK's OS module, the parts that were ARM9 assembly or drove DS hardware: interrupts,
// the frame wait, ticks, alarms, locks, threads and printing. The rest of the OS module (arenas,
// heaps, the interrupt table, mutexes, messages) is the decomp's own C, built as it is.
//
// Phase 0 runs everything on one thread: the game's main loop. Interrupts are not real: the
// VBlank handler the game registers runs from OS_WaitIrq, once per 3DS frame.
#include <stdarg.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <nitro.h>
#include "port.h"

// ---------------------------------------------------------------- interrupts (none are real)
static OSIntrMode sIntrMode = OS_INTRMODE_IRQ_ENABLE;

OSIntrMode OS_DisableInterrupts(void) { OSIntrMode m = sIntrMode; sIntrMode = OS_INTRMODE_IRQ_DISABLE; return m; }
OSIntrMode OS_EnableInterrupts(void) { OSIntrMode m = sIntrMode; sIntrMode = OS_INTRMODE_IRQ_ENABLE; return m; }
OSIntrMode OS_RestoreInterrupts(OSIntrMode state) { OSIntrMode m = sIntrMode; sIntrMode = state; return m; }
OSIntrMode OS_DisableInterrupts_IrqAndFiq(void) { return OS_DisableInterrupts(); }
OSIntrMode OS_RestoreInterrupts_IrqAndFiq(OSIntrMode state) { return OS_RestoreInterrupts(state); }
OSIntrMode_Irq OS_GetCpsrIrq(void) { return (OSIntrMode_Irq)sIntrMode; }
static int sInIrq;  // inside the VBlank handler (as an interrupt)
OSProcMode OS_GetProcMode(void) { return sInIrq ? OS_PROCMODE_IRQ : OS_PROCMODE_SYS; }
void OS_IrqHandler(void) {}
void OS_Halt(void) { port_frame(); }
void OS_SpinWait(u32 cycle) { (void)cycle; }
void OS_Terminate(void) {
    // (the caller, and the one before it if frame pointers allow: look them up in soulsilver.map)
    port_fatal("OS_Terminate from %p (lr), %p", __builtin_return_address(0), __builtin_extract_return_addr(__builtin_return_address(0)));
}

// The interrupt check flag the VBlank handlers set (HW_INTR_CHECK_BUF, at the end of DTCM)
#define INTR_CHECK (*(vu32 *)HW_INTR_CHECK_BUF)

volatile uint32_t port_frame_count;
u32 port_dump_every;  // (set from the SD card: debug.txt)

static void run_alarms(void);
static u64 renderTicks;

void port_input_update(void);

// The VBlank interrupt during long work (loading): on the DS it fires every 16.7 ms wherever the
// game is, and the game relies on it (the field queues more one-off VBlank tasks while loading
// than its queue holds: they drain as it goes). Called at points where the game waits on the
// card; runs the game's VBlank handler if a frame's time has passed and interrupts are enabled.
static u64 sLastVblank;
void port_vblank_irq_maybe(void) {
    if (sInIrq || sIntrMode == OS_INTRMODE_IRQ_DISABLE || !sLastVblank) return;
    u64 now = port_ctr_ticks();
    if (now - sLastVblank < 268111856ull / 60) return;
    sLastVblank = now;
    OSIrqFunction vblank = OS_IRQTable[REG_OS_IE_VB_SHIFT];
    if (vblank) { sInIrq = 1; vblank(); sInIrq = 0; }
    INTR_CHECK |= OS_IE_V_BLANK;
}

static u64 sGameTicks, sFrameEnter, sFrameExit;
void port_frame(void) {
    sFrameEnter = port_ctr_ticks();
    if (sFrameExit) sGameTicks += sFrameEnter - sFrameExit;  // (the game's own code since the last frame)
    if (!port_ctr_frame_wait()) port_ctr_exit(0);
    port_frame_count++;
    if (port_frame_count % 600 == 0) {
        extern u32 port_gx_fifo_words(void);
        static u64 last;
        u64 now = port_ctr_ticks();
        port_log("frame %lu: %lu 3D words, %lu fps, 2D (GPU path, CPU side) %lu us/frame\n", (unsigned long)port_frame_count,
                 (unsigned long)port_gx_fifo_words(), (unsigned long)(last ? 600ull * 268111856ull / (now - last) : 0),
                 (unsigned long)(renderTicks / 600 / 268));
        last = now;
        renderTicks = 0;
        extern u64 gGpu2dBusyTicks;
        extern u64 gGpu2dMaxTicks;
        extern u32 gGpu2dRebuilds;
        port_log("  2D frame building (CPU): %lu us avg, %lu us worst, %lu BG rebuilds\n", (unsigned long)(gGpu2dBusyTicks / 600 / 268),
                 (unsigned long)(gGpu2dMaxTicks / 268), (unsigned long)gGpu2dRebuilds);
        gGpu2dBusyTicks = gGpu2dMaxTicks = 0;
        gGpu2dRebuilds = 0;
        extern u64 gProfPlanes, gProfSprites, gProf3d;
        port_log("  of which: BG planes %lu us, sprite sheet %lu us, 3D layer %lu us per frame\n", (unsigned long)(gProfPlanes / 600 / 268),
                 (unsigned long)(gProfSprites / 600 / 268), (unsigned long)(gProf3d / 600 / 268));
        extern u64 gProf3dTex, gProf3dWide, gProf3dIds;
        port_log("  3D: textures %lu us, widening test %lu us, edge IDs %lu us\n", (unsigned long)(gProf3dTex / 600 / 268), (unsigned long)(gProf3dWide / 600 / 268), (unsigned long)(gProf3dIds / 600 / 268));
        gProfPlanes = gProfSprites = gProf3d = gProf3dTex = gProf3dWide = gProf3dIds = 0;
        extern u64 gGpu2dWaitTicks;
        extern u64 gG3Ticks;
        port_log("  geometry engine %lu us per frame (part of the game code)\n", (unsigned long)(gG3Ticks / 600 / 268));
        gG3Ticks = 0;
        port_log("  game code %lu us, waiting for the GPU / frame limit %lu us per frame\n", (unsigned long)(sGameTicks / 600 / 268), (unsigned long)(gGpu2dWaitTicks / 600 / 268));
        sGameTicks = gGpu2dWaitTicks = 0;
        extern void port_snd_report(void);
        port_snd_report();
    }
    port_input_update();
    extern void port_wm_frame(void);
    port_wm_frame();
    extern void port_cheats_frame(void);
    port_cheats_frame();
    run_alarms();
    // The game's VBlank handler (OS_SetIrqFunction(OS_IE_V_BLANK, ...)): it does the frame's
    // transfers and sets the interrupt check flag the main loop waits for.
    OSIrqFunction vblank = OS_IRQTable[REG_OS_IE_VB_SHIFT];
    if (vblank && !sInIrq) { sInIrq = 1; vblank(); sInIrq = 0; }
    INTR_CHECK |= OS_IE_V_BLANK;
    sLastVblank = port_ctr_ticks();
    // The frame the DS would now scan out: drawn from the state after the VBlank transfers, on the
    // render core while the game runs its next frame (shown a frame later), or here if there's none.
    extern int port_g3_vblank(void);
    port_g3_vblank();
    extern void port_gpu2d_frame(void);
    extern void port_gpu2d_readback(u16 *top, u16 *bottom);
    u64 t0 = port_ctr_ticks();
    port_gpu2d_frame();
    renderTicks += port_ctr_ticks() - t0;
    if (port_dump_every && port_frame_count % port_dump_every == 0) {
        static u16 top[256 * 192], bottom[256 * 192];
        void port_dump_screens(const u16 *, const u16 *, u32);
        port_gpu2d_readback(top, bottom);
        port_dump_screens(top, bottom, port_frame_count);
        void port_gpu2d_dump_full(u32 frame);
        port_gpu2d_dump_full(port_frame_count);
        // the software renderer's picture of the same frame, and how far apart they are
        static u16 stop[256 * 192], sbot[256 * 192];
        port_render2d(stop, sbot);
        port_dump_screens(stop, sbot, port_frame_count + 1);
        // (counting pixels where a channel is off by more than one step of 32: the GPU rounds blends
        // and fades where the DS truncates, which never differs by more than one)
        u32 diff[2] = { 0, 0 };
        for (int k = 0; k < 2; k++) {
            const u16 *g = k ? bottom : top, *w = k ? sbot : stop;
            for (int i = 0; i < 256 * 192; i++) {
                for (int sh = 0; sh < 15; sh += 5) {
                    int d = (int)((g[i] >> sh) & 31) - (int)((w[i] >> sh) & 31);
                    if (d > 1 || d < -1) { diff[k]++; break; }
                }
            }
        }
        port_log("frame %lu: GPU vs software: %lu / %lu pixels differ by more than 1/32 (top / bottom)\n", (unsigned long)port_frame_count,
                 (unsigned long)diff[0], (unsigned long)diff[1]);
        if (diff[0] || diff[1]) {
            void port_gpu2d_describe(int engine);
            port_gpu2d_describe(0);
            port_gpu2d_describe(1);
        }
    }
    sFrameExit = port_ctr_ticks();
}

void OS_WaitIrq(BOOL clear, OSIrqMask irqFlags) {

    if (clear) INTR_CHECK &= ~irqFlags;
    while (!(INTR_CHECK & irqFlags)) port_frame();
}

void OS_WaitVBlankIntr(void) {
    OS_WaitIrq(TRUE, OS_IE_V_BLANK);
}

void OS_WaitAnyIrq(void) {
    port_frame();
}

void OS_Init(void) {
    OS_InitArena();
    OS_InitIrqTable();
    OS_SetIrqStackChecker();
    OS_InitLock();
    OS_InitTick();
    OS_InitAlarm();
    OS_InitThread();
}

// (OS_GetResetParameter reads HW_RESET_PARAMETER_BUF in the system area: 0 after a power-on)
void OS_ResetSystem(u32 parameter) {
    port_fatal("soft reset (not supported yet, parameter %lu)", parameter);
}

// ---------------------------------------------------------------- TCM, protection unit, caches
u32 OS_GetDTCMAddress(void) { return HW_DTCM; }
void OS_EnableProtectionUnit(void) {}
void OS_DisableProtectionUnit(void) {}
void OS_SetProtectionRegion1(u32 param) { (void)param; }
void OS_SetProtectionRegion2(u32 param) { (void)param; }
void OS_SetDPermissionsForProtectionRegion(u32 setMask, u32 flags) { (void)setMask; (void)flags; }
void DC_FlushAll(void) {}
void DC_StoreAll(void) {}
void DC_InvalidateAll(void) {}
void DC_FlushRange(const void *startAddr, u32 nBytes) { (void)startAddr; (void)nBytes; }
void DC_StoreRange(const void *startAddr, u32 nBytes) { (void)startAddr; (void)nBytes; }
void DC_InvalidateRange(void *startAddr, u32 nBytes) { (void)startAddr; (void)nBytes; }
void DC_WaitWriteBufferEmpty(void) {}
void IC_InvalidateAll(void) {}
void IC_InvalidateRange(void *startAddr, u32 nBytes) { (void)startAddr; (void)nBytes; }

// ---------------------------------------------------------------- ticks
// OSTick counts at the DS's 33.513982 MHz / 64; the 3DS's system tick runs at 268.111856 MHz.
static u64 sTickBase;
vu64 OSi_TickCounter;  // (os_entropy.c mixes it in)
static BOOL sTickReady;
void OS_InitTick(void) { sTickBase = port_ctr_ticks(); sTickReady = TRUE; }
BOOL OS_IsTickAvailable(void) { return sTickReady; }
OSTick OS_GetTick(void) { return (OSTick)((port_ctr_ticks() - sTickBase) / 512); }
u16 OS_GetTickLo(void) { return (u16)OS_GetTick(); }
void OS_SetTick(OSTick count) { sTickBase = port_ctr_ticks() - (u64)count * 512; }

// ---------------------------------------------------------------- alarms (checked once a frame)
static struct OSiAlarmQueue sAlarms;
static BOOL sAlarmReady;
void OS_InitAlarm(void) { sAlarms.head = sAlarms.tail = NULL; sAlarmReady = TRUE; }
void OS_EndAlarm(void) { sAlarmReady = FALSE; }
BOOL OS_IsAlarmAvailable(void) { return sAlarmReady; }
struct OSiAlarmQueue *OSi_GetAlarmQueue(void) { return &sAlarms; }
void OS_CreateAlarm(OSAlarm *alarm) { memset(alarm, 0, sizeof(*alarm)); }

static void unlink_alarm(OSAlarm *a) {
    if (a->prev) a->prev->next = a->next; else if (sAlarms.head == a) sAlarms.head = a->next;
    if (a->next) a->next->prev = a->prev; else if (sAlarms.tail == a) sAlarms.tail = a->prev;
    a->prev = a->next = NULL;
}

void OSi_InsertAlarm(OSAlarm *alarm, OSTick fire) {
    alarm->fire = fire;
    alarm->prev = sAlarms.tail;
    alarm->next = NULL;
    if (sAlarms.tail) sAlarms.tail->next = alarm; else sAlarms.head = alarm;
    sAlarms.tail = alarm;
}

void OS_SetAlarm(OSAlarm *alarm, OSTick tick, OSAlarmHandler handler, void *arg) {
    alarm->handler = handler;
    alarm->arg = arg;
    alarm->period = 0;
    OSi_InsertAlarm(alarm, OS_GetTick() + tick);
}

void OS_SetPeriodicAlarm(OSAlarm *alarm, OSTick start, OSTick period, OSAlarmHandler handler, void *arg) {
    alarm->handler = handler;
    alarm->arg = arg;
    alarm->period = period;
    alarm->start = start;
    OSi_InsertAlarm(alarm, start + period);
}

void OS_CancelAlarm(OSAlarm *alarm) {
    if (alarm->handler) unlink_alarm(alarm);
    alarm->handler = NULL;
}

void OS_CancelAlarms(u32 tag) {
    for (OSAlarm *a = sAlarms.head; a;) {
        OSAlarm *n = a->next;
        if (a->tag == tag) OS_CancelAlarm(a);
        a = n;
    }
}

void OS_CancelAllAlarms(void) {
    while (sAlarms.head) OS_CancelAlarm(sAlarms.head);
}

void OS_SetAlarmTag(OSAlarm *alarm, u32 tag) { alarm->tag = tag; }

void OSi_SetTimer(OSAlarm *alarm) { (void)alarm; }

static void run_alarms(void) {
    OSTick now = OS_GetTick();
    for (OSAlarm *a = sAlarms.head; a;) {
        OSAlarm *n = a->next;
        if ((s64)(now - a->fire) >= 0) {
            OSAlarmHandler h = a->handler;
            void *arg = a->arg;
            unlink_alarm(a);
            if (a->period) OSi_InsertAlarm(a, a->fire + a->period);
            else a->handler = NULL;
            if (h) h(arg);
        }
        a = n;
    }
}

// ---------------------------------------------------------------- locks (one thread: never contended)
static u32 sLockIds;
void OS_InitLock(void) { sLockIds = 0; }
s32 OS_GetLockID(void) {
    for (int i = 0; i < 32; i++) {
        if (!(sLockIds & (1u << i))) { sLockIds |= 1u << i; return 0x40 + i; }
    }
    return OS_LOCK_ID_ERROR;
}
void OS_ReleaseLockID(u16 lockID) { if (lockID >= 0x40 && lockID < 0x60) sLockIds &= ~(1u << (lockID - 0x40)); }

static s32 lock_word(u16 lockID, OSLockWord *lockp, void (*ctrl)(void)) {
    if (lockp->ownerID && lockp->ownerID != lockID) return OS_LOCK_ERROR;
    lockp->ownerID = lockID;
    if (ctrl) ctrl();
    return OS_LOCK_SUCCESS;
}
static s32 unlock_word(u16 lockID, OSLockWord *lockp, void (*ctrl)(void)) {
    if (lockp->ownerID != lockID) return OS_UNLOCK_ERROR;
    if (ctrl) ctrl();
    lockp->ownerID = 0;
    return OS_UNLOCK_SUCCESS;
}
s32 OS_LockByWord(u16 lockID, OSLockWord *lockp, void (*ctrl)(void)) { return lock_word(lockID, lockp, ctrl); }
s32 OS_TryLockByWord(u16 lockID, OSLockWord *lockp, void (*ctrl)(void)) { return lock_word(lockID, lockp, ctrl); }
s32 OS_UnlockByWord(u16 lockID, OSLockWord *lockp, void (*ctrl)(void)) { return unlock_word(lockID, lockp, ctrl); }
s32 OSi_DoLockByWord(u16 lockID, OSLockWord *lockp, void (*ctrl)(void), BOOL disableFiq) { (void)disableFiq; return lock_word(lockID, lockp, ctrl); }
s32 OSi_DoTryLockByWord(u16 lockID, OSLockWord *lockp, void (*ctrl)(void), BOOL disableFiq) { (void)disableFiq; return lock_word(lockID, lockp, ctrl); }
s32 OSi_DoUnlockByWord(u16 lockID, OSLockWord *lockp, void (*ctrl)(void), BOOL disableFiq) { (void)disableFiq; return unlock_word(lockID, lockp, ctrl); }
u16 OS_ReadOwnerOfLockWord(OSLockWord *lockp) { return lockp->ownerID; }

static OSLockWord sCardLock, sCartridgeLock;
s32 OS_LockCard(u16 lockID) { return lock_word(lockID, &sCardLock, NULL); }
s32 OS_TryLockCard(u16 lockID) { return lock_word(lockID, &sCardLock, NULL); }
s32 OS_UnlockCard(u16 lockID) { return unlock_word(lockID, &sCardLock, NULL); }
s32 OS_UnLockCard(u16 lockID) { return unlock_word(lockID, &sCardLock, NULL); }
s32 OS_LockCartridge(u16 lockID) { return lock_word(lockID, &sCartridgeLock, NULL); }
s32 OS_TryLockCartridge(u16 lockID) { return lock_word(lockID, &sCartridgeLock, NULL); }
s32 OS_UnlockCartridge(u16 lockID) { return unlock_word(lockID, &sCartridgeLock, NULL); }
s32 OS_UnLockCartridge(u16 lockID) { return unlock_word(lockID, &sCartridgeLock, NULL); }

BOOL OSi_TryLockVram(u16 bank, u16 lockId) { (void)bank; (void)lockId; return TRUE; }
void OSi_UnlockVram(u16 bank, u16 lockId) { (void)bank; (void)lockId; }

// ---------------------------------------------------------------- threads (phase 0: only the main one)
OSThreadInfo OSi_ThreadInfo;
OSThreadQueue OSi_IrqThreadQueue;
static OSThread sMainThread;
static BOOL sThreadReady;

void OS_InitThread(void) {
    if (sThreadReady) return;
    memset(&sMainThread, 0, sizeof(sMainThread));
    sMainThread.state = OS_THREAD_STATE_READY;
    sMainThread.priority = OS_THREAD_LAUNCHER_PRIORITY;
    OSi_ThreadInfo.current = &sMainThread;
    OSi_ThreadInfo.list = &sMainThread;
    sThreadReady = TRUE;
}
BOOL OS_IsThreadAvailable(void) { return sThreadReady; }

void OS_CreateThread(OSThread *thread, void (*func)(void *), void *arg, void *stack, u32 stackSize, u32 prio) {
    (void)thread; (void)func; (void)arg; (void)stack; (void)stackSize; (void)prio;
    port_fatal("OS_CreateThread: threads are not in phase 0 yet");
}
void OS_ExitThread(void) { port_fatal("OS_ExitThread"); }
void OSi_ExitThread(void *arg) { (void)arg; port_fatal("OSi_ExitThread"); }
void OSi_ExitThread_ArgSpecified(OSThread *thread, void *arg) { (void)thread; (void)arg; port_fatal("OSi_ExitThread_ArgSpecified"); }
void OSi_ExitThread_Destroy(void) { port_fatal("OSi_ExitThread_Destroy"); }
void OS_DestroyThread(OSThread *thread) { (void)thread; }
void OS_KillThread(OSThread *thread, void *arg) { (void)thread; (void)arg; }
void OS_JoinThread(OSThread *thread) { (void)thread; }
BOOL OS_IsThreadTerminated(const OSThread *thread) { return thread != &sMainThread; }
void OS_SleepThread(OSThreadQueue *queue) { (void)queue; port_frame(); }
void OS_WakeupThread(OSThreadQueue *queue) { (void)queue; }
void OS_WakeupThreadDirect(OSThread *thread) { (void)thread; }
void OS_RescheduleThread(void) {}
void OSi_RescheduleThread(void) {}
void OS_YieldThread(void) {}
u32 OS_DisableScheduler(void) { return 0; }
u32 OS_EnableScheduler(void) { return 0; }
u32 OS_GetThreadPriority(const OSThread *thread) { return thread->priority; }
BOOL OS_SetThreadPriority(OSThread *thread, u32 prio) { thread->priority = prio; return TRUE; }
void OS_Sleep(u32 msec) { port_ctr_sleep_ns((s64)msec * 1000000); }
OSThread *OS_GetThread(u32 id) { return id == 0 ? &sMainThread : NULL; }
u32 OSi_GetUnusedThreadId(void) { static u32 next = 1; return next++; }
void OSi_InsertThreadToList(OSThread *thread) { (void)thread; }
void OSi_RemoveThreadFromList(OSThread *thread) { (void)thread; }
int OS_GetErrno(void) { return 0; }

// The thread/mutex queue helpers os_mutex.c and os_message.c use.
void OSi_InsertLinkToQueue(OSThreadQueue *queue, OSThread *thread) {
    thread->link.next = NULL;
    thread->link.prev = queue->tail;
    if (queue->tail) queue->tail->link.next = thread; else queue->head = thread;
    queue->tail = thread;
}
OSThread *OSi_RemoveLinkFromQueue(OSThreadQueue *queue) {
    OSThread *t = queue->head;
    if (t) {
        queue->head = t->link.next;
        if (queue->head) queue->head->link.prev = NULL; else queue->tail = NULL;
        t->queue = NULL;
    }
    return t;
}
OSThread *OSi_RemoveSpecifiedLinkFromQueue(OSThreadQueue *queue, OSThread *thread) {
    for (OSThread *t = queue->head; t; t = t->link.next) {
        if (t == thread) {
            if (t->link.prev) t->link.prev->link.next = t->link.next; else queue->head = t->link.next;
            if (t->link.next) t->link.next->link.prev = t->link.prev; else queue->tail = t->link.prev;
            return t;
        }
    }
    return NULL;
}
OSMutex *OSi_RemoveMutexLinkFromQueue(OSMutexQueue *queue) {
    OSMutex *m = queue->head;
    if (m) {
        queue->head = m->link.next;
        if (queue->head) queue->head->link.prev = NULL; else queue->tail = NULL;
    }
    return m;
}

// ---------------------------------------------------------------- printing
int OS_VSPrintf(char *dst, const char *fmt, va_list vlist) { return vsprintf(dst, fmt, vlist); }
int OS_VSNPrintf(char *dst, size_t len, const char *fmt, va_list vlist) { return vsnprintf(dst, len, fmt, vlist); }
int OS_SPrintf(char *dst, const char *fmt, ...) {
    va_list ap; va_start(ap, fmt); int n = vsprintf(dst, fmt, ap); va_end(ap); return n;
}
int OS_SNPrintf(char *dst, size_t len, const char *fmt, ...) {
    va_list ap; va_start(ap, fmt); int n = vsnprintf(dst, len, fmt, ap); va_end(ap); return n;
}

// ---------------------------------------------------------------- PXI (the ARM7 link): nothing on the other end
void OSi_SendToPxi(u16 data) { (void)data; }
void OSi_ReferSymbol(void *symbol) { (void)symbol; }
void OS_InitException(void) {}
