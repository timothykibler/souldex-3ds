/* (checking, compiled into the ARM7 driver: its players' and channels' volume state as text) */
#include <stdio.h>
#include <nitro/snd/common/work.h>
#include <nitro/snd/common/util.h>

void SND_PortDiag(char *out, int cap) {
    int n = 0;
    for (int p = 0; p < SND_PLAYER_NUM && n < cap - 80; p++) {
        const SNDPlayer *pl = &SNDi_Work.player[p];
        if (!pl->active_flag) continue;
        n += snprintf(out + n, cap - n, " P%d vol %d fader %d tracks", p, pl->volume, pl->extFader);
        for (int t = 0; t < SND_TRACK_NUM_PER_PLAYER && n < cap - 40; t++) {
            if (pl->tracks[t] == 0xFF) continue;
            const SNDTrack *tr = &SNDi_Work.track[pl->tracks[t]];
            n += snprintf(out + n, cap - n, " %d/%d", tr->volume, tr->volume2);
        }
    }
    for (int c = 0; c < SND_CHANNEL_NUM && n < cap - 60; c++) {
        const SNDExChannel *ch = &SNDi_Work.channel[c];
        if (!ch->active_flag) continue;
        n += snprintf(out + n, cap - n, " C%d vel %d env %d/%ld ud %d/%d vol %03X", c, ch->velocity, ch->env_status,
                      (long)ch->env_decay, ch->user_decay, ch->user_decay2, ch->volume);
    }
}
