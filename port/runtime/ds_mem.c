// The DS address space the game expects, as shadow memory in the 3DS heap region.
//
// Every DS hardware address the game, the SDK and the decomp's assembly use is moved up by
// PORT_DS_DELTA (port/include/port_ds.h), so a DS address A lives at A + 0x08000000 on the 3DS.
// Here the parts that are used get committed as plain read/write memory: main memory (with the
// system area, DTCM and the OS arena), the shared WRAM, I/O registers, palettes, VRAM and OAM.
// The renderer reads the registers, VRAM, palettes and OAM from here each frame.
#include <3ds.h>
#include <stdio.h>
#include <string.h>
#include "port_ds.h"
#include "port.h"

// libctru's own heap stays below the window: 0x08000000 up to 0x09800000. The linear heap (GPU
// buffers) is fixed too, so the window's ~19 MB is left for ds_mem: an original 3DS gives an
// application 64 MB in all (code ~5 MB + 20 + 12 + 19).
u32 __ctru_heap_size = 20 * 1024 * 1024;
u32 __ctru_linear_heap_size = 12 * 1024 * 1024;

typedef struct {
    u32 ds;    // DS address
    u32 size;  // bytes (whole pages)
    const char *name;
} Region;

static const Region kRegions[] = {
    { 0x01FF8000, 0x00008000, "ITCM" },
    { 0x02000000, 0x00800000, "main memory (with DTCM and the system area)" },
    { 0x037F8000, 0x00008000, "shared WRAM" },
    { 0x04000000, 0x00010000, "I/O registers" },
    { 0x04100000, 0x00001000, "I/O registers (FIFO, card)" },
    { 0x05000000, 0x00001000, "palettes" },
    { 0x06000000, 0x00080000, "main BG VRAM" },
    { 0x06200000, 0x00020000, "sub BG VRAM" },
    { 0x06400000, 0x00040000, "main OBJ VRAM" },
    { 0x06600000, 0x00020000, "sub OBJ VRAM" },
    { 0x06800000, 0x000A4000, "LCDC VRAM (banks A-I)" },
    { 0x07000000, 0x00001000, "OAM" },
};

bool port_ds_mem_init(void) {
    for (size_t i = 0; i < sizeof(kRegions) / sizeof(kRegions[0]); i++) {
        const Region *r = &kRegions[i];
        u32 out = 0;
        Result rc = svcControlMemory(&out, r->ds + PORT_DS_DELTA, 0, r->size, MEMOP_ALLOC,
                                     MEMPERM_READ | MEMPERM_WRITE);
        if (R_FAILED(rc) || out != r->ds + PORT_DS_DELTA) {
            port_log("ds_mem: cannot commit %s at %08lX (%08lX)\n", r->name, r->ds + PORT_DS_DELTA, rc);
            return false;
        }
        memset((void *)out, 0, r->size);
    }
    return true;
}
