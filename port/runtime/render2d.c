// The DS's two 2D engines, drawn in software from the shadow registers, VRAM, palettes and OAM
// the game writes (port/runtime/ds_mem.c). One 256x192 picture per engine, 15-bit BGR, 1:1.
//
// Covered: text, affine and extended (affine 16-bit map, 256-colour and direct bitmap) BGs,
// sprites (normal and affine, 1D/2D mapping, 16/256 colours, extended palettes, semi-transparent,
// OBJ window), windows, colour effects (alpha, brighten, darken), master brightness, VRAM
// display mode and the screen swap. Not yet: the 3D layer (BG0 in 3D mode: phase 2), mosaic,
// bitmap sprites, large-bitmap mode 6, raster (H-blank) effects.
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "port_ds.h"
#include "port.h"

typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef int32_t s32;

#define DS(a) ((u8 *)(uintptr_t)((a) + PORT_DS_DELTA))

static inline u16 rd16(const u8 *p) { return (u16)(p[0] | p[1] << 8); }
static inline u32 rd32(const u8 *p) { return (u32)(p[0] | p[1] << 8 | p[2] << 16 | (u32)p[3] << 24); }

typedef struct {
    int main;           // engine A
    const u8 *io;       // its registers (0x04000000 / 0x04001000)
    const u8 *bgpal;    // standard palettes
    const u8 *objpal;
    const u8 *oam;
    const u8 *bgvram;   // its VRAM windows
    u32 bgvram_size;
    const u8 *objvram;
    u32 objvram_size;
    u32 dispcnt;
    u16 bgp[256], objp[256];  // this frame's palettes, as colour | OPAQUE
    u8 objlist[128];          // sprites that can show this frame
    int nobj;
} Engine;

// One line of one layer: colour (bit 15 set = opaque) and, for sprites, priority and flags.
#define OPAQUE 0x8000
static u16 sBg[4][256];
static u16 sObj[256];
static u8 sObjPrio[256];
static u8 sObjSemi[256];
static u8 sObjWin[256];

// ---------------------------------------------------------------- VRAM banks (for extended palettes and VRAM display)
static const u32 kBankLcdc[9] = { 0x06800000, 0x06820000, 0x06840000, 0x06860000, 0x06880000,
                                  0x06890000, 0x06894000, 0x06898000, 0x068A0000 };
static const u8 kBankCnt[9] = { 0x40, 0x41, 0x42, 0x43, 0x44, 0x45, 0x46, 0x48, 0x49 };

static const u8 *sIoBase, *sExtBase;
static u8 vramcnt(int bank) { return sIoBase[0x200 + kBankCnt[bank]]; }
static const u8 *bank_data(int bank) { return sExtBase + (kBankLcdc[bank] - 0x06880000); }

// Extended palette slot `slot` (0-3, 8 KB each) for BGs, or the OBJ one (slot -1), of an engine.
// The data is where the game wrote it: through the LCDC window, before mapping the bank.
static const u8 *ext_palette(const Engine *e, int slot) {
    if (e->main) {
        if (slot >= 0) {
            u8 c = vramcnt(4);  // E: all four BG slots
            if ((c & 0x80) && (c & 7) == 4) return bank_data(4) + slot * 0x2000;
            for (int b = 5; b <= 6; b++) {  // F, G: two slots each, at offset 0 or 2
                c = vramcnt(b);
                if ((c & 0x80) && (c & 7) == 4) {
                    int first = (c >> 3) & 1 ? 2 : 0;
                    if (slot == first || slot == first + 1) return bank_data(b) + (slot - first) * 0x2000;
                }
            }
        } else {
            u8 c = vramcnt(4);
            if ((c & 0x80) && (c & 7) == 5) return bank_data(4);
            for (int b = 5; b <= 6; b++) {
                c = vramcnt(b);
                if ((c & 0x80) && (c & 7) == 5) return bank_data(b);
            }
        }
    } else {
        if (slot >= 0) {
            u8 c = vramcnt(7);  // H: sub BG
            if ((c & 0x80) && (c & 3) == 2) return bank_data(7) + slot * 0x2000;
        } else {
            u8 c = vramcnt(8);  // I: sub OBJ
            if ((c & 0x80) && (c & 3) == 3) return bank_data(8);
        }
    }
    return NULL;
}

