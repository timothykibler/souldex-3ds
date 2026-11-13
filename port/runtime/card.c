// The game card: ROM reads come from the player's .nds file, and the save chip (a 4 Mbit flash,
// 512 KB) is a .sav file in the same raw format TWiLight Menu++ and other emulators use.
// Everything is synchronous: an "async" request has finished, callback called, when it returns.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <nitro.h>
#include "port.h"

#define PORT_DIR "sdmc:/3ds/soulsilver-3ds"

static FILE *sRom;
static u32 sRomSize;

static const char *const kRomPaths[] = {
    PORT_DIR "/soulsilver.nds",
    "sdmc:/roms/nds/soulsilver.nds",
    "sdmc:/roms/nds/Pokemon SoulSilver.nds",
    "sdmc:/soulsilver.nds",
};

bool port_rom_open(void) {
    for (size_t i = 0; i < sizeof(kRomPaths) / sizeof(kRomPaths[0]); i++) {
        sRom = fopen(kRomPaths[i], "rb");
        if (sRom) {
            char code[5] = {0};
            fseek(sRom, 0x0C, SEEK_SET);
            fread(code, 1, 4, sRom);
            fseek(sRom, 0, SEEK_END);
            sRomSize = (u32)ftell(sRom);
            port_log("ROM: %s (%s, %lu bytes)\n", kRomPaths[i], code, sRomSize);
            if (memcmp(code, "IPGE", 4) != 0) {
                port_log("  not SoulSilver (US): skipped\n");
                fclose(sRom);
                sRom = NULL;
                continue;
            }
            setvbuf(sRom, NULL, _IOFBF, 64 * 1024);
            return true;
        }
    }
    return false;
}

uint32_t port_rom_size(void) { return sRomSize; }

void port_rom_read(uint32_t offset, void *dst, uint32_t len) {
    void port_vblank_irq_maybe(void);
    port_vblank_irq_maybe();
    if (fseek(sRom, offset, SEEK_SET) != 0 || fread(dst, 1, len, sRom) != len) {
        port_fatal("ROM read failed: %lu bytes at %08lX", len, offset);
    }
}

// ---------------------------------------------------------------- ROM
static CARDResult sResult = CARD_RESULT_SUCCESS;

void CARD_Init(void) {}
BOOL CARD_IsAvailable(void) { return TRUE; }
BOOL CARD_IsEnabled(void) { return TRUE; }
void CARD_CheckEnabled(void) {}
void CARD_Enable(BOOL enable) { (void)enable; }
BOOL CARD_IsPulledOut(void) { return FALSE; }
void CARD_CheckPulledOut(void) {}
void CARD_LockRom(u16 lock_id) { (void)lock_id; }
void CARD_UnlockRom(u16 lock_id) { (void)lock_id; }
BOOL CARD_TryWaitRomAsync(void) { return TRUE; }
void CARD_WaitRomAsync(void) {}
u32 CARDi_ReadRomID(void) { return 0x00003FC2; }
u32 CARD_GetThreadPriority(void) { return CARD_THREAD_PRIORITY_DEFAULT; }
u32 CARD_SetThreadPriority(u32 prior) { (void)prior; return CARD_THREAD_PRIORITY_DEFAULT; }
CARDResult CARD_GetResultCode(void) { return sResult; }
const u8 *CARD_GetRomHeader(void) { return (const u8 *)HW_ROM_HEADER_BUF; }
void CARD_GetCacheFlushThreshold(u32 *icache, u32 *dcache) { if (icache) *icache = 0; if (dcache) *dcache = 0; }
void CARD_SetCacheFlushThreshold(u32 icache, u32 dcache) { (void)icache; (void)dcache; }

void CARDi_ReadRom(u32 dma, const void *src, void *dst, u32 len, MIDmaCallback callback, void *arg, BOOL is_async) {
    (void)dma; (void)is_async;
    port_rom_read((u32)src, dst, len);
    if (callback) callback(arg);
}

// ---------------------------------------------------------------- backup (the save)
#define SAVE_SIZE (512 * 1024)
static u8 *sSave;
static char sSavePath[256];
static CARDBackupType sBackupType;

static void save_load(void) {
    if (sSave) return;
    sSave = malloc(SAVE_SIZE);
    memset(sSave, 0xFF, SAVE_SIZE);
    snprintf(sSavePath, sizeof(sSavePath), PORT_DIR "/soulsilver.sav");
    FILE *f = fopen(sSavePath, "rb");
    if (f) {
        size_t n = fread(sSave, 1, SAVE_SIZE, f);
        fclose(f);
        port_log("save: %s (%u bytes)\n", sSavePath, (unsigned)n);
    } else {
        port_log("save: none yet (%s)\n", sSavePath);
    }
}

