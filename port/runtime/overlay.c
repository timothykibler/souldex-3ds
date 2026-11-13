// NitroSDK's overlay loading (fs_overlay.c), for overlays that are all resident.
//
// FS_LoadOverlayInfo still reads the ROM's overlay table, so the game sees the same RAM addresses
// and sizes it would on a DS (it uses them to know which overlays share memory). Loading an image
// only resets the overlay's own variables; nothing is copied from the card.
#include <string.h>
#include <nitro.h>
#include "port.h"

void port_overlay_load(uint32_t id);

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

void FS_ClearOverlayImage(FSOverlayInfo *p_ovi) { port_overlay_load(p_ovi->header.id); }
BOOL FS_LoadOverlayImage(FSOverlayInfo *p_ovi) { port_overlay_load(p_ovi->header.id); return TRUE; }
BOOL FS_LoadOverlayImageAsync(FSOverlayInfo *p_ovi, FSFile *p_file) {
    // (the caller waits on and closes this file afterwards, so it has to be a real open file)
    if (!FS_OpenFileFast(p_file, FS_GetOverlayFileID(p_ovi))) return FALSE;
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
