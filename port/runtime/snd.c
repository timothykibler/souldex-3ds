// Sound: the DS's ARM7 half, on a core of its own.
//
// The game (ARM9) drives NitroSystem's sound player, which sends command lists to the ARM7. There
// the SDK's sound driver (libsnd_arm7: sequencer, instruments, envelopes, voice allocation) runs
// every 5.2 ms and programs 16 hardware channels. The port builds that driver as it is (port/
// build.py: every symbol with an a7_ prefix, its I/O registers in port_a7_io) and runs it here,
// on a thread on another core, with a mixer standing in for the DS's sound hardware: the 16
// channels (PCM8, PCM16, IMA-ADPCM, PSG square waves, noise) at their timer rates, volume, shift
// and pan, mixed at 32768 Hz and played through the 3DS's DSP (NDSP). The driver's tick runs in
// the output's time: before each 5.2 ms of samples.
//
// The two sides talk as on the DS: command lists through the PXI FIFO (here a ring the game's
// PXI_SendWordByFifo fills, devices.c), results in the shared work area the driver updates.
// NDSP needs the DSP firmware (sdmc:/3ds/dspfirm.cdc, dumped once with DSP1); without it the
// driver still runs, silently, so the game never waits on it.
#include <3ds.h>
#include <stdio.h>
#include <string.h>
#include "port.h"

// ---------------------------------------------------------------- the ARM7 side's I/O registers
unsigned char port_a7_io[0x1000] __attribute__((aligned(4)));
volatile unsigned char port_a7_restart[16];  // (set by the driver's channel set-up / stop: port/arm7)
#define REG8(o) (*(volatile u8 *)(port_a7_io + (o)))
#define REG16(o) (*(volatile u16 *)(port_a7_io + (o)))
#define REG32(o) (*(volatile u32 *)(port_a7_io + (o)))

// ---------------------------------------------------------------- the PXI FIFO: command lists in
#define RING 64
static volatile u32 sRing[RING];
static volatile u32 sRingHead, sRingTail;  // (written: head by producers, tail by the sound thread)
static volatile int sRingLock;

// From the game (PXI_SendWordByFifo, PXI_FIFO_TAG_SOUND): a command list's address, or a message
// (small numbers: "process now", which the periodic tick does anyway).
void port_snd_pxi_send(u32 data) {
    if (data < 0x100) return;
    __sync_synchronize();  // (the list written before it's handed over)
    while (__sync_lock_test_and_set(&sRingLock, 1)) {}
    u32 h = sRingHead;
    if (h - sRingTail < RING) { sRing[h % RING] = data; __sync_synchronize(); sRingHead = h + 1; }
    __sync_lock_release(&sRingLock);
}

static u32 sCmdCount[40];
// The driver's OS message queue (its only one: incoming command lists) is that ring.
void port_a7_InitMessageQueue(void *q, void *buf, s32 n) { (void)q; (void)buf; (void)n; }
int port_a7_SendMessage(void *q, void *msg, s32 flags) { (void)q; (void)flags; port_snd_pxi_send((u32)msg); return 1; }
int port_a7_ReceiveMessage(void *q, void **msg, s32 flags) {
    (void)q; (void)flags;
    u32 t = sRingTail;
    if (t == sRingHead) return 0;
    __sync_synchronize();
    *msg = (void *)sRing[t % RING];
    sRingTail = t + 1;
    for (const u32 *c = (const u32 *)*msg; c; c = (const u32 *)c[0])  // (checking: commands by kind; SNDCommand: next, id, arg[4])
        if (c[1] < 40) sCmdCount[c[1]]++;
    return 1;
}
// (the driver runs on one thread, its own: nothing to mask)
int port_a7_DisableInterrupts(void) { return 0; }
int port_a7_RestoreInterrupts(int s) { return s; }
// (sound alarms, the ARM7 telling the ARM9 a timed event happened: not yet)
void port_a7_CreateAlarm(void *a) { (void)a; }
void port_a7_SetAlarm(void *a, u64 t, void *h, void *arg) { (void)a; (void)t; (void)h; (void)arg; }
void port_a7_SetPeriodicAlarm(void *a, u64 s, u64 p, void *h, void *arg) { (void)a; (void)s; (void)p; (void)h; (void)arg; }
void port_a7_CancelAlarm(void *a) { (void)a; }
int port_a7_PXI_SendWordByFifo(int tag, u32 data, int err) { (void)tag; (void)data; (void)err; return 0; }
void port_a7_PXI_SetFifoRecvCallback(int tag, void *cb) { (void)tag; (void)cb; }
int port_a7_PMi_SetControl(u32 v) { (void)v; return 0; }
int port_a7_PMi_ResetControl(u32 v) { (void)v; return 0; }
void port_a7_Assert(void) {}
void port_a7_MinMaxAssert(void) {}
void port_a7_ResetSoundBias(void) {}
void port_a7_SetSoundBias(void) {}
// (the parts of the driver's main file the port does itself)
void a7_SNDi_LockMutex(void) {}
void a7_SNDi_UnlockMutex(void) {}
int a7_SND_SendWakeupMessage(void) { return 1; }
void a7_SND_StartIntervalTimer(void) {}
void a7_SND_StopIntervalTimer(void) {}