static void save_flush(void) {
    FILE *f = fopen(sSavePath, "wb");
    if (!f) { port_log("save: cannot write %s\n", sSavePath); return; }
    fwrite(sSave, 1, SAVE_SIZE, f);
    fclose(f);
}

BOOL CARD_IdentifyBackup(CARDBackupType type) {
    sBackupType = type;
    save_load();
    sResult = CARD_RESULT_SUCCESS;
    return TRUE;
}
CARDBackupType CARD_GetCurrentBackupType(void) { return sBackupType; }
u32 CARD_GetBackupTotalSize(void) { return SAVE_SIZE; }
u32 CARD_GetBackupSectorSize(void) { return 0x10000; }
u32 CARD_GetBackupPageSize(void) { return 0x100; }
void CARD_LockBackup(u16 lock_id) { (void)lock_id; }
void CARD_UnlockBackup(u16 lock_id) { (void)lock_id; }
BOOL CARD_TryWaitBackupAsync(void) { return TRUE; }
BOOL CARD_WaitBackupAsync(void) { return sResult == CARD_RESULT_SUCCESS; }
void CARD_CancelBackupAsync(void) {}

BOOL CARDi_RequestStreamCommand(u32 src, u32 dst, u32 len, MIDmaCallback callback, void *arg, BOOL is_async,
                                CARDRequest req_type, int req_retry, CARDRequestMode req_mode) {
    (void)is_async; (void)req_retry; (void)req_mode;
    save_load();
    sResult = CARD_RESULT_SUCCESS;
    switch (req_type) {
    case CARD_REQ_READ_BACKUP:  // src: save offset, dst: memory
        if (src + len > SAVE_SIZE) { sResult = CARD_RESULT_INVALID_PARAM; break; }
        memcpy((void *)dst, sSave + src, len);
        port_log("card: read %lX+%lX -> %08lX\n", (unsigned long)src, (unsigned long)len, (unsigned long)dst);
        break;
    case CARD_REQ_WRITE_BACKUP:
    case CARD_REQ_PROGRAM_BACKUP:  // src: memory, dst: save offset
        if (dst + len > SAVE_SIZE) { sResult = CARD_RESULT_INVALID_PARAM; break; }
        memcpy(sSave + dst, (const void *)src, len);
        save_flush();
        break;
    case CARD_REQ_VERIFY_BACKUP:
        if (dst + len > SAVE_SIZE || memcmp(sSave + dst, (const void *)src, len) != 0) sResult = CARD_RESULT_FAILURE;
        break;
    case CARD_REQ_ERASE_PAGE_BACKUP:
    case CARD_REQ_ERASE_SECTOR_BACKUP:
    case CARD_REQ_ERASE_SUBSECTOR_BACKUP:
        if (dst + len <= SAVE_SIZE) { memset(sSave + dst, 0xFF, len); save_flush(); }
        break;
    case CARD_REQ_ERASE_CHIP_BACKUP:
        memset(sSave, 0xFF, SAVE_SIZE);
        save_flush();
        break;
    default:
        port_log("card: request %d not handled\n", req_type);
        break;
    }
    if (callback) callback(arg);
    return sResult == CARD_RESULT_SUCCESS;
}

BOOL CARDi_RequestWriteSectorCommand(u32 src, u32 dst, u32 len, BOOL verify, MIDmaCallback callback, void *arg, BOOL is_async) {
    (void)verify;
    return CARDi_RequestStreamCommand(src, dst, len, callback, arg, is_async, CARD_REQ_WRITE_BACKUP, 1, CARD_REQUEST_MODE_SEND);
}

int CARDi_AccessStatus(CARDRequest command, u8 value) { (void)command; (void)value; return 0; }

// The game's own SPI helpers (src/field/fieldmap.c, overlay 112): a flash status read returns 0xAA
// on a real cartridge after the anti-copy check, which the port has nothing to check.
int CARD_SpiWaitGetStatus(void) { return 170; }
void CARD_SpiWaitInit(void) {}
void CARD_SetSpiWriteWaitCycles(u32 c) { (void)c; }
void CARD_SetSpiReadWaitCycles(u32 c) { (void)c; }
BOOL CARD_SpiWaitReadRange(u32 a, void *d, u32 l) { (void)a; (void)d; (void)l; return TRUE; }
BOOL CARD_SpiWaitWriteRange(u32 a, const void *d, u32 l) { (void)a; (void)d; (void)l; return TRUE; }

const char _version_NINTENDO_BACKUP[] = "[SDK+NINTENDO:BACKUP]";