static inline u8 bgv8(const Engine *e, u32 a) { return a < e->bgvram_size ? e->bgvram[a] : 0; }
static inline u16 bgv16(const Engine *e, u32 a) { return a + 1 < e->bgvram_size ? rd16(e->bgvram + a) : 0; }
static inline u8 objv8(const Engine *e, u32 a) { return e->objvram[a & (e->objvram_size - 1)]; }

static u32 char_base(const Engine *e, u16 cnt) {
    return ((cnt >> 2) & 0xF) * 0x4000 + (e->main ? ((e->dispcnt >> 24) & 7) * 0x10000 : 0);
}
static u32 screen_base(const Engine *e, u16 cnt) {
    return ((cnt >> 8) & 0x1F) * 0x800 + (e->main ? ((e->dispcnt >> 27) & 7) * 0x10000 : 0);
}

// ---------------------------------------------------------------- text BGs
static void bg_text(const Engine *e, int bg, int line, u16 *out) {
    u16 cnt = rd16(e->io + 8 + bg * 2);
    u32 hofs = rd16(e->io + 0x10 + bg * 4) & 0x1FF, vofs = rd16(e->io + 0x12 + bg * 4) & 0x1FF;
    int size = cnt >> 14;
    u32 w = (size & 1) ? 512 : 256, h = (size & 2) ? 512 : 256;
    u32 cb = char_base(e, cnt), sb = screen_base(e, cnt);
    u32 vmask = e->bgvram_size - 1;
    const u8 *v = e->bgvram;
    int col256 = (cnt >> 7) & 1;
    const u8 *ext = NULL;
    if (col256 && (e->dispcnt & (1u << 30))) ext = ext_palette(e, (bg < 2 && (cnt & 0x2000)) ? bg + 2 : bg);
    u32 y = (line + vofs) & (h - 1);
    u32 rowbase = sb + ((y >> 8) * (w >> 8)) * 0x800 + ((y & 255) >> 3) * 64;
    // tile by tile: one map entry and one row of pixel data per 8 pixels
    int x = -(int)(hofs & 7);
    u32 sx = hofs & ~7u;
    while (x < 256) {
        sx &= w - 1;
        u16 ent = rd16(v + ((rowbase + (sx >> 8) * 0x800 + ((sx & 255) >> 3) * 2) & vmask));
        u32 tile = ent & 0x3FF, ty = (ent & 0x800) ? 7 - (y & 7) : (y & 7);
        int hf = (ent >> 10) & 1;
        u16 px[8];
        if (col256) {
            const u8 *t = v + ((cb + tile * 64 + ty * 8) & vmask);
            const u8 *pal = ext ? ext + (ent >> 12) * 512 : NULL;
            for (int i = 0; i < 8; i++) {
                u8 idx = t[hf ? 7 - i : i];
                px[i] = !idx ? 0 : (pal ? (rd16(pal + idx * 2) | OPAQUE) : e->bgp[idx]);
            }
        } else {
            u32 row = rd32(v + ((cb + tile * 32 + ty * 4) & vmask));
            const u16 *pal = e->bgp + (ent >> 12) * 16;
            for (int i = 0; i < 8; i++) {
                u32 idx = (row >> ((hf ? 7 - i : i) * 4)) & 15;
                px[i] = idx ? pal[idx] : 0;
            }
        }
        for (int i = 0; i < 8; i++, x++) {
            if ((unsigned)x < 256) out[x] = px[i];
        }
        sx += 8;
    }
}

// ---------------------------------------------------------------- affine and extended BGs (2 and 3)
static s32 sAffX[2][2], sAffY[2][2];  // [engine][bg-2]: the internal reference point

static void affine_latch(const Engine *e) {
    for (int i = 0; i < 2; i++) {
        const u8 *r = e->io + 0x20 + i * 0x10;
        s32 x = (s32)(rd32(r + 8) << 4) >> 4, y = (s32)(rd32(r + 12) << 4) >> 4;
        sAffX[!e->main][i] = x;
        sAffY[!e->main][i] = y;
    }
}