// The driver (libsnd_arm7, renamed).
void a7_SND_CommandInit(void);
void a7_SND_ExChannelInit(void);
void a7_SND_SeqInit(void);
void a7_SND_AlarmInit(void);
void a7_SND_Enable(void);
void a7_SND_SetOutputSelector(int l, int r, int c1, int c3);
void a7_SND_SetMasterVolume(int v);
void a7_SND_UpdateExChannel(void);
void a7_SND_CommandProc(void);
void a7_SND_SeqMain(int periodic);
void a7_SND_ExChannelMain(int periodic);
void a7_SND_UpdateSharedWork(void);
u16 a7_SND_CalcRandom(void);

static void driver_tick(void) {
    a7_SND_UpdateExChannel();
    a7_SND_CommandProc();
    a7_SND_SeqMain(1);
    a7_SND_ExChannelMain(1);
    a7_SND_UpdateSharedWork();
    (void)a7_SND_CalcRandom();
}

// ---------------------------------------------------------------- the 16 channels
#define RATE 32768
#define DS_CLOCK 16756991.0  // (a channel timer's input: 33.51 MHz / 2)

typedef struct {
    int on;           // playing (latched at the enable bit's rising edge)
    const u8 *data;
    u32 total, loop;  // samples in all, the loop's first sample
    int format, repeat;
    double pos, step; // sample position, samples per output sample
    // IMA-ADPCM
    s32 pcm, index, at;     // decoder state, at sample `at`
    s32 loopPcm, loopIndex; int loopSaved;
    // PSG and noise
    u32 phase, lfsr;
    s16 last;
} Chan;
static Chan sCh[16];

static const s16 kAdpcmStep[89] = { 7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31, 34, 37, 41, 45, 50, 55, 60,
    66, 73, 80, 88, 97, 107, 118, 130, 143, 157, 173, 190, 209, 230, 253, 279, 307, 337, 371, 408, 449, 494, 544, 598, 658,
    724, 796, 876, 963, 1060, 1166, 1282, 1411, 1552, 1707, 1878, 2066, 2272, 2499, 2749, 3024, 3327, 3660, 4026, 4428,
    4871, 5358, 5894, 6484, 7132, 7845, 8630, 9493, 10442, 11487, 12635, 13899, 15289, 16818, 18500, 20350, 22385,
    24623, 27086, 29794, 32767 };
static const s8 kAdpcmIndex[8] = { -1, -1, -1, -1, 2, 4, 6, 8 };

static void adpcm_step(Chan *c) {  // decode sample c->at + 1
    u32 n = c->at + 1;
    u8 b = c->data[4 + n / 2];
    int nib = (n & 1) ? b >> 4 : b & 15;
    int step = kAdpcmStep[c->index];
    int diff = step / 8;
    if (nib & 1) diff += step / 4;
    if (nib & 2) diff += step / 2;
    if (nib & 4) diff += step;
    if (nib & 8) { c->pcm -= diff; if (c->pcm < -0x7FFF) c->pcm = -0x7FFF; }
    else { c->pcm += diff; if (c->pcm > 0x7FFF) c->pcm = 0x7FFF; }
    c->index += kAdpcmIndex[nib & 7];
    if (c->index < 0) c->index = 0;
    if (c->index > 88) c->index = 88;
    c->at = (s32)n;
}

