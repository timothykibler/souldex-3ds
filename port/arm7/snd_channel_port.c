/* The ARM7 sound driver's channel layer (NitroSDK ARM7/snd_channel.c, included as it is) with
 * its set-up and stop functions reporting to the port's mixer (runtime/snd.c): on the DS a
 * channel restarts whenever its enable bit is written 0 then 1, which the driver does within one
 * tick (stop, set up, start) and the mixer, running between ticks, would never see. */
#define SND_SetupChannelPcm sdk_SND_SetupChannelPcm
#define SND_SetupChannelPsg sdk_SND_SetupChannelPsg
#define SND_SetupChannelNoise sdk_SND_SetupChannelNoise
#define SND_StopChannel sdk_SND_StopChannel
#include "snd_channel.c"
#undef SND_SetupChannelPcm
#undef SND_SetupChannelPsg
#undef SND_SetupChannelNoise
#undef SND_StopChannel

extern volatile unsigned char port_a7_restart[16];

void SND_SetupChannelPcm(int chNo, const void *dataaddr, SNDWaveFormat format, SNDChannelLoop loop, int loopStart,
                         int loopLen, int volume, SNDChannelDataShift shift, int timer, int pan) {
    // The ARM9's SND_SetupChannelPcm command packs the sample address into 27 bits (enough for any
    // DS address: UNPACK_COMMAND(arg, 0, 27) in snd_command.c); the port's addresses, the DS's
    // moved up by PORT_DS_DELTA (0x08000000), lose that bit on the way. Every sound a channel plays
    // directly (moves, cries, switching, the wave and stream players) arrived pointing 128 MB low:
    // a crash on hardware. Restored here, for the driver and the mixer alike.
    unsigned long a = (unsigned long)dataaddr;
    if (a >= 0x02000000 && a < 0x03000000) dataaddr = (const void *)(a + 0x08000000);
    sdk_SND_SetupChannelPcm(chNo, dataaddr, format, loop, loopStart, loopLen, volume, shift, timer, pan);
    port_a7_restart[chNo] = 1;
}
void SND_SetupChannelPsg(int chNo, SNDDuty duty, int volume, SNDChannelDataShift shift, int timer, int pan) {
    sdk_SND_SetupChannelPsg(chNo, duty, volume, shift, timer, pan);
    port_a7_restart[chNo] = 1;
}
void SND_SetupChannelNoise(int chNo, int volume, SNDChannelDataShift shift, int timer, int pan) {
    sdk_SND_SetupChannelNoise(chNo, volume, shift, timer, pan);
    port_a7_restart[chNo] = 1;
}
void SND_StopChannel(int chNo, s32 flags) {
    sdk_SND_StopChannel(chNo, flags);
    port_a7_restart[chNo] = 1;
}