static void bg_affine(const Engine *e, int bg, int line, u16 *out, int extended) {
    (void)line;
    u16 cnt = rd16(e->io + 8 + bg * 2);
    const u8 *r = e->io + 0x20 + (bg - 2) * 0x10;
    s32 pa = (int16_t)rd16(r), pc = (int16_t)rd16(r + 4);
    s32 pb = (int16_t)rd16(r + 2), pd = (int16_t)rd16(r + 6);
    s32 *rx = &sAffX[!e->main][bg - 2], *ry = &sAffY[!e->main][bg - 2];
    s32 x = *rx, y = *ry;
    *rx += pb;
    *ry += pd;
    int wrap = (cnt >> 13) & 1;
    int size = cnt >> 14;
    if (!extended) {
        // rotation/scaling: 8-bit map, 256-colour tiles
        u32 dim = 128u << size;
        u32 cb = char_base(e, cnt), sb = screen_base(e, cnt);
        for (int i = 0; i < 256; i++, x += pa, y += pc) {
            s32 tx = x >> 8, ty = y >> 8;
            if (wrap) { tx &= dim - 1; ty &= dim - 1; }
            else if ((u32)tx >= dim || (u32)ty >= dim) { out[i] = 0; continue; }
            u8 tile = bgv8(e, sb + (ty >> 3) * (dim >> 3) + (tx >> 3));
            u8 idx = bgv8(e, cb + tile * 64 + (ty & 7) * 8 + (tx & 7));
            out[i] = idx ? (rd16(e->bgpal + idx * 2) | OPAQUE) : 0;
        }
        return;
    }
    if (!(cnt & 0x80)) {
        // extended, 16-bit map (tiles as in text BGs, 256 colours)
        u32 dim = 128u << size;
        u32 cb = char_base(e, cnt), sb = screen_base(e, cnt);
        const u8 *ext = (e->dispcnt & (1u << 30)) ? ext_palette(e, bg) : NULL;
        for (int i = 0; i < 256; i++, x += pa, y += pc) {
            s32 tx = x >> 8, ty = y >> 8;
            if (wrap) { tx &= dim - 1; ty &= dim - 1; }
            else if ((u32)tx >= dim || (u32)ty >= dim) { out[i] = 0; continue; }
            u16 ent = bgv16(e, sb + ((ty >> 3) * (dim >> 3) + (tx >> 3)) * 2);
            u32 px = tx & 7, py = ty & 7;
            if (ent & 0x400) px = 7 - px;
            if (ent & 0x800) py = 7 - py;
            u8 idx = bgv8(e, cb + (ent & 0x3FF) * 64 + py * 8 + px);
            if (!idx) { out[i] = 0; continue; }
            u16 c = ext ? rd16(ext + ((ent >> 12) * 256 + idx) * 2) : rd16(e->bgpal + idx * 2);
            out[i] = c | OPAQUE;
        }
        return;
    }
    // extended bitmap: 256 colours or direct colour
    static const u16 kW[4] = { 128, 256, 512, 512 }, kH[4] = { 128, 256, 256, 512 };
    u32 w = kW[size], h = kH[size];
    u32 base = ((cnt >> 8) & 0x1F) * 0x4000;
    int direct = (cnt >> 2) & 1;
    for (int i = 0; i < 256; i++, x += pa, y += pc) {
        s32 tx = x >> 8, ty = y >> 8;
        if (wrap) { tx &= w - 1; ty &= h - 1; }
        else if ((u32)tx >= w || (u32)ty >= h) { out[i] = 0; continue; }
        if (direct) {
            u16 c = bgv16(e, base + (ty * w + tx) * 2);
            out[i] = (c & 0x8000) ? c : 0;
        } else {
            u8 idx = bgv8(e, base + ty * w + tx);
            out[i] = idx ? (rd16(e->bgpal + idx * 2) | OPAQUE) : 0;
        }
    }
}

// ---------------------------------------------------------------- sprites
static const u8 kObjW[3][4] = { { 8, 16, 32, 64 }, { 16, 32, 32, 64 }, { 8, 8, 16, 32 } };
static const u8 kObjH[3][4] = { { 8, 16, 32, 64 }, { 8, 8, 16, 32 }, { 16, 32, 32, 64 } };