static void chan_start(int i) {
    Chan *c = &sCh[i];
    u32 cnt = REG32(0x400 + i * 16);
    c->data = (const u8 *)REG32(0x404 + i * 16);
    u32 tmr = REG16(0x408 + i * 16), pnt = REG16(0x40A + i * 16), len = REG32(0x40C + i * 16) & 0x3FFFFF;
    c->format = (cnt >> 29) & 3;
    c->repeat = (cnt >> 27) & 3;
    c->pos = 0;
    c->step = DS_CLOCK / (double)(0x10000 - tmr) / RATE;
    switch (c->format) {
    case 0: c->loop = pnt * 4; c->total = (pnt + len) * 4; break;
    case 1: c->loop = pnt * 2; c->total = (pnt + len) * 2; break;
    case 2:
        c->loop = pnt ? (pnt - 1) * 8 : 0; c->total = (pnt + len) ? (pnt + len - 1) * 8 : 0;
        c->pcm = c->data ? (s16)(c->data[0] | c->data[1] << 8) : 0;
        c->index = c->data ? c->data[2] & 0x7F : 0;
        if (c->index > 88) c->index = 88;
        c->at = -1;  // (the header's sample comes before the first nibble)
        c->loopSaved = 0;
        break;
    default: c->total = 0xFFFFFFFF; c->phase = 0; c->lfsr = 0x7FFF; break;
    }
    c->on = c->data || c->format == 3;
}

// One output sample of channel i (and its position moved on); 0 when it's done.
static int chan_sample(int i, u32 cnt) {
    Chan *c = &sCh[i];
    // (the timer can change while playing: pitch bends, vibrato)
    u16 tmr = REG16(0x408 + i * 16);
    c->step = DS_CLOCK / (double)(0x10000 - tmr) / RATE;
    s32 s;
    if (c->format == 3) {
        u32 n = (u32)c->pos;
        while (c->phase < n) {  // (PSG: one duty step, noise: one LFSR shift, per timer period)
            c->phase++;
            if (i >= 14) {
                int carry = c->lfsr & 1;
                c->lfsr >>= 1;
                if (carry) c->lfsr ^= 0x6000;
                c->last = carry ? -0x7FFF : 0x7FFF;
            }
        }
        if (i >= 8 && i < 14) {
            int duty = (cnt >> 24) & 7;
            s = (duty == 7) ? -0x7FFF : ((int)(c->phase & 7) <= duty ? 0x7FFF : -0x7FFF);
        } else if (i >= 14) s = c->last;
        else s = 0;
    } else {
        u32 n = (u32)c->pos;
        if (n >= c->total) {
            if (c->repeat == 1 && c->total > c->loop) {  // loop
                n = c->loop + (n - c->total) % (c->total - c->loop);
                c->pos = n + (c->pos - (u32)c->pos);
                if (c->format == 2 && c->loopSaved) { c->pcm = c->loopPcm; c->index = c->loopIndex; c->at = (s32)c->loop; }
                else if (c->format == 2) { c->at = -1; c->pcm = (s16)(c->data[0] | c->data[1] << 8); c->index = c->data[2] & 0x7F; }
            } else {
                c->on = 0;
                REG32(0x400 + i * 16) = cnt & ~0x80000000u;  // (one-shot over: the hardware clears its enable bit)
                return 0;
            }
        }
        switch (c->format) {
        case 0: s = (s8)c->data[n] << 8; break;
        case 1: s = (s16)(c->data[n * 2] | c->data[n * 2 + 1] << 8); break;
        default:
            while (c->at < (s32)n) {
                adpcm_step(c);
                if (!c->loopSaved && c->at == (s32)c->loop) { c->loopPcm = c->pcm; c->loopIndex = c->index; c->loopSaved = 1; }
            }
            s = c->pcm;
            break;
        }
    }
    c->pos += c->step;
    return s;
}

// (checking: ticks run, the most channels on at once, the loudest sample, since the last report;
// and debug.txt "wavdump": the first 30 s of output into sound.wav)
static volatile u32 sTicks, sPeakChannels, sPeakLevel;
static u32 sChPeak[16], sChVol[16];
static u8 sChFmt[16];
int port_snd_wavdump;
static FILE *sWav;
static u32 sWavFrames;

