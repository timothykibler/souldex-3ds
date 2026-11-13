// debug.txt "strmtest F": from frame F, a 440 Hz tone through NitroSystem's sound stream (the
// path only the game's Wi-Fi voice chat uses, so untestable through the game offline): checks
// the sound alarms that ask for each half-buffer. The log gets the refill count every 600 frames
// (~9 a second at this rate); "wavdump" records whether the tone is continuous.
#include <nitro.h>
#include <nnsys/snd.h>
#include <math.h>
#include "port.h"

int port_strmtest_frame_no;
static NNSSndStrm sStrm;
static s16 sBuf[1088] __attribute__((aligned(32)));  // (two halves of 544 samples, as the voice chat)
static u32 sRefills, sPhase;
#define TIMER 0x41  // (x32 = a channel timer of 2080: 8056 Hz, the voice chat's rate)

static void fill(NNSSndStrmCallbackStatus status, int numChannels, void *buffer[], u32 len, NNSSndStrmFormat format, void *arg) {
    (void)status; (void)numChannels; (void)format; (void)arg;
    s16 *p = buffer[0];
    const double rate = 16756991.0 / (TIMER * 32);
    for (u32 i = 0; i < len / 2; i++, sPhase++)
        p[i] = (s16)(12000.0 * sin(2.0 * 3.14159265358979 * 440.0 * sPhase / rate));
    sRefills++;
}

void port_strmtest_frame(void) {
    extern volatile uint32_t port_frame_count;
    if (!port_strmtest_frame_no) return;
    if (port_frame_count == (u32)port_strmtest_frame_no) {
        static const u8 ch[1] = { 14 };
        NNS_SndStrmInit(&sStrm);
        BOOL ok = NNS_SndStrmAllocChannel(&sStrm, 1, ch)
               && NNS_SndStrmSetup(&sStrm, NNS_SND_STRM_FORMAT_PCM16, sBuf, sizeof(sBuf), TIMER, 2, fill, NULL);
        if (ok) { NNS_SndStrmSetVolume(&sStrm, 0); NNS_SndStrmStart(&sStrm); }
        port_log("strmtest: started (%s)\n", ok ? "ok" : "failed");
    }
    if (port_frame_count > (u32)port_strmtest_frame_no && port_frame_count % 600 == 0)
        port_log("strmtest: %lu refills by frame %lu\n", (unsigned long)sRefills, (unsigned long)port_frame_count);
}