static void sprites(const Engine *e, int line) {
    static int dirty = 1;
    if (dirty) {
        memset(sObj, 0, sizeof(sObj));
        memset(sObjPrio, 4, sizeof(sObjPrio));
        memset(sObjSemi, 0, sizeof(sObjSemi));
        memset(sObjWin, 0, sizeof(sObjWin));
        dirty = 0;
    }
    if (!(e->dispcnt & 0x1000) && !(e->dispcnt & 0x8000)) return;
    if (!e->nobj) return;
    dirty = 1;
    int map1d = (e->dispcnt >> 4) & 1;
    u32 bshift = e->main ? (e->dispcnt >> 20) & 3 : ((e->dispcnt >> 20) & 3 > 2 ? 2 : (e->dispcnt >> 20) & 3);
    const u8 *ext = (e->dispcnt & (1u << 31)) ? ext_palette(e, -1) : NULL;
    for (int n = 0; n < e->nobj; n++) {
        int i = e->objlist[n];
        const u8 *o = e->oam + i * 8;
        u16 a0 = rd16(o), a1 = rd16(o + 2), a2 = rd16(o + 4);
        int affine = (a0 >> 8) & 1;
        if (!affine && (a0 & 0x200)) continue;  // hidden
        int mode = (a0 >> 10) & 3;
        if (mode == 3) continue;  // bitmap sprites: not yet
        int shape = a0 >> 14, sz = a1 >> 14;
        if (shape == 3) continue;
        int w = kObjW[shape][sz], h = kObjH[shape][sz];
        int bw = w, bh = h;
        if (affine && (a0 & 0x200)) { bw *= 2; bh *= 2; }
        int y = a0 & 0xFF;
        int ly = (line - y) & 0xFF;
        if (ly >= bh) continue;
        int x = a1 & 0x1FF;
        if (x >= 256) x -= 512;
        int col256 = (a0 >> 13) & 1;
        u32 tile = a2 & 0x3FF;
        int prio = (a2 >> 10) & 3;
        int pal = a2 >> 12;
        s32 pa = 256, pb = 0, pc = 0, pd = 256;
        if (affine) {
            const u8 *p = e->oam + ((a1 >> 9) & 31) * 32;
            pa = (int16_t)rd16(p + 6); pb = (int16_t)rd16(p + 14);
            pc = (int16_t)rd16(p + 22); pd = (int16_t)rd16(p + 30);
        }
        for (int px = 0; px < bw; px++) {
            int sx = x + px;
            if (sx < 0 || sx >= 256) continue;
            int tx, ty;
            if (affine) {
                int dx = px - bw / 2, dy = ly - bh / 2;
                tx = ((pa * dx + pb * dy) >> 8) + w / 2;
                ty = ((pc * dx + pd * dy) >> 8) + h / 2;
                if (tx < 0 || ty < 0 || tx >= w || ty >= h) continue;
            } else {
                tx = (a1 & 0x1000) ? w - 1 - px : px;
                ty = (a1 & 0x2000) ? h - 1 - ly : ly;
            }
            u32 addr;
            if (col256) {
                if (map1d) addr = (tile << (5 + bshift)) + ((ty >> 3) * (w >> 3) + (tx >> 3)) * 64 + (ty & 7) * 8 + (tx & 7);
                else addr = tile * 32 + (ty >> 3) * 1024 + (tx >> 3) * 64 + (ty & 7) * 8 + (tx & 7);
            } else {
                if (map1d) addr = (tile << (5 + bshift)) + ((ty >> 3) * (w >> 3) + (tx >> 3)) * 32 + (ty & 7) * 4 + ((tx & 7) >> 1);
                else addr = tile * 32 + (ty >> 3) * 1024 + (tx >> 3) * 32 + (ty & 7) * 4 + ((tx & 7) >> 1);
            }
            u8 idx = objv8(e, addr);
            if (!col256) idx = (tx & 1) ? idx >> 4 : idx & 15;
            if (!idx) continue;
            if (mode == 2) { sObjWin[sx] = 1; continue; }
            if (prio >= sObjPrio[sx]) continue;
            u16 c;
            if (col256) c = ext ? (rd16(ext + (pal * 256 + idx) * 2) | OPAQUE) : e->objp[idx];
            else c = e->objp[pal * 16 + idx];
            sObj[sx] = c;
            sObjPrio[sx] = (u8)prio;
            sObjSemi[sx] = mode == 1;
        }
    }
}