static void mix(s16 *out, int frames) {
    u32 soundcnt = REG16(0x500);
    int master = soundcnt & 0x7F;
    for (int f = 0; f < frames; f++) {
        s32 l = 0, r = 0;
        if (soundcnt & 0x8000) {
            for (int i = 0; i < 16; i++) {
                u32 cnt = REG32(0x400 + i * 16);
                if (port_a7_restart[i]) { port_a7_restart[i] = 0; sCh[i].on = 0; }  // (set up again: starts afresh)
                if (!(cnt & 0x80000000u)) { sCh[i].on = 0; continue; }
                if (!sCh[i].on) chan_start(i);
                if (!sCh[i].on) continue;
                s32 s = chan_sample(i, cnt);
                static const u8 kShift[4] = { 0, 1, 2, 4 };
                s = s * (s32)(cnt & 0x7F) / 128 >> kShift[(cnt >> 8) & 3];
                { u32 a = (u32)(s < 0 ? -s : s); if (a > sChPeak[i]) sChPeak[i] = a; sChFmt[i] = (u8)sCh[i].format; sChVol[i] = cnt & 0x37F; }
                int pan = (cnt >> 16) & 0x7F;
                l += s * (128 - pan) / 128;
                r += s * pan / 128;
            }
        }
        l = l * master / 128;
        r = r * master / 128;
        {
            u32 on = 0, lv = (u32)(l < 0 ? -l : l);
            for (int i = 0; i < 16; i++) on += sCh[i].on;
            if (on > sPeakChannels) sPeakChannels = on;
            if (lv > sPeakLevel) sPeakLevel = lv;
        }
        out[f * 2] = (s16)(l > 32767 ? 32767 : l < -32768 ? -32768 : l);
        out[f * 2 + 1] = (s16)(r > 32767 ? 32767 : r < -32768 ? -32768 : r);
    }
}

// ---------------------------------------------------------------- the thread
// (the driver's period: 2728 cycles of a 33.51 MHz / 64 timer = 5.2095 ms)
#define TICK_SAMPLES (RATE * 2728.0 * 64.0 / 33513982.0)
#define BUF_FRAMES 512
#define NBUF 4
static ndspWaveBuf sWave[NBUF];
static s16 *sPcm;
static LightEvent sEvent;
static volatile int sRun;
static double sNextTick;
static u64 sMixed;

static void fill(s16 *out, int frames) {
    // the driver ticks at its own times within the samples (sample-accurate)
    int done = 0;
    while (done < frames) {
        if (sMixed >= sNextTick) { driver_tick(); sTicks++; sNextTick += TICK_SAMPLES; }
        int n = frames - done;
        double until = sNextTick - (double)sMixed;
        if (until < n) n = (int)until + (until > (int)until);
        if (n < 1) n = 1;
        mix(out + done * 2, n);
        done += n;
        sMixed += n;
    }
    extern volatile uint32_t port_frame_count;
    if (port_snd_wavdump && port_frame_count >= (u32)port_snd_wavdump - 1 && sWavFrames < RATE * 30) {
        if (!sWav) {
            sWav = fopen("sdmc:/3ds/soulsilver-3ds/sound.wav", "wb");
            static const u8 hdr[44] = { 'R','I','F','F', 0,0,0,0, 'W','A','V','E', 'f','m','t',' ', 16,0,0,0, 1,0, 2,0,
                0x00,0x80,0,0, 0x00,0x00,2,0, 4,0, 16,0, 'd','a','t','a', 0,0,0,0 };  // (32768 Hz, stereo, 16-bit)
            if (sWav) fwrite(hdr, 1, 44, sWav);
        }
        if (sWav) {
            fwrite(out, 4, frames, sWav);
            sWavFrames += frames;
            if (sWavFrames >= RATE * 30) {  // (the sizes into the header, and done)
                u32 data = sWavFrames * 4, riff = data + 36;
                fseek(sWav, 4, SEEK_SET); fwrite(&riff, 4, 1, sWav);
                fseek(sWav, 40, SEEK_SET); fwrite(&data, 4, 1, sWav);
                fclose(sWav); sWav = NULL;
            }
        }
    }
}

