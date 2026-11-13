// The game's GF_ASSERT failures. Retail builds carry on after one (GF_AssertFail returns unless a
// debug flag is set), so a broken assumption in the port would pass silently: each failing call
// site is logged once (look it up in build-3ds/soulsilver.map). Linked with --wrap=GF_AssertFail.
#include <nitro.h>
#include "port.h"

extern volatile uint32_t port_frame_count;
void __real_GF_AssertFail(void);
void __wrap_GF_AssertFail(void) {
    static void *seen[64];
    static int n;
    void *ra = __builtin_return_address(0);
    int i;
    for (i = 0; i < n && seen[i] != ra; i++) {}
    if (i == n && n < 64) {
        seen[n++] = ra;
        port_log("assert failed at %p (frame %lu)\n", ra, (unsigned long)port_frame_count);
    }
    __real_GF_AssertFail();
}
