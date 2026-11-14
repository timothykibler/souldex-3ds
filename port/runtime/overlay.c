// NitroSDK's overlay loading (fs_overlay.c), for overlays that are all resident.
//
// FS_LoadOverlayInfo still reads the ROM's overlay table, so the game sees the same RAM addresses
// and sizes it would on a DS (it uses them to know which overlays share memory). Loading an image
// only resets the overlay's own variables; nothing is copied from the card.
#include <string.h>
#include <nitro.h>
#include "port.h"

void port_overlay_load(uint32_t id);

// Which overlay occupies which memory: on the DS, loading an overlay overwrites any it overlaps.
// The port keeps them all resident, but calls into shared memory are dispatched by this
// (port_overlay_dispatch, for the trampolines port/ovdispatch.py generates).
#define MAX_OV 130
static u8 sLoaded[MAX_OV];
static u32 sStart[MAX_OV], sEnd[MAX_OV];
static int sRanges;
static void ranges(void) {
    if (sRanges) return;
    sRanges = 1;
    const u8 *header = (const u8 *)HW_ROM_HEADER_BUF;
    u32 ovt_offset = *(const u32 *)(header + 0x50), ovt_size = *(const u32 *)(header + 0x54);
    for (u32 id = 0; id < MAX_OV && (id + 1) * sizeof(FSOverlayInfoHeader) <= ovt_size; id++) {
        FSOverlayInfoHeader h;
        port_rom_read(ovt_offset + id * sizeof(FSOverlayInfoHeader), &h, sizeof(h));
        sStart[id] = (u32)h.ram_address;
        sEnd[id] = (u32)h.ram_address + h.ram_size + h.bss_size;
    }
}
static void mark_loaded(u32 id) {
    if (id >= MAX_OV) return;
    ranges();
    for (u32 k = 0; k < MAX_OV; k++)
        if (k != id && sLoaded[k] && sStart[k] < sEnd[id] && sStart[id] < sEnd[k]) sLoaded[k] = 0;
    sLoaded[id] = 1;
}

typedef struct { unsigned char ov; void *fn; } OvCand;
typedef struct { unsigned long ds; void *dflt; const char *name; unsigned char n; const OvCand *c; } OvDispatch;
extern const OvDispatch port_ovdispatch[];
// The function to run for a call to port_ovdispatch[idx]: that address's function in whichever
// overlay is loaded there now (the one linked if none of them is known to be).
void *port_overlay_dispatch(u32 idx) {
    const OvDispatch *d = &port_ovdispatch[idx];
    for (int k = 0; k < d->n; k++) {
        if (!sLoaded[d->c[k].ov]) continue;
        if (d->c[k].fn != d->dflt) {
            static u8 said[512];
            if (idx < sizeof(said) && !said[idx]) { said[idx] = 1; port_log("ovdispatch: %s -> overlay %u's function at %08lX\n", d->name, d->c[k].ov, d->ds); }
        }
        return d->c[k].fn;
    }
    // (the overlay loaded there may have only a local function at that address, not in the
    // table: the one linked, as before; said once)
    static u8 told[512];
    for (u32 ov = 0; ov < MAX_OV; ov++)
        if (sLoaded[ov] && sStart[ov] <= d->ds && d->ds < sEnd[ov] && idx < sizeof(told) && !told[idx]) {
            told[idx] = 1;
            port_log("ovdispatch: %s while overlay %lu is loaded there, no function of its known: the linked one\n", d->name, (unsigned long)ov);
        }
    return d->dflt;
}

BOOL FS_LoadOverlayInfo(FSOverlayInfo *p_ovi, MIProcessor target, FSOverlayID id) {
    if (target != MI_PROCESSOR_ARM9 || id >= port_overlay_count()) return FALSE;
    const u8 *header = (const u8 *)HW_ROM_HEADER_BUF;
    u32 ovt_offset = *(const u32 *)(header + 0x50);
    u32 ovt_size = *(const u32 *)(header + 0x54);
    if ((id + 1) * sizeof(FSOverlayInfoHeader) > ovt_size) return FALSE;
    port_rom_read(ovt_offset + id * sizeof(FSOverlayInfoHeader), &p_ovi->header, sizeof(FSOverlayInfoHeader));
    p_ovi->target = target;
    // (the overlay's file in the ROM: FAT entry file_id)
    u32 fat_offset = *(const u32 *)(header + 0x48);
    u32 pos[2];
    port_rom_read(fat_offset + p_ovi->header.file_id * 8, pos, sizeof(pos));
    p_ovi->file_pos.offset = pos[0];
    p_ovi->file_pos.length = pos[1] - pos[0];
    return TRUE;
}

FSFileID FS_GetOverlayFileID(const FSOverlayInfo *p_ovi) {
    FSFileID f;
    f.arc = FS_FindArchive("rom", 3);
    f.file_id = p_ovi->header.file_id;
    return f;
}

void FS_ClearOverlayImage(FSOverlayInfo *p_ovi) { mark_loaded(p_ovi->header.id); port_overlay_load(p_ovi->header.id); }
BOOL FS_LoadOverlayImage(FSOverlayInfo *p_ovi) { mark_loaded(p_ovi->header.id); port_overlay_load(p_ovi->header.id); return TRUE; }
BOOL FS_LoadOverlayImageAsync(FSOverlayInfo *p_ovi, FSFile *p_file) {
    // (the caller waits on and closes this file afterwards, so it has to be a real open file)
    if (!FS_OpenFileFast(p_file, FS_GetOverlayFileID(p_ovi))) return FALSE;
    mark_loaded(p_ovi->header.id);
    port_overlay_load(p_ovi->header.id);
    return TRUE;
}
void FS_StartOverlay(FSOverlayInfo *p_ovi) { port_overlay_start(p_ovi->header.id); }
void FS_EndOverlay(FSOverlayInfo *p_ovi) { (void)p_ovi; }
BOOL FS_UnloadOverlayImage(FSOverlayInfo *p_ovi) { (void)p_ovi; return TRUE; }

BOOL FS_LoadOverlay(MIProcessor target, FSOverlayID id) {
    FSOverlayInfo info;
    if (!FS_LoadOverlayInfo(&info, target, id)) return FALSE;
    FS_LoadOverlayImage(&info);
    FS_StartOverlay(&info);
    return TRUE;
}

BOOL FS_UnloadOverlay(MIProcessor target, FSOverlayID id) { (void)target; (void)id; return TRUE; }
void FS_AttachOverlayTable(MIProcessor target, const void *ptr, u32 len) { (void)target; (void)ptr; (void)len; }