void a7_SND_PortDiag(char *out, int cap);
void port_snd_report(void) {
    { static char d[2048]; d[0] = 0; a7_SND_PortDiag(d, sizeof(d)); port_log("  sound state:%s\n", d); }
    port_log("  sound: %lu driver ticks, up to %lu channels, peak level %lu\n", (unsigned long)sTicks,
             (unsigned long)sPeakChannels, (unsigned long)sPeakLevel);
    port_log("  sound channels (format:peak/vol):");
    for (int i = 0; i < 16; i++) port_log(" %d:%u:%lu/%03lX", i, sChFmt[i], (unsigned long)sChPeak[i], (unsigned long)sChVol[i]);
    port_log("\n");
    memset(sChPeak, 0, sizeof(sChPeak));
    port_log("  sound commands:");
    for (int i = 0; i < 40; i++) if (sCmdCount[i]) port_log(" %d:%lu", i, (unsigned long)sCmdCount[i]);
    port_log("\n");
    memset(sCmdCount, 0, sizeof(sCmdCount));
    sTicks = sPeakChannels = sPeakLevel = 0;
}

static void dsp_callback(void *arg) { (void)arg; LightEvent_Signal(&sEvent); }

static void sound_thread(void *arg) {
    int dsp = (int)arg;
    a7_SND_CommandInit();
    a7_SND_ExChannelInit();
    a7_SND_SeqInit();
    a7_SND_AlarmInit();
    a7_SND_Enable();
    a7_SND_SetOutputSelector(0, 0, 0, 0);  // (the mixer for both sides, channels 1 and 3 too)
    a7_SND_SetMasterVolume(127);
    if (!dsp) {  // no DSP: the driver keeps its time, silently
        static s16 scratch[BUF_FRAMES * 2];
        while (sRun) {
            fill(scratch, BUF_FRAMES);
            svcSleepThread((s64)(1e9 * BUF_FRAMES / RATE));
        }
        return;
    }
    for (int i = 0; i < NBUF; i++) {
        memset(&sWave[i], 0, sizeof(sWave[i]));
        sWave[i].data_vaddr = sPcm + i * BUF_FRAMES * 2;
        sWave[i].nsamples = BUF_FRAMES;
        fill(sPcm + i * BUF_FRAMES * 2, BUF_FRAMES);
        DSP_FlushDataCache(sPcm + i * BUF_FRAMES * 2, BUF_FRAMES * 4);
        ndspChnWaveBufAdd(0, &sWave[i]);
    }
    while (sRun) {
        for (int i = 0; i < NBUF; i++) {
            if (sWave[i].status != NDSP_WBUF_DONE) continue;
            fill(sPcm + i * BUF_FRAMES * 2, BUF_FRAMES);
            DSP_FlushDataCache(sPcm + i * BUF_FRAMES * 2, BUF_FRAMES * 4);
            ndspChnWaveBufAdd(0, &sWave[i]);
        }
        LightEvent_Wait(&sEvent);
    }
}

void port_snd_init(void) {
    int dsp = R_SUCCEEDED(ndspInit());
    if (dsp) {
        sPcm = linearAlloc(NBUF * BUF_FRAMES * 4);
        if (!sPcm) { ndspExit(); dsp = 0; }
    }
    if (dsp) {
        ndspSetOutputMode(NDSP_OUTPUT_STEREO);
        ndspChnReset(0);
        ndspChnSetInterp(0, NDSP_INTERP_NONE);
        ndspChnSetRate(0, RATE);
        ndspChnSetFormat(0, NDSP_FORMAT_STEREO_PCM16);
        float mixv[12] = { 1, 1 };
        ndspChnSetMix(0, mixv);
        LightEvent_Init(&sEvent, RESET_ONESHOT);
        ndspSetCallback(dsp_callback, NULL);
    }
    port_log("sound: %s\n", dsp ? "DSP output" : "no DSP (sdmc:/3ds/dspfirm.cdc missing?): silent");
    sRun = 1;
    // (its own core: the New 3DS's third; else core 1, of which the application may have a share)
    Thread t = threadCreate(sound_thread, (void *)dsp, 32 * 1024, 0x18, 2, true);
    if (!t) { APT_SetAppCpuTimeLimit(30); t = threadCreate(sound_thread, (void *)dsp, 32 * 1024, 0x18, 1, true); }
    if (!t) t = threadCreate(sound_thread, (void *)dsp, 32 * 1024, 0x18, 0, true);
    port_log("sound: thread %s\n", t ? "started" : "FAILED");
}