// ---------------------------------------------------------------- composition
static inline u16 blend_alpha(u16 a, u16 b, int eva, int evb) {
    int r = ((a & 31) * eva + (b & 31) * evb) >> 4;
    int g = (((a >> 5) & 31) * eva + ((b >> 5) & 31) * evb) >> 4;
    int bl = (((a >> 10) & 31) * eva + ((b >> 10) & 31) * evb) >> 4;
    if (r > 31) r = 31;
    if (g > 31) g = 31;
    if (bl > 31) bl = 31;
    return (u16)(r | g << 5 | bl << 10);
}
static inline u16 brighten(u16 c, int y) {
    int r = c & 31, g = (c >> 5) & 31, b = (c >> 10) & 31;
    r += ((31 - r) * y) >> 4; g += ((31 - g) * y) >> 4; b += ((31 - b) * y) >> 4;
    return (u16)(r | g << 5 | b << 10);
}
static inline u16 darken(u16 c, int y) {
    int r = c & 31, g = (c >> 5) & 31, b = (c >> 10) & 31;
    r -= (r * y) >> 4; g -= (g * y) >> 4; b -= (b * y) >> 4;
    return (u16)(r | g << 5 | b << 10);
}

uint64_t port_ctr_ticks(void);
uint64_t gProfBg, gProfObj, gProfMix;
static int in_range(int v, int lo, int hi) { return lo <= hi ? (v >= lo && v < hi) : (v >= lo || v < hi); }

static void render_engine(Engine *e, u16 *out) {
    e->dispcnt = rd32(e->io);
    int dmode = (e->dispcnt >> 16) & 3;
    if (!e->main && dmode > 1) dmode = 1;
    if (dmode == 0 || (e->dispcnt & 0x80)) {  // display off / forced blank: white
        for (int i = 0; i < 256 * 192; i++) out[i] = 0x7FFF;
        return;
    }
    if (dmode == 2) {  // VRAM display: a bank as a 256x192 direct-colour picture
        const u8 *src = DS(kBankLcdc[(e->dispcnt >> 18) & 3]);
        for (int i = 0; i < 256 * 192; i++) out[i] = rd16(src + i * 2) & 0x7FFF;
        goto master;
    }
    if (dmode == 3) {
        memset(out, 0, 256 * 192 * 2);
        goto master;
    }
    {
        int bgmode = e->dispcnt & 7;
        affine_latch(e);
        for (int i = 0; i < 256; i++) {
            e->bgp[i] = rd16(e->bgpal + i * 2) | OPAQUE;
            e->objp[i] = rd16(e->objpal + i * 2) | OPAQUE;
        }
        e->nobj = 0;
        for (int i = 0; i < 128; i++) {
            u16 a0 = rd16(e->oam + i * 8);
            if (!(a0 & 0x100) && (a0 & 0x200)) continue;
            if (((a0 >> 10) & 3) == 3 || (a0 >> 14) == 3) continue;
            e->objlist[e->nobj++] = (u8)i;
        }
        u16 bgcnt[4];
        for (int i = 0; i < 4; i++) bgcnt[i] = rd16(e->io + 8 + i * 2);
        u16 bldcnt = rd16(e->io + 0x50), bldalpha = rd16(e->io + 0x52);
        int eva = bldalpha & 31, evb = (bldalpha >> 8) & 31, evy = rd16(e->io + 0x54) & 31;
        if (eva > 16) eva = 16;
        if (evb > 16) evb = 16;
        if (evy > 16) evy = 16;
        int effect = (bldcnt >> 6) & 3;
        u16 win0h = rd16(e->io + 0x40), win1h = rd16(e->io + 0x42), win0v = rd16(e->io + 0x44), win1v = rd16(e->io + 0x46);
        u16 winin = rd16(e->io + 0x48), winout = rd16(e->io + 0x4A);
        int windows = (e->dispcnt >> 13) & 7;
        u16 backdrop = rd16(e->bgpal) & 0x7FFF;

        for (int line = 0; line < 192; line++) {
            uint64_t t0 = port_ctr_ticks();
            int on[4] = { 0 };
            for (int bg = 0; bg < 4; bg++) {
                if (!(e->dispcnt & (0x100 << bg))) continue;
                if (bg == 0 && e->main && (e->dispcnt & 8)) continue;  // 3D layer: phase 2
                int kind;  // 0 text, 1 affine, 2 extended
                if (bg < 2) kind = 0;
                else if (bg == 2) kind = (bgmode == 2 || bgmode == 4) ? 1 : (bgmode == 5 ? 2 : (bgmode <= 1 || bgmode == 3 ? 0 : -1));
                else kind = (bgmode == 0) ? 0 : (bgmode <= 2 ? 1 : (bgmode <= 5 ? 2 : -1));
                if (kind < 0) continue;
                if (kind == 0) bg_text(e, bg, line, sBg[bg]);
                else bg_affine(e, bg, line, sBg[bg], kind == 2);
                on[bg] = 1;
            }
            uint64_t t1 = port_ctr_ticks();
            sprites(e, line);
            uint64_t t2 = port_ctr_ticks();
            gProfBg += t1 - t0; gProfObj += t2 - t1;
            int objon = (e->dispcnt >> 12) & 1;
            int w0in = (windows & 1) && in_range(line, win0v >> 8, win0v & 0xFF);
            int w1in = (windows & 2) && in_range(line, win1v >> 8, win1v & 0xFF);
            u16 *row = out + line * 256;
            // Fast path: no windows and no colour effect on this line: back to front, opaque wins.
            int anysemi = 0;
            if (objon) for (int x = 0; x < 256; x++) anysemi |= sObjSemi[x];
            if (!windows && effect == 0 && !anysemi) {
                for (int x = 0; x < 256; x++) row[x] = backdrop;
                for (int p = 3; p >= 0; p--) {
                    for (int bg = 3; bg >= 0; bg--) {
                        if (!on[bg] || (bgcnt[bg] & 3) != p) continue;
                        const u16 *src = sBg[bg];
                        for (int x = 0; x < 256; x++) if (src[x] & OPAQUE) row[x] = src[x] & 0x7FFF;
                    }
                    if (objon) {
                        for (int x = 0; x < 256; x++) if ((sObj[x] & OPAQUE) && sObjPrio[x] == p) row[x] = sObj[x] & 0x7FFF;
                    }
                }
                gProfMix += port_ctr_ticks() - t2;
                continue;
            }
            int order[8], orderPrio[8], norder = 0;
            for (int p = 0; p < 4; p++) {
                if (objon) { order[norder] = 4; orderPrio[norder++] = p; }
                for (int bg = 0; bg < 4; bg++) {
                    if (on[bg] && (bgcnt[bg] & 3) == p) { order[norder] = bg; orderPrio[norder++] = p; }
                }
            }
            // window masks for the line
            static u8 wm[256];
            if (windows) {
                memset(wm, winout & 0x3F, 256);
                if (windows & 4) for (int x = 0; x < 256; x++) if (sObjWin[x]) wm[x] = (winout >> 8) & 0x3F;
                if (w1in) for (int x = 0; x < 256; x++) if (in_range(x, win1h >> 8, win1h & 0xFF)) wm[x] = (winin >> 8) & 0x3F;
                if (w0in) for (int x = 0; x < 256; x++) if (in_range(x, win0h >> 8, win0h & 0xFF)) wm[x] = winin & 0x3F;
            } else {
                memset(wm, 0x3F, 256);
            }
            // the two front-most layers per pixel, built back to front
            static u16 c1[256], c2[256];
            static u8 l1[256], l2[256];
            for (int x = 0; x < 256; x++) { c1[x] = c2[x] = backdrop; l1[x] = l2[x] = 5; }
            for (int k = norder - 1; k >= 0; k--) {
                int l = order[k];
                if (l == 4) {
                    u8 p = (u8)orderPrio[k];
                    for (int x = 0; x < 256; x++) {
                        if ((sObj[x] & OPAQUE) && sObjPrio[x] == p && (wm[x] & 0x10)) {
                            c2[x] = c1[x]; l2[x] = l1[x]; c1[x] = sObj[x] & 0x7FFF; l1[x] = 4;
                        }
                    }
                } else {
                    const u16 *src = sBg[l];
                    u8 bit = (u8)(1 << l);
                    for (int x = 0; x < 256; x++) {
                        if ((src[x] & OPAQUE) && (wm[x] & bit)) {
                            c2[x] = c1[x]; l2[x] = l1[x]; c1[x] = src[x] & 0x7FFF; l1[x] = (u8)l;
                        }
                    }
                }
            }
            // colour effects
            u16 first = bldcnt & 0x3F, second = (bldcnt >> 8) & 0x3F;
            for (int x = 0; x < 256; x++) {
                u16 c = c1[x];
                int a = l1[x], b = l2[x];
                if (a == 4 && sObjSemi[x] && (second & (1 << b))) {
                    c = blend_alpha(c, c2[x], eva, evb);
                } else if (effect && (wm[x] & 0x20) && (first & (1 << a))) {
                    if (effect == 1) { if (second & (1 << b)) c = blend_alpha(c, c2[x], eva, evb); }
                    else if (effect == 2) c = brighten(c, evy);
                    else c = darken(c, evy);
                }
                row[x] = c;
            }
            gProfMix += port_ctr_ticks() - t2;
        }
    }
master: {
        u16 mb = rd16(e->io + 0x6C);
        int f = mb & 31, m = (mb >> 14) & 3;
        if (f > 16) f = 16;
        if (f && (m == 1 || m == 2)) {
            for (int i = 0; i < 256 * 192; i++) out[i] = m == 1 ? brighten(out[i], f) : darken(out[i], f);
        }
    }
}

// ---------------------------------------------------------------- frames, on a core of their own
// The game's display state is copied at the VBlank (as the DS would scan it out during the next
// frame), and a render thread draws that copy while the game runs its next frame.
typedef struct {
    u8 io[0x1100];       // 0x04000000-0x040010FF
    u8 pal[0x800];       // 0x05000000
    u8 oam[0x800];       // 0x07000000
    u8 bga[0x80000];     // 0x06000000
    u8 bgb[0x20000];     // 0x06200000
    u8 obja[0x40000];    // 0x06400000
    u8 objb[0x20000];    // 0x06600000
    u8 ext[0x24000];     // 0x06880000: banks E-I (extended palettes)
} Snapshot;

static Snapshot *sSnap;

static void snapshot(Snapshot *s) {
    memcpy(s->io, DS(0x04000000), sizeof(s->io));
    memcpy(s->pal, DS(0x05000000), sizeof(s->pal));
    memcpy(s->oam, DS(0x07000000), sizeof(s->oam));
    memcpy(s->bga, DS(0x06000000), sizeof(s->bga));
    memcpy(s->bgb, DS(0x06200000), sizeof(s->bgb));
    memcpy(s->obja, DS(0x06400000), sizeof(s->obja));
    memcpy(s->objb, DS(0x06600000), sizeof(s->objb));
    memcpy(s->ext, DS(0x06880000), sizeof(s->ext));
}

// Both engines; `top`/`bottom` follow the screen swap (POWCNT1 bit 15: engine A on the top screen).
static void render_snapshot(const Snapshot *sn, u16 *top, u16 *bottom) {
    static Engine a, b;
    sIoBase = sn->io;
    sExtBase = sn->ext;
    a.main = 1; a.io = sn->io; a.bgpal = sn->pal; a.objpal = sn->pal + 0x200; a.oam = sn->oam;
    a.bgvram = sn->bga; a.bgvram_size = 0x80000; a.objvram = sn->obja; a.objvram_size = 0x40000;
    b.main = 0; b.io = sn->io + 0x1000; b.bgpal = sn->pal + 0x400; b.objpal = sn->pal + 0x600; b.oam = sn->oam + 0x400;
    b.bgvram = sn->bgb; b.bgvram_size = 0x20000; b.objvram = sn->objb; b.objvram_size = 0x20000;
    int a_on_top = (rd16(sn->io + 0x304) >> 15) & 1;
    render_engine(&a, a_on_top ? top : bottom);
    render_engine(&b, a_on_top ? bottom : top);
}

void port_render2d(u16 *top, u16 *bottom) {
    if (!sSnap) sSnap = malloc(sizeof(Snapshot));
    snapshot(sSnap);
    render_snapshot(sSnap, top, bottom);
}

// Threaded: port_render2d_begin() at the VBlank copies the state and starts the drawing; the
// previous frame's picture (if it's done) comes back from port_render2d_finished().
static Snapshot *sSnapT;
static u16 sOut[2][2][256 * 192];  // [buffer][top/bottom]
static volatile int sWork, sDone = 1, sOutReady = -1;
static int sCur;
int port_ctr_thread_start(void (*fn)(void *), int stack);
void port_ctr_wait_signal(volatile int *flag, int value);

static void render_thread(void *arg) {
    (void)arg;
    for (;;) {
        port_ctr_wait_signal(&sWork, 1);
        sWork = 0;
        render_snapshot(sSnapT, sOut[sCur][0], sOut[sCur][1]);
        sOutReady = sCur;
        __sync_synchronize();
        sDone = 1;
    }
}

static int sThreaded = -1;
int port_render2d_begin(void) {
    if (sThreaded < 0) {
        sSnapT = malloc(sizeof(Snapshot));
        sThreaded = sSnapT && port_ctr_thread_start(render_thread, 64 * 1024);
        port_log("2D renderer: %s\n", sThreaded ? "on its own core" : "on the game's core");
    }
    if (!sThreaded) return 0;
    while (!sDone) {}                 // (the previous frame is still being drawn)
    snapshot(sSnapT);
    sCur ^= 1;
    sDone = 0;
    __sync_synchronize();
    sWork = 1;
    return 1;
}

// The last finished picture: index into sOut, or -1.
const u16 *port_render2d_picture(int screen) {
    int r = sOutReady;
    return r < 0 ? NULL : sOut[r][screen];
}

// A 24-bit BMP of both screens (top above bottom) on the SD card: for checking the renderer.
#include <stdio.h>
void port_dump_screens(const u16 *top, const u16 *bottom, u32 frame) {
    char path[96];
    snprintf(path, sizeof(path), "sdmc:/3ds/soulsilver-3ds/shot_%05lu.bmp", (unsigned long)frame);
    FILE *f = fopen(path, "wb");
    if (!f) return;
    const int w = 256, h = 384, rowb = w * 3, size = 54 + rowb * h;
    u8 hd[54] = { 'B', 'M' };
    hd[2] = size; hd[3] = size >> 8; hd[4] = size >> 16; hd[10] = 54; hd[14] = 40;
    hd[18] = w; hd[19] = w >> 8; hd[22] = h; hd[23] = h >> 8; hd[26] = 1; hd[28] = 24;
    fwrite(hd, 1, 54, f);
    static u8 row[256 * 3];
    for (int y = h - 1; y >= 0; y--) {
        const u16 *src = y < 192 ? top + y * 256 : bottom + (y - 192) * 256;
        for (int x = 0; x < w; x++) {
            u16 c = src[x];
            row[x * 3 + 0] = (u8)(((c >> 10) & 31) << 3);
            row[x * 3 + 1] = (u8)(((c >> 5) & 31) << 3);
            row[x * 3 + 2] = (u8)((c & 31) << 3);
        }
        fwrite(row, 1, rowb, f);
    }
    fclose(f);
}

// One engine from live memory (the GPU compositor's fallback for VRAM / main-memory display).
void port_render2d_engine_sw(int engine, u16 *out) {
    static Snapshot *sn;
    if (!sn) sn = malloc(sizeof(Snapshot));
    if (!sn) return;
    snapshot(sn);
    static Engine e;
    sIoBase = sn->io;
    sExtBase = sn->ext;
    if (engine == 0) {
        e.main = 1; e.io = sn->io; e.bgpal = sn->pal; e.objpal = sn->pal + 0x200; e.oam = sn->oam;
        e.bgvram = sn->bga; e.bgvram_size = 0x80000; e.objvram = sn->obja; e.objvram_size = 0x40000;
    } else {
        e.main = 0; e.io = sn->io + 0x1000; e.bgpal = sn->pal + 0x400; e.objpal = sn->pal + 0x600; e.oam = sn->oam + 0x400;
        e.bgvram = sn->bgb; e.bgvram_size = 0x20000; e.objvram = sn->objb; e.objvram_size = 0x20000;
    }
    render_engine(&e, out);
}
