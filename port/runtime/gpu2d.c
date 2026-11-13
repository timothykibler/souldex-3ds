// The DS's two 2D engines, composited by the 3DS GPU (citro3d).
//
// Backgrounds become textures: a DS 8x8 tile is exactly one PICA 8x8 texture tile, so decoding is
// a copy through the palette. A background's texture is rebuilt only when its map, tiles or palette
// change (a hash per frame); scrolling, rotation and scaling are texture coordinates. Sprites go
// into a sprite sheet each frame. Layers are drawn back to front in DS priority order; the stencil
// buffer holds the window region of each pixel (bits 0-1) and whether the pixel so far belongs to
// a second blend target (bit 2), which is what DS colour effects need:
//   alpha blending  src * EVA + dst * EVB via the blend constant (rgb = EVA, alpha = EVB)
//   brighten/darken in the texture combiner; master brightness as a last full-screen quad.
// Each engine is drawn into a 256x256 texture, then 1:1 onto its 3DS screen.
//
// Not drawn by the GPU (the software renderer, render2d.c, does those frames): VRAM display mode,
// main-memory display mode. Not yet anywhere: mosaic, bitmap sprites, the 3D layer (phase 2),
// mid-frame (H-blank) effects.
#include <3ds.h>
#include <citro3d.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "port_ds.h"
#include "port.h"
#include "gx3d.h"

#define DS(a) ((u8 *)(uintptr_t)((a) + PORT_DS_DELTA))
static inline u16 rd16(const u8 *p) { return (u16)(p[0] | p[1] << 8); }
static inline u32 rd32(const u8 *p) { return (u32)(p[0] | p[1] << 8 | p[2] << 16 | (u32)p[3] << 24); }

extern const u8 quad_shbin[];
extern const u32 quad_shbin_size;

static DVLB_s *sDvlb;
static shaderProgram_s sProg;
static int sProjLoc;
static C3D_RenderTarget *sTop, *sBottom;

// ---------------------------------------------------------------- vertices
typedef struct { float x, y, u, v; } Vtx;
#define MAX_VTX (64 * 1024)
static Vtx *sVbo;
static int sVtxUsed;
static float sOX, sOY;  // (offset added to every quad: 0, the projection places the DS picture)

// Screen layout. The DS picture is shown 1:1 (exact pixels) in the middle of each screen. On the
// top screen the 3D layer can be widened to fill all of it (400x240): the DS frustum is extended
// on both axes (clip x scaled by 256/400, y by 192/240), so what's inside the DS frame lands
// exactly where it would, at the same scale as the 2D, and the border shows more of the scene.
int port_wide_mode = 1;
int port_edge_mode = 1;    // edge marking (debug.txt "edge 0" turns it off)    // 0 never, 1 when the scene is 3D-backed, 2 always (debug.txt "wide N")
float port_g3_wide = 1.0f, port_g3_wide_y = 1.0f;  // view size over the DS's (the geometry engine's box test)
static const float sScale = 1.0f;
static float sL, sT, sW, sH;  // the screen being drawn, in DS pixels: left, top, width, height
static int sScrW;             // its width in screen pixels
static int sWide;             // this frame's 3D layer is widened
static void scissor_ds(int whole);
static void layout(int screenW);

static void quad(float x0, float y0, float x1, float y1, float u0, float v0, float u1, float v1) {
    if (sVtxUsed + 6 > MAX_VTX) return;
    Vtx *p = sVbo + sVtxUsed;
    x0 += sOX; x1 += sOX; y0 += sOY; y1 += sOY;
    p[0] = (Vtx){ x0, y0, u0, v0 }; p[1] = (Vtx){ x1, y0, u1, v0 }; p[2] = (Vtx){ x0, y1, u0, v1 };
    p[3] = (Vtx){ x1, y0, u1, v0 }; p[4] = (Vtx){ x1, y1, u1, v1 }; p[5] = (Vtx){ x0, y1, u0, v1 };
    C3D_DrawArrays(GPU_TRIANGLES, sVtxUsed, 6);
    sVtxUsed += 6;
}

// A quad with its four corners' texture coordinates given (for affine BGs and sprites).
static void quad4(const float xy[4][2], const float uv[4][2]) {
    if (sVtxUsed + 6 > MAX_VTX) return;
    Vtx *p = sVbo + sVtxUsed;
    static const int idx[6] = { 0, 1, 2, 1, 3, 2 };  // corners: 0 top-left, 1 top-right, 2 bottom-left, 3 bottom-right
    for (int i = 0; i < 6; i++) p[i] = (Vtx){ xy[idx[i]][0] + sOX, xy[idx[i]][1] + sOY, uv[idx[i]][0], uv[idx[i]][1] };
    C3D_DrawArrays(GPU_TRIANGLES, sVtxUsed, 6);
    sVtxUsed += 6;
}

// ---------------------------------------------------------------- textures in PICA layout
// RGBA5551 texels, 8x8 tiles in Morton order, tile rows bottom-up in memory. (Sampled with v = 0
// at the image's top row: checked against the software renderer.)
static u16 sColor[32768];  // DS BGR555 -> PICA RGBA5551 (opaque)
static u8 sMorton[64];      // (y & 7) * 8 + (x & 7) -> offset in the tile

static void tables_init(void) {
    for (int c = 0; c < 32768; c++) {
        int r = c & 31, g = (c >> 5) & 31, b = (c >> 10) & 31;
        sColor[c] = (u16)(r << 11 | g << 6 | b << 1 | 1);
    }
    for (int y = 0; y < 8; y++) {
        for (int x = 0; x < 8; x++) {
            int m = 0;
            for (int i = 0; i < 3; i++) m |= ((x >> i) & 1) << (2 * i) | ((y >> i) & 1) << (2 * i + 1);
            sMorton[y * 8 + x] = (u8)m;
        }
    }
}

// The tile (tx, ty) (counted from the top) of a W x H texture.
static inline u16 *tex_tile(u16 *data, int W, int H, int tx, int ty) {
    int row = (H >> 3) - 1 - ty;
    return data + (row * (W >> 3) + tx) * 64;
}
// One texel; y counted from the top.
static inline u16 *tex_px(u16 *data, int W, int H, int x, int y) {
    return tex_tile(data, W, H, x >> 3, y >> 3) + sMorton[(7 - (y & 7)) * 8 + (x & 7)];
}

// ---------------------------------------------------------------- DS state
typedef struct {
    int main;
    const u8 *io, *bgpal, *objpal, *oam, *bgv, *objv;
    u32 bgsize, objsize;
    u32 dispcnt;
} Eng;

static const u32 kBankLcdc[9] = { 0x06800000, 0x06820000, 0x06840000, 0x06860000, 0x06880000,
                                  0x06890000, 0x06894000, 0x06898000, 0x068A0000 };
static const u8 kBankCnt[9] = { 0x40, 0x41, 0x42, 0x43, 0x44, 0x45, 0x46, 0x48, 0x49 };
static u8 vramcnt(int b) { return DS(0x04000200)[kBankCnt[b]]; }

// Extended palette slot (BG 0-3, OBJ -1): see render2d.c.
static const u8 *ext_palette(const Eng *e, int slot) {
    if (e->main) {
        if (slot >= 0) {
            u8 c = vramcnt(4);
            if ((c & 0x80) && (c & 7) == 4) return DS(kBankLcdc[4]) + slot * 0x2000;
            for (int b = 5; b <= 6; b++) {
                c = vramcnt(b);
                if ((c & 0x80) && (c & 7) == 4) {
                    int first = (c >> 3) & 1 ? 2 : 0;
                    if (slot == first || slot == first + 1) return DS(kBankLcdc[b]) + (slot - first) * 0x2000;
                }
            }
        } else {
            u8 c = vramcnt(4);
            if ((c & 0x80) && (c & 7) == 5) return DS(kBankLcdc[4]);
            for (int b = 5; b <= 6; b++) {
                c = vramcnt(b);
                if ((c & 0x80) && (c & 7) == 5) return DS(kBankLcdc[b]);
            }
        }
    } else {
        if (slot >= 0) {
            u8 c = vramcnt(7);
            if ((c & 0x80) && (c & 3) == 2) return DS(kBankLcdc[7]) + slot * 0x2000;
        } else {
            u8 c = vramcnt(8);
            if ((c & 0x80) && (c & 3) == 3) return DS(kBankLcdc[8]);
        }
    }
    return NULL;
}

static u32 char_base(const Eng *e, u16 cnt) { return ((cnt >> 2) & 0xF) * 0x4000 + (e->main ? ((e->dispcnt >> 24) & 7) * 0x10000 : 0); }
static u32 screen_base(const Eng *e, u16 cnt) { return ((cnt >> 8) & 0x1F) * 0x800 + (e->main ? ((e->dispcnt >> 27) & 7) * 0x10000 : 0); }

static u32 hash_bytes(u32 h, const u8 *p, u32 n) {
    const u32 *w = (const u32 *)p;
    for (u32 i = 0; i < n / 4; i++) h = (h ^ w[i]) * 16777619u + (h >> 15);
    return h;
}
// (VRAM ranges wrap at the window's end)
static u32 hash_vram(u32 h, const u8 *v, u32 size, u32 addr, u32 n) {
    addr &= size - 1;
    if (addr + n <= size) return hash_bytes(h, v + addr, n);
    h = hash_bytes(h, v + addr, size - addr);
    return hash_bytes(h, v, n - (size - addr));
}

// ---------------------------------------------------------------- BG planes
typedef struct {
    C3D_Tex tex;
    int w, h, ready;
    u32 hash;
    // what the text plane was last built from, to rebuild only the tiles that changed
    u16 *map;        // map entries (w/8 * h/8)
    u8 *chars;       // tile data (1024 tiles)
    u16 pal[256];    // palette as converted
    u32 cb, sb;
    int bpp8;
    const u8 *ext;
} Plane;
static Plane sPlane[2][4];
extern u32 gGpu2dRebuilds;

static bool plane_alloc(Plane *p, int w, int h, bool wrap) {
    if (!p->ready || p->w != w || p->h != h) {
        if (p->ready) C3D_TexDelete(&p->tex);
        p->ready = 0;
        if (!C3D_TexInit(&p->tex, (u16)w, (u16)h, GPU_RGBA5551)) return false;
        p->w = w; p->h = h; p->ready = 1; p->hash = 0; p->cb = ~0u;
        C3D_TexSetFilter(&p->tex, GPU_NEAREST, GPU_NEAREST);
    }
    C3D_TexSetWrap(&p->tex, wrap ? GPU_REPEAT : GPU_CLAMP_TO_BORDER, wrap ? GPU_REPEAT : GPU_CLAMP_TO_BORDER);
    p->tex.border = 0;
    return true;
}

// One 8x8 tile into a texture tile: 4bpp or 8bpp, flips, palette (NULL palette rows: transparent 0).
static void put_tile(u16 *dst, const u8 *src, int bpp8, int hf, int vf, const u16 *pal16, const u8 *pal256) {
    for (int y = 0; y < 8; y++) {
        int sy = vf ? 7 - y : y;
        const u8 *row = src + sy * (bpp8 ? 8 : 4);
        const u8 *m = sMorton + (7 - y) * 8;
        for (int x = 0; x < 8; x++) {
            int sx = hf ? 7 - x : x;
            u32 idx = bpp8 ? row[sx] : ((row[sx >> 1] >> ((sx & 1) * 4)) & 15);
            u16 c = 0;
            if (idx) c = pal256 ? sColor[rd16(pal256 + idx * 2) & 0x7FFF] : pal16[idx];
            dst[m[x]] = c;
        }
    }
}

static u16 sPal[2][2][256];  // [engine][bg/obj]: this frame's palettes as RGBA5551

// Text BG: the whole map (256 or 512 square) into its plane.
static Plane *text_plane(const Eng *e, int bg) {
    u16 cnt = rd16(e->io + 8 + bg * 2);
    int size = cnt >> 14;
    int w = (size & 1) ? 512 : 256, h = (size & 2) ? 512 : 256;
    u32 cb = char_base(e, cnt), sb = screen_base(e, cnt);
    int bpp8 = (cnt >> 7) & 1;
    const u8 *ext = (bpp8 && (e->dispcnt & (1u << 30))) ? ext_palette(e, (bg < 2 && (cnt & 0x2000)) ? bg + 2 : bg) : NULL;
    u32 mapbytes = (u32)(w / 256) * (h / 256) * 0x800;
    // hash: the map, the tiles it can reach, the palette
    u32 hsh = hash_vram(2166136261u ^ cnt, e->bgv, e->bgsize, sb, mapbytes);
    u32 maxtile = 0;
    for (u32 i = 0; i < mapbytes; i += 2) {
        u16 ent = rd16(e->bgv + ((sb + i) & (e->bgsize - 1)));
        if ((ent & 0x3FF) > maxtile) maxtile = ent & 0x3FF;
    }
    hsh = hash_vram(hsh, e->bgv, e->bgsize, cb, (maxtile + 1) * (bpp8 ? 64 : 32));
    hsh = ext ? hash_bytes(hsh, ext, 0x2000) : hash_bytes(hsh, (const u8 *)sPal[!e->main][0], 512);
    Plane *p = &sPlane[!e->main][bg];
    if (!plane_alloc(p, w, h, true)) return NULL;
    if (p->hash == hsh) return p;
    p->hash = hsh;
    gGpu2dRebuilds++;
    u16 *data = p->tex.data;
    int ntx = w / 8, nty = h / 8;
    u32 tilebytes = bpp8 ? 64 : 32;
    if (!p->map) {
        p->map = malloc(64 * 64 * 2);
        p->chars = malloc(1024 * 64);
        if (!p->map || !p->chars) return NULL;
        p->cb = ~0u;
    }
    // Full rebuild when the layout changed; otherwise only tiles whose entry, pixels or palette
    // row (16 colours; the whole palette for 256 colours) changed.
    const u16 *pal = sPal[!e->main][0];
    int full = p->cb != cb || p->sb != sb || p->bpp8 != bpp8 || p->ext != ext || ext;
    u16 rowChanged = 0;  // bit per 16-colour row
    for (int r = 0; r < 16; r++) if (memcmp(p->pal + r * 16, pal + r * 16, 32)) rowChanged |= (u16)(1 << r);
    if (bpp8 && rowChanged) full = 1;
    static u8 tileChanged[1024];
    for (u32 t = 0; t <= maxtile; t++) {
        const u8 *src = e->bgv + ((cb + t * tilebytes) & (e->bgsize - 1));
        tileChanged[t] = full || memcmp(p->chars + t * 64, src, tilebytes) != 0;
        if (tileChanged[t]) memcpy(p->chars + t * 64, src, tilebytes);
    }
    memcpy(p->pal, pal, sizeof(p->pal));
    p->cb = cb; p->sb = sb; p->bpp8 = bpp8; p->ext = ext;
    for (int ty = 0; ty < nty; ty++) {
        for (int tx = 0; tx < ntx; tx++) {
            u32 block = (u32)((ty >> 5) * (w >> 8) + (tx >> 5));
            u16 ent = rd16(e->bgv + ((sb + block * 0x800 + ((ty & 31) * 32 + (tx & 31)) * 2) & (e->bgsize - 1)));
            u16 *old = &p->map[ty * ntx + tx];
            u32 tile = ent & 0x3FF;
            if (!full && *old == ent && !tileChanged[tile] && !(!bpp8 && (rowChanged >> (ent >> 12)) & 1)) continue;
            *old = ent;
            const u8 *src = e->bgv + ((cb + tile * tilebytes) & (e->bgsize - 1));
            const u16 *pal16 = bpp8 ? pal : pal + (ent >> 12) * 16;
            const u8 *pal256 = ext ? ext + (ent >> 12) * 512 : NULL;
            put_tile(tex_tile(data, w, h, tx, ty), src, bpp8, (ent >> 10) & 1, (ent >> 11) & 1, pal16, pal256);
        }
    }
    C3D_TexFlush(&p->tex);
    return p;
}

// Affine / extended BG: its plane, and whether it wraps.
static Plane *affine_plane(const Eng *e, int bg, int extended, int *wrapOut) {
    u16 cnt = rd16(e->io + 8 + bg * 2);
    int size = cnt >> 14;
    int wrap = (cnt >> 13) & 1;
    *wrapOut = wrap;
    u32 cb = char_base(e, cnt), sb = screen_base(e, cnt);
    Plane *p = &sPlane[!e->main][bg];
    u32 hsh = 2166136261u ^ cnt ^ (extended << 16);
    if (!extended || !(cnt & 0x80)) {
        int dim = 128 << size;
        int m16 = extended;  // extended: 16-bit map entries (flips, ext palettes); else 8-bit
        const u8 *ext = (m16 && (e->dispcnt & (1u << 30))) ? ext_palette(e, bg) : NULL;
        u32 mapbytes = (u32)(dim / 8) * (dim / 8) * (m16 ? 2 : 1);
        hsh = hash_vram(hsh, e->bgv, e->bgsize, sb, mapbytes);
        hsh = hash_vram(hsh, e->bgv, e->bgsize, cb, m16 ? 1024 * 64 : 256 * 64);
        hsh = ext ? hash_bytes(hsh, ext, 0x2000) : hash_bytes(hsh, (const u8 *)sPal[!e->main][0], 512);
        if (!plane_alloc(p, dim, dim, wrap)) return NULL;
        if (p->hash == hsh) return p;
        p->hash = hsh;
        for (int ty = 0; ty < dim / 8; ty++) {
            for (int tx = 0; tx < dim / 8; tx++) {
                u16 ent = m16 ? rd16(e->bgv + ((sb + (ty * (dim / 8) + tx) * 2) & (e->bgsize - 1)))
                              : e->bgv[(sb + ty * (dim / 8) + tx) & (e->bgsize - 1)];
                u32 tile = m16 ? (ent & 0x3FF) : ent;
                const u8 *src = e->bgv + ((cb + tile * 64) & (e->bgsize - 1));
                put_tile(tex_tile(p->tex.data, dim, dim, tx, ty), src, 1, m16 && ((ent >> 10) & 1), m16 && ((ent >> 11) & 1),
                         sPal[!e->main][0], ext ? ext + (ent >> 12) * 512 : NULL);
            }
        }
    } else {
        static const int kW[4] = { 128, 256, 512, 512 }, kH[4] = { 128, 256, 256, 512 };
        int w = kW[size], h = kH[size];
        u32 base = ((cnt >> 8) & 0x1F) * 0x4000;
        int direct = (cnt >> 2) & 1;
        hsh = hash_vram(hsh, e->bgv, e->bgsize, base, (u32)w * h * (direct ? 2 : 1));
        if (!direct) hsh = hash_bytes(hsh, (const u8 *)sPal[!e->main][0], 512);
        if (!plane_alloc(p, w, h, wrap)) return NULL;
        if (p->hash == hsh) return p;
        p->hash = hsh;
        for (int y = 0; y < h; y++) {
            for (int x = 0; x < w; x++) {
                u16 c;
                if (direct) {
                    u16 v = rd16(e->bgv + ((base + (y * w + x) * 2) & (e->bgsize - 1)));
                    c = (v & 0x8000) ? sColor[v & 0x7FFF] : 0;
                } else {
                    u8 i = e->bgv[(base + y * w + x) & (e->bgsize - 1)];
                    c = i ? sPal[!e->main][0][i] : 0;
                }
                *tex_px(p->tex.data, w, h, x, y) = c;
            }
        }
    }
    C3D_TexFlush(&p->tex);
    return p;
}

// ---------------------------------------------------------------- sprite sheet (per frame)
#define SHEET 512
static C3D_Tex sSheet[2];
static int sShelfX, sShelfY, sShelfH;

static bool sheet_alloc(int e) {
    if (sSheet[e].data) return true;
    if (!C3D_TexInit(&sSheet[e], SHEET, SHEET, GPU_RGBA5551)) return false;
    C3D_TexSetFilter(&sSheet[e], GPU_NEAREST, GPU_NEAREST);
    C3D_TexSetWrap(&sSheet[e], GPU_CLAMP_TO_EDGE, GPU_CLAMP_TO_EDGE);
    return true;
}

static bool sheet_place(int w, int h, int *x, int *y) {
    if (sShelfX + w > SHEET) { sShelfX = 0; sShelfY += sShelfH; sShelfH = 0; }
    if (sShelfY + h > SHEET) return false;
    *x = sShelfX; *y = sShelfY;
    sShelfX += w;
    if (h > sShelfH) sShelfH = h;
    return true;
}

static const u8 kObjW[3][4] = { { 8, 16, 32, 64 }, { 16, 32, 32, 64 }, { 8, 8, 16, 32 } };
static const u8 kObjH[3][4] = { { 8, 16, 32, 64 }, { 8, 8, 16, 32 }, { 16, 32, 32, 64 } };

// Rotating/scaling sprites each get a texture of exactly their size, with a transparent border:
// the DS shows nothing outside a sprite's own image however far the matrix reaches.
static C3D_Tex sAffTex[2][128];

typedef struct {
    s16 x, y, bw, bh, w, h;   // screen position, bounding box, sprite size
    s16 sx, sy;               // its image in the sheet (top-left of the w x h image, inside its cell)
    u8 prio, mode, affine, idx;
    float m[4];               // affine: pa pb pc pd
    C3D_Tex *tex;             // affine: its own texture
} Spr;
static Spr sSpr[2][128];
static int sNumSpr[2];

static void sprites_build(const Eng *e) {
    int ei = !e->main;
    sNumSpr[ei] = 0;
    if (!(e->dispcnt & 0x1000) && !(e->dispcnt & 0x8000)) return;
    if (!sheet_alloc(ei)) return;
    sShelfX = sShelfY = sShelfH = 0;
    u16 *sheet = sSheet[ei].data;
    int map1d = (e->dispcnt >> 4) & 1;
    u32 bshift = (e->dispcnt >> 20) & 3;
    if (!e->main && bshift > 2) bshift = 2;
    const u8 *ext = (e->dispcnt & (1u << 31)) ? ext_palette(e, -1) : NULL;
    for (int i = 0; i < 128; i++) {
        const u8 *o = e->oam + i * 8;
        u16 a0 = rd16(o), a1 = rd16(o + 2), a2 = rd16(o + 4);
        int affine = (a0 >> 8) & 1;
        if (!affine && (a0 & 0x200)) continue;
        int mode = (a0 >> 10) & 3, shape = a0 >> 14;
        if (mode == 3 || shape == 3) continue;
        int w = kObjW[shape][a1 >> 14], h = kObjH[shape][a1 >> 14];
        int bw = w, bh = h;
        if (affine && (a0 & 0x200)) { bw *= 2; bh *= 2; }
        int y = a0 & 0xFF, x = a1 & 0x1FF;
        if (x >= 256) x -= 512;
        if (y + bh > 256) y -= 256;
        if (x + bw <= 0 || x >= 256 || y + bh <= 0 || y >= 192) continue;
        int cx = 0, cy = 0;
        u16 *dst = sheet;
        int dstW = SHEET, dstH = SHEET;
        C3D_Tex *atex = NULL;
        if (affine) {
            atex = &sAffTex[ei][sNumSpr[ei]];
            if (atex->data && (atex->width != w || atex->height != h)) { C3D_TexDelete(atex); atex->data = NULL; }
            if (!atex->data) {
                if (!C3D_TexInit(atex, (u16)w, (u16)h, GPU_RGBA5551)) continue;
                C3D_TexSetFilter(atex, GPU_NEAREST, GPU_NEAREST);
                C3D_TexSetWrap(atex, GPU_CLAMP_TO_BORDER, GPU_CLAMP_TO_BORDER);
                atex->border = 0;
            }
            dst = atex->data; dstW = w; dstH = h;
        } else if (!sheet_place(w, h, &cx, &cy)) {
            break;
        }
        int ix = cx, iy = cy;
        int bpp8 = (a0 >> 13) & 1;
        u32 tile = a2 & 0x3FF;
        int pal = a2 >> 12;
        int hf = !affine && ((a1 >> 12) & 1), vf = !affine && ((a1 >> 13) & 1);
        const u16 *pal16 = bpp8 ? sPal[ei][1] : sPal[ei][1] + pal * 16;
        const u8 *pal256 = (bpp8 && ext) ? ext + pal * 512 : NULL;
        for (int ty = 0; ty < h / 8; ty++) {
            for (int tx = 0; tx < w / 8; tx++) {
                u32 addr;
                if (map1d) addr = (tile << (5 + bshift)) + (u32)(ty * (w / 8) + tx) * (bpp8 ? 64 : 32);
                else addr = tile * 32 + (u32)ty * 1024 + (u32)tx * (bpp8 ? 64 : 32);
                const u8 *src = e->objv + (addr & (e->objsize - 1));
                int dx = hf ? w / 8 - 1 - tx : tx, dy = vf ? h / 8 - 1 - ty : ty;
                put_tile(tex_tile(dst, dstW, dstH, ix / 8 + dx, iy / 8 + dy), src, bpp8, hf, vf, pal16, pal256);
            }
        }
        Spr *s = &sSpr[ei][sNumSpr[ei]++];
        *s = (Spr){ (s16)x, (s16)y, (s16)bw, (s16)bh, (s16)w, (s16)h, (s16)ix, (s16)iy,
                    (u8)((a2 >> 10) & 3), (u8)mode, (u8)affine, (u8)i, { 1, 0, 0, 1 }, atex };
        if (atex) C3D_TexFlush(atex);
        if (affine) {
            const u8 *p = e->oam + ((a1 >> 9) & 31) * 32;
            s->m[0] = (int16_t)rd16(p + 6) / 256.0f; s->m[1] = (int16_t)rd16(p + 14) / 256.0f;
            s->m[2] = (int16_t)rd16(p + 22) / 256.0f; s->m[3] = (int16_t)rd16(p + 30) / 256.0f;
        }
    }
    if (sNumSpr[ei]) C3D_TexFlush(&sSheet[ei]);
}

// ---------------------------------------------------------------- GPU state helpers
enum { TEV_PLAIN, TEV_BRIGHTEN, TEV_DARKEN, TEV_FLAT };

static void set_tev(int mode, int evy) {
    C3D_TexEnv *env = C3D_GetTexEnv(0);
    C3D_TexEnvInit(env);
    C3D_TexEnv *env1 = C3D_GetTexEnv(1);
    C3D_TexEnvInit(env1);  // (stage 1 passes the previous stage through by default)
    u8 k = (u8)(255 - (evy * 255 + 8) / 16);  // 1 - EVY/16
    u32 kc = 0xFF000000u | k << 16 | k << 8 | k;
    switch (mode) {
    case TEV_PLAIN:
        C3D_TexEnvSrc(env, C3D_Both, GPU_TEXTURE0, 0, 0);
        C3D_TexEnvFunc(env, C3D_Both, GPU_REPLACE);
        break;
    case TEV_DARKEN:  // c * (1 - y)
        C3D_TexEnvSrc(env, C3D_RGB, GPU_TEXTURE0, GPU_CONSTANT, 0);
        C3D_TexEnvFunc(env, C3D_RGB, GPU_MODULATE);
        C3D_TexEnvSrc(env, C3D_Alpha, GPU_TEXTURE0, 0, 0);
        C3D_TexEnvFunc(env, C3D_Alpha, GPU_REPLACE);
        C3D_TexEnvColor(env, kc);
        break;
    case TEV_BRIGHTEN:  // c + (1 - c) * y = 1 - (1 - c)(1 - y)
        C3D_TexEnvSrc(env, C3D_RGB, GPU_TEXTURE0, GPU_CONSTANT, 0);
        C3D_TexEnvOpRgb(env, GPU_TEVOP_RGB_ONE_MINUS_SRC_COLOR, GPU_TEVOP_RGB_SRC_COLOR, GPU_TEVOP_RGB_SRC_COLOR);
        C3D_TexEnvFunc(env, C3D_RGB, GPU_MODULATE);
        C3D_TexEnvSrc(env, C3D_Alpha, GPU_TEXTURE0, 0, 0);
        C3D_TexEnvFunc(env, C3D_Alpha, GPU_REPLACE);
        C3D_TexEnvColor(env, kc);
        C3D_TexEnvSrc(env1, C3D_Both, GPU_PREVIOUS, 0, 0);
        C3D_TexEnvOpRgb(env1, GPU_TEVOP_RGB_ONE_MINUS_SRC_COLOR, GPU_TEVOP_RGB_SRC_COLOR, GPU_TEVOP_RGB_SRC_COLOR);
        C3D_TexEnvFunc(env1, C3D_Both, GPU_REPLACE);
        break;
    case TEV_FLAT:  // the constant colour (for master brightness)
        C3D_TexEnvSrc(env, C3D_Both, GPU_CONSTANT, 0, 0);
        C3D_TexEnvFunc(env, C3D_Both, GPU_REPLACE);
        C3D_TexEnvColor(env, (u32)evy);
        break;
    }
}

static void blend_off(void) {
    C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_ONE, GPU_ZERO, GPU_ONE, GPU_ZERO);
}
static void blend_alpha(int eva, int evb) {
    u8 a = (u8)((eva > 16 ? 16 : eva) * 255 / 16), b = (u8)((evb > 16 ? 16 : evb) * 255 / 16);
    C3D_BlendingColor((u32)b << 24 | (u32)a << 16 | (u32)a << 8 | a);
    C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_CONSTANT_COLOR, GPU_CONSTANT_ALPHA, GPU_ZERO, GPU_ONE);
}

// ---------------------------------------------------------------- one engine
typedef struct {
    int windows;         // DISPCNT bits 13-15
    u8 regionMask[4];    // layer enables per stencil region: 0 outside, 1 OBJ window, 2 WIN1, 3 WIN0
    u16 first, second;   // blend targets (BLDCNT)
    int effect, eva, evb, evy;
} Mix;

// Draws one layer with what the DS would do to it: per window region whose mask enables it, with
// its colour effect, keeping the "second target" stencil bit up to date. `draw` emits the quads.
static void draw_layer(const Mix *mx, int layerBit, int semi, void (*draw)(void *), void *arg) {
    int isFirst = (mx->first & layerBit) != 0, isSecond = (mx->second & layerBit) != 0;
    int nreg = mx->windows ? 4 : 1;
    for (int r = 0; r < nreg; r++) {
        u8 mask = mx->windows ? mx->regionMask[r] : 0x3F;
        int bit = layerBit == 0x10 ? 0x10 : layerBit;
        if (!(mask & bit)) continue;
        int fx = (mask & 0x20) ? mx->effect : 0;
        int regBits = mx->windows ? r : 0, regMask = mx->windows ? 3 : 0;
        int blends = semi || (fx == 1 && isFirst);
        if (blends) {
            // over a second target: blended; elsewhere: plain. Neither pass changes the stencil (the
            // second would see the first's changes); a third, colourless pass then marks the
            // layer's pixels as being (or not being) a second target.
            set_tev(TEV_PLAIN, 0);
            C3D_StencilOp(GPU_STENCIL_KEEP, GPU_STENCIL_KEEP, GPU_STENCIL_KEEP);
            blend_alpha(mx->eva, mx->evb);
            C3D_StencilTest(true, GPU_EQUAL, regBits | 4, regMask | 4, 0);
            draw(arg);
            blend_off();
            C3D_StencilTest(true, GPU_EQUAL, regBits, regMask | 4, 0);
            draw(arg);
            C3D_DepthTest(true, GPU_ALWAYS, GPU_WRITE_DEPTH);
            C3D_StencilTest(true, GPU_EQUAL, regBits | (isSecond ? 4 : 0), regMask, 4);
            C3D_StencilOp(GPU_STENCIL_KEEP, GPU_STENCIL_KEEP, GPU_STENCIL_REPLACE);
            draw(arg);
            C3D_DepthTest(true, GPU_ALWAYS, GPU_WRITE_ALL);
        } else {
            set_tev(isFirst && fx == 2 ? TEV_BRIGHTEN : isFirst && fx == 3 ? TEV_DARKEN : TEV_PLAIN, mx->evy);
            blend_off();
            C3D_StencilTest(true, GPU_EQUAL, regBits | (isSecond ? 4 : 0), regMask, 4);
            C3D_StencilOp(GPU_STENCIL_KEEP, GPU_STENCIL_KEEP, GPU_STENCIL_REPLACE);
            draw(arg);
        }
    }
}

typedef struct { const Eng *e; int bg; Plane *p; int kind; } BgDraw;
static C3D_Tex s3dTex;
static C3D_Tex sIdTex;                 // edge marking's polygon-ID image (see the 3D layer)
static C3D_RenderTarget *sIdTarget;
static int s3dHave;
static void render3d(int screenW);
static bool init3d(void);

static void bg_quads(void *arg) {
    BgDraw *d = arg;
    const Eng *e = d->e;
    if (d->kind == 3) {  // the 3D layer: rendered at the screen's own resolution, the whole screen
        C3D_TexBind(0, port_edge_mode >= 2 && sIdTarget ? &sIdTex : &s3dTex);
        if (sWide) scissor_ds(1);
        quad(sL, sT, sL + sW, sT + sH, 0, 0, sScrW / 512.0f, 240 / 256.0f);
        if (sWide) scissor_ds(0);
        return;
    }
    Plane *p = d->p;
    C3D_TexBind(0, &p->tex);
    if (d->kind == 0) {
        float hofs = rd16(e->io + 0x10 + d->bg * 4) & 0x1FF, vofs = rd16(e->io + 0x12 + d->bg * 4) & 0x1FF;
        quad(0, 0, 256, 192, hofs / p->w, vofs / p->h, (hofs + 256) / p->w, (vofs + 192) / p->h);
    } else {
        const u8 *r = e->io + 0x20 + (d->bg - 2) * 0x10;
        float pa = (int16_t)rd16(r) / 256.0f, pb = (int16_t)rd16(r + 2) / 256.0f;
        float pc = (int16_t)rd16(r + 4) / 256.0f, pd = (int16_t)rd16(r + 6) / 256.0f;
        float X = ((s32)(rd32(r + 8) << 4) >> 4) / 256.0f, Y = ((s32)(rd32(r + 12) << 4) >> 4) / 256.0f;
        static const float xy[4][2] = { { 0, 0 }, { 256, 0 }, { 0, 192 }, { 256, 192 } };
        float uv[4][2];
        for (int i = 0; i < 4; i++) {
            float sx = xy[i][0] - 0.5f, sy = xy[i][1] - 0.5f;  // (pixel centres: see the sprites)
            float tx = X + pa * sx + pb * sy, ty = Y + pc * sx + pd * sy;
            uv[i][0] = tx / p->w;
            uv[i][1] = ty / p->h;
        }
        quad4(xy, uv);
    }
}

typedef struct { int ei; const Spr *s; } SprDraw;
static void spr_quads(void *arg) {
    SprDraw *d = arg;
    const Spr *s = d->s;
    C3D_TexBind(0, &sSheet[d->ei]);
    const float S = SHEET;
    if (!s->affine) {
        quad(s->x, s->y, s->x + s->w, s->y + s->h, s->sx / S, s->sy / S, (s->sx + s->w) / S, (s->sy + s->h) / S);
        return;
    }
    C3D_TexBind(0, s->tex);
    float xy[4][2] = { { s->x, s->y }, { s->x + s->bw, s->y }, { s->x, s->y + s->bh }, { s->x + s->bw, s->y + s->bh } };
    float uv[4][2];
    for (int i = 0; i < 4; i++) {
        // (the GPU samples pixel centres; the DS steps from each pixel's corner: half a pixel back)
        float dx = xy[i][0] - 0.5f - (s->x + s->bw / 2.0f), dy = xy[i][1] - 0.5f - (s->y + s->bh / 2.0f);
        float tx = s->m[0] * dx + s->m[1] * dy + s->w / 2.0f, ty = s->m[2] * dx + s->m[3] * dy + s->h / 2.0f;
        uv[i][0] = tx / s->w;
        uv[i][1] = ty / s->h;
    }
    quad4(xy, uv);
}

static void fill(int x0, int y0, int x1, int y1) {
    if (x1 > x0 && y1 > y0) quad(x0, y0, x1, y1, 0, 0, 0, 0);
}
static void fill_range(int a, int b, int lo, int hi, int vertical) {
    // a DS window edge pair: [a, b) wraps when a > b
    if (a <= b) { if (vertical) fill(0, a, 256, b); else fill(a, lo, b, hi); }
    else {
        if (vertical) { fill(0, a, 256, 192); fill(0, 0, 256, b); }
        else { fill(a, lo, 256, hi); fill(0, lo, b, hi); }
    }
}

// Window rectangles into the stencil (region ids), drawn without touching colour.
static void windows_to_stencil(const Eng *e, int windows, int ei) {
    C3D_DepthTest(true, GPU_ALWAYS, GPU_WRITE_DEPTH);  // (stencil only: no colour writes)
    C3D_AlphaTest(true, GPU_GREATER, 0);
    set_tev(TEV_PLAIN, 0);
    C3D_StencilOp(GPU_STENCIL_KEEP, GPU_STENCIL_KEEP, GPU_STENCIL_REPLACE);
    if (windows & 4) {  // OBJ window: the opaque pixels of mode-2 sprites
        C3D_StencilTest(true, GPU_ALWAYS, 1, 0, 3);
        for (int n = 0; n < sNumSpr[ei]; n++) {
            if (sSpr[ei][n].mode != 2) continue;
            SprDraw d = { ei, &sSpr[ei][n] };
            spr_quads(&d);
        }
    }
    set_tev(TEV_FLAT, 0xFFFFFFFF);
    C3D_AlphaTest(false, GPU_ALWAYS, 0);
    for (int w = 1; w >= 0; w--) {
        if (!(windows & (1 << w))) continue;
        u16 h = rd16(e->io + 0x40 + w * 2), v = rd16(e->io + 0x44 + w * 2);
        C3D_StencilTest(true, GPU_ALWAYS, w == 0 ? 3 : 2, 0, 3);
        // inside = rows in [v>>8, v&255) and columns in [h>>8, h&255), each wrapping
        int y0 = v >> 8, y1 = v & 0xFF, x0 = h >> 8, x1 = h & 0xFF;
        int ys[2][2], ny = 0, xs[2][2], nx = 0;
        if (y0 <= y1) { ys[ny][0] = y0; ys[ny++][1] = y1; } else { ys[ny][0] = y0; ys[ny++][1] = 192; ys[ny][0] = 0; ys[ny++][1] = y1; }
        if (x0 <= x1) { xs[nx][0] = x0; xs[nx++][1] = x1; } else { xs[nx][0] = x0; xs[nx++][1] = 256; xs[nx][0] = 0; xs[nx++][1] = x1; }
        for (int a = 0; a < ny; a++) for (int b = 0; b < nx; b++) fill(xs[b][0], ys[a][0], xs[b][1], ys[a][1]);
    }
    C3D_DepthTest(true, GPU_ALWAYS, GPU_WRITE_ALL);  // (on: the depth-stencil buffer is only written with it)
}

static void prepare_palettes(const Eng *e) {
    int ei = !e->main;
    for (int i = 0; i < 256; i++) {
        sPal[ei][0][i] = sColor[rd16(e->bgpal + i * 2) & 0x7FFF];
        sPal[ei][1][i] = sColor[rd16(e->objpal + i * 2) & 0x7FFF];
    }
}

void port_render2d_engine_sw(int engine, u16 *out);  // render2d.c

static u32 bgr8_abgr(u16 c) {  // DS colour -> a TEV constant (0xAABBGGRR)
    u32 r = c & 31, g = (c >> 5) & 31, b = (c >> 10) & 31;
    r = r << 3 | r >> 2; g = g << 3 | g >> 2; b = b << 3 | b >> 2;
    return 0xFF000000u | b << 16 | g << 8 | r;
}

// The software renderer's picture of an engine, as the engine's image (for the modes the GPU
// path doesn't draw).
static void engine_from_software(int ei) {
    static u16 img[256 * 192];
    static C3D_Tex fb;
    if (!fb.data && !C3D_TexInit(&fb, 256, 256, GPU_RGBA5551)) return;
    C3D_TexSetFilter(&fb, GPU_NEAREST, GPU_NEAREST);
    port_render2d_engine_sw(ei, img);
    for (int y = 0; y < 192; y++)
        for (int x = 0; x < 256; x++) *tex_px(fb.data, 256, 256, x, y) = sColor[img[y * 256 + x] & 0x7FFF];
    C3D_TexFlush(&fb);
    set_tev(TEV_PLAIN, 0);
    blend_off();
    C3D_StencilTest(false, GPU_ALWAYS, 0, 0, 0);
    C3D_TexBind(0, &fb);
    quad(0, 0, 256, 192, 0, 0, 1, 192.0f / 256);
}

static void flat(u32 abgr, int x0, int y0, int x1, int y1) {
    set_tev(TEV_FLAT, (int)abgr);
    fill(x0, y0, x1, y1);
}

// The scissor box around the DS picture (or the whole screen), in the screen target's own
// (rotated) coordinates.
static void scissor_ds(int whole) {
    if (whole) { C3D_SetScissor(GPU_SCISSOR_NORMAL, 0, 0, 240, (u32)sScrW); return; }
    int x0 = (int)(-sL * sScale + 0.5f), y0 = (int)(-sT * sScale + 0.5f);
    int w = (int)(256 * sScale + 0.5f), h = (int)(192 * sScale + 0.5f);
    C3D_SetScissor(GPU_SCISSOR_NORMAL, (u32)(240 - (y0 + h)), (u32)x0, (u32)(240 - y0), (u32)(x0 + w));
}

static void layout(int screenW) {
    sScrW = screenW;
    sW = screenW / sScale; sH = 240 / sScale;
    sL = -(sW - 256) / 2; sT = -(sH - 192) / 2;
}

static void render_engine(Eng *e, C3D_RenderTarget *t, int screenW) {
    int ei = !e->main;
    e->dispcnt = rd32(e->io);
    C3D_FrameDrawOn(t);
    layout(screenW);
    C3D_Mtx proj;
    Mtx_OrthoTilt(&proj, sL, sL + sW, sT + sH, sT, -1, 1, true);
    C3D_FVUnifMtx4x4(GPU_VERTEX_SHADER, sProjLoc, &proj);
    sOX = sOY = 0;
    int wide3d = e->main && sWide;
    scissor_ds(0);
    C3D_DepthTest(true, GPU_ALWAYS, GPU_WRITE_ALL);  // (on: the depth-stencil buffer is only written with it)
    C3D_CullFace(GPU_CULL_NONE);
    C3D_AlphaTest(false, GPU_ALWAYS, 0);
    C3D_StencilTest(false, GPU_ALWAYS, 0, 0, 0);
    blend_off();
    int dmode = (e->dispcnt >> 16) & 3;
    if (!e->main && dmode > 1) dmode = 1;
    if (dmode == 0 || (e->dispcnt & 0x80)) {
        flat(0xFFFFFFFF, 0, 0, 256, 192);
        return;
    }
    prepare_palettes(e);
    u16 backdrop = rd16(e->bgpal) & 0x7FFF;
    Mix mx;
    u16 bldcnt = rd16(e->io + 0x50), bldalpha = rd16(e->io + 0x52);
    mx.first = bldcnt & 0x3F; mx.second = (bldcnt >> 8) & 0x3F; mx.effect = (bldcnt >> 6) & 3;
    mx.eva = bldalpha & 31; mx.evb = (bldalpha >> 8) & 31; mx.evy = rd16(e->io + 0x54) & 31;
    if (mx.evy > 16) mx.evy = 16;
    mx.windows = (e->dispcnt >> 13) & 7;
    u16 winin = rd16(e->io + 0x48), winout = rd16(e->io + 0x4A);
    mx.regionMask[0] = winout & 0x3F; mx.regionMask[1] = (winout >> 8) & 0x3F;
    mx.regionMask[2] = (winin >> 8) & 0x3F; mx.regionMask[3] = winin & 0x3F;
    // the backdrop, with a brighten/darken effect on it if it's a first target
    if ((mx.first & 0x20) && (mx.effect == 2 || mx.effect == 3) && !mx.windows) {
        int r = backdrop & 31, g = (backdrop >> 5) & 31, b = (backdrop >> 10) & 31;
        if (mx.effect == 2) { r += ((31 - r) * mx.evy) >> 4; g += ((31 - g) * mx.evy) >> 4; b += ((31 - b) * mx.evy) >> 4; }
        else { r -= (r * mx.evy) >> 4; g -= (g * mx.evy) >> 4; b -= (b * mx.evy) >> 4; }
        backdrop = (u16)(r | g << 5 | b << 10);
    }
    // the backdrop, and in the stencil whether it is a second blend target
    C3D_StencilTest(true, GPU_ALWAYS, (mx.second & 0x20) ? 4 : 0, 0, 0xFF);
    C3D_StencilOp(GPU_STENCIL_KEEP, GPU_STENCIL_KEEP, GPU_STENCIL_REPLACE);
    if (wide3d) { scissor_ds(1); flat(bgr8_abgr(backdrop), (int)sL - 1, (int)sT - 1, (int)(sL + sW) + 1, (int)(sT + sH) + 1); scissor_ds(0); }
    else flat(bgr8_abgr(backdrop), 0, 0, 256, 192);
    C3D_StencilTest(false, GPU_ALWAYS, 0, 0, 0);
    if (dmode >= 2) { engine_from_software(ei); return; }
    sprites_build(e);
    if (mx.windows) windows_to_stencil(e, mx.windows, ei);
    C3D_AlphaTest(true, GPU_GREATER, 0);

    int bgmode = e->dispcnt & 7;
    BgDraw bgd[4];
    int on[4] = { 0 };
    for (int bg = 0; bg < 4; bg++) {
        if (!(e->dispcnt & (0x100 << bg))) continue;
        if (bg == 0 && e->main && (e->dispcnt & 8)) {  // the 3D layer
            if (!s3dHave) continue;
            bgd[0] = (BgDraw){ e, 0, NULL, 3 };
            on[0] = 1;
            continue;
        }
        int kind;
        if (bg < 2) kind = 0;
        else if (bg == 2) kind = (bgmode == 2 || bgmode == 4) ? 1 : (bgmode == 5 ? 2 : (bgmode <= 1 || bgmode == 3 ? 0 : -1));
        else kind = (bgmode == 0) ? 0 : (bgmode <= 2 ? 1 : (bgmode <= 5 ? 2 : -1));
        if (kind < 0) continue;
        int wrap = 1;
        Plane *p = kind == 0 ? text_plane(e, bg) : affine_plane(e, bg, kind == 2, &wrap);
        if (!p) continue;
        bgd[bg] = (BgDraw){ e, bg, p, kind };
        on[bg] = 1;
    }
    int objon = (e->dispcnt >> 12) & 1;
    for (int prio = 3; prio >= 0; prio--) {
        for (int bg = 3; bg >= 0; bg--) {
            if (on[bg] && (rd16(e->io + 8 + bg * 2) & 3) == prio) draw_layer(&mx, 1 << bg, 0, bg_quads, &bgd[bg]);
        }
        if (objon) {
            // later OAM entries first, so lower ones end up in front
            for (int n = sNumSpr[ei] - 1; n >= 0; n--) {
                const Spr *s = &sSpr[ei][n];
                if (s->prio != prio || s->mode == 2) continue;
                SprDraw d = { ei, s };
                draw_layer(&mx, 0x10, s->mode == 1, spr_quads, &d);
            }
        }
    }
    // master brightness
    u16 mb = rd16(e->io + 0x6C);
    int f = mb & 31, m = (mb >> 14) & 3;
    if (f > 16) f = 16;
    if (f && (m == 1 || m == 2)) {
        C3D_AlphaTest(false, GPU_ALWAYS, 0);
        C3D_StencilTest(false, GPU_ALWAYS, 0, 0, 0);
        u8 a = (u8)(f * 255 / 16);
        set_tev(TEV_FLAT, (int)((u32)a << 24 | (m == 1 ? 0xFFFFFF : 0)));
        C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_SRC_ALPHA, GPU_ONE_MINUS_SRC_ALPHA, GPU_ZERO, GPU_ONE);
        if (wide3d) { scissor_ds(1); fill((int)sL - 1, (int)sT - 1, (int)(sL + sW) + 1, (int)(sT + sH) + 1); scissor_ds(0); }
        else fill(0, 0, 256, 192);
        blend_off();
    }
    C3D_StencilTest(false, GPU_ALWAYS, 0, 0, 0);
}

// ---------------------------------------------------------------- frames
static bool sReady;

bool port_gpu2d_init(void) {
    port_log("gpu: init\n");
    if (!C3D_Init(C3D_DEFAULT_CMDBUF_SIZE * 4)) return false;
    port_log("gpu: citro3d up\n");
    tables_init();
    sDvlb = DVLB_ParseFile((u32 *)quad_shbin, quad_shbin_size);
    shaderProgramInit(&sProg);
    shaderProgramSetVsh(&sProg, &sDvlb->DVLE[0]);
    C3D_BindProgram(&sProg);
    sProjLoc = shaderInstanceGetUniformLocation(sProg.vertexShader, "projection");
    C3D_AttrInfo *ai = C3D_GetAttrInfo();
    AttrInfo_Init(ai);
    AttrInfo_AddLoader(ai, 0, GPU_FLOAT, 2);
    AttrInfo_AddLoader(ai, 1, GPU_FLOAT, 2);
    port_log("gpu: shader %p\n", (void *)sDvlb);
    sVbo = linearAlloc(sizeof(Vtx) * MAX_VTX);
    if (!sVbo) { port_log("gpu: no vbo (linear free %lu)\n", linearSpaceFree()); return false; }
    C3D_BufInfo *bi = C3D_GetBufInfo();
    BufInfo_Init(bi);
    BufInfo_Add(bi, sVbo, sizeof(Vtx), 2, 0x10);
    const u32 xfer = GX_TRANSFER_FLIP_VERT(0) | GX_TRANSFER_OUT_TILED(0) | GX_TRANSFER_RAW_COPY(0) |
                     GX_TRANSFER_IN_FORMAT(GX_TRANSFER_FMT_RGBA8) | GX_TRANSFER_OUT_FORMAT(GX_TRANSFER_FMT_RGB8) |
                     GX_TRANSFER_SCALING(GX_TRANSFER_SCALE_NO);
    sTop = C3D_RenderTargetCreate(240, 400, GPU_RB_RGBA8, GPU_RB_DEPTH24_STENCIL8);
    sBottom = C3D_RenderTargetCreate(240, 320, GPU_RB_RGBA8, GPU_RB_DEPTH24_STENCIL8);
    if (!sTop || !sBottom) { port_log("gpu: no screen targets (vram free %lu)\n", vramSpaceFree()); return false; }
    C3D_RenderTargetSetOutput(sTop, GFX_TOP, GFX_LEFT, xfer);
    C3D_RenderTargetSetOutput(sBottom, GFX_BOTTOM, GFX_LEFT, xfer);
    if (!init3d()) port_log("gpu: no 3D layer\n");
    port_log("gpu: ready\n");
    sReady = true;
    return true;
}

// Draws both engines onto their screens (engine A on top when POWCNT1 bit 15 says so).
static int sReadable;  // (screenshot mode: single-buffered screens, so what's shown can be read)

u64 gGpu2dBusyTicks;  // CPU time building the frame (not waiting for the GPU or the VBlank)
u64 gGpu2dMaxTicks;   // the slowest frame
u32 gGpu2dRebuilds;   // BG textures rebuilt

void port_gpu2d_frame(void) {
    if (!sReady) return;
    C3D_FrameBegin(C3D_FRAME_SYNCDRAW);
    u64 t0 = svcGetSystemTick();
    sVtxUsed = 0;
    C3D_BindProgram(&sProg);
    static Eng a, b;
    a = (Eng){ 1, DS(0x04000000), DS(0x05000000), DS(0x05000200), DS(0x07000000), DS(0x06000000), DS(0x06400000), 0x80000, 0x40000, 0 };
    b = (Eng){ 0, DS(0x04001000), DS(0x05000400), DS(0x05000600), DS(0x07000400), DS(0x06200000), DS(0x06600000), 0x20000, 0x20000, 0 };
    int a_on_top = (rd16(DS(0x04000304)) >> 15) & 1;
    render3d(a_on_top ? 400 : 320);
    C3D_RenderTargetClear(sTop, C3D_CLEAR_ALL, 0x000000FF, 0);
    C3D_RenderTargetClear(sBottom, C3D_CLEAR_ALL, 0x000000FF, 0);
    render_engine(&a, a_on_top ? sTop : sBottom, a_on_top ? 400 : 320);
    render_engine(&b, a_on_top ? sBottom : sTop, a_on_top ? 320 : 400);
    u64 dt = svcGetSystemTick() - t0;
    gGpu2dBusyTicks += dt;
    if (dt > gGpu2dMaxTicks) gGpu2dMaxTicks = dt;
    C3D_FrameEnd(0);
}

// For checking: what the screens show, as DS colours (the DS rectangle of each screen).
void port_gpu2d_readback(u16 *top, u16 *bottom) {
    if (!sReady) return;
    if (!sReadable) {
        gfxSetDoubleBuffering(GFX_TOP, false);
        gfxSetDoubleBuffering(GFX_BOTTOM, false);
        sReadable = 1;
    }
    C3D_FrameBegin(C3D_FRAME_SYNCDRAW);  // (waits for the GPU and the transfer to the screens)
    C3D_FrameEnd(0);
    gspWaitForPPF();
    for (int s = 0; s < 2; s++) {
        u16 w, h;
        const u8 *fb = gfxGetFramebuffer(s == 0 ? GFX_TOP : GFX_BOTTOM, GFX_LEFT, &w, &h);
        u16 *out = s == 0 ? top : bottom;
        layout(h);
        for (int y = 0; y < 192; y++) {
            for (int x = 0; x < 256; x++) {
                // (the screen pixel at the DS pixel's centre: exactly that pixel under nearest scaling)
                int sx = (int)((x + 0.5f - sL) * sScale), sy = (int)((y + 0.5f - sT) * sScale);
                const u8 *p = fb + (sx * 240 + (239 - sy)) * 3;  // (B, G, R)
                out[y * 256 + x] = (u16)((p[2] >> 3) | (p[1] >> 3) << 5 | (p[0] >> 3) << 10);
            }
        }
    }
}

// For checking: both screens as shown, at the 3DS's resolution (full_N.bmp: the top screen over
// the bottom one, centred). Call after port_gpu2d_readback (which makes the screens readable).
void port_gpu2d_dump_full(u32 frame) {
    {
        const G3Frame *g = port_g3_frame();
        port_log("edge colours %04X %04X %04X %04X %04X %04X %04X %04X\n", rd16(DS(0x04000330)), rd16(DS(0x04000332)), rd16(DS(0x04000334)), rd16(DS(0x04000336)),
                 rd16(DS(0x04000338)), rd16(DS(0x0400033A)), rd16(DS(0x0400033C)), rd16(DS(0x0400033E)));
        port_log("frame %lu: wide %d BGCNT %04X %04X %04X %04X DISPCNT %08lX DISP3DCNT %04X CLEAR %08lX vp %d,%d,%d,%d\n", (unsigned long)frame, sWide, rd16(DS(0x04000008)), rd16(DS(0x0400000A)), rd16(DS(0x0400000C)), rd16(DS(0x0400000E)), rd32(DS(0x04000000)),
                 *(volatile u16 *)DS(0x04000060), *(volatile u32 *)DS(0x04000350), g ? g->viewport[0] : -1, g ? g->viewport[1] : -1, g ? g->viewport[2] : -1, g ? g->viewport[3] : -1);
    }
    char path[96];
    snprintf(path, sizeof(path), "sdmc:/3ds/soulsilver-3ds/full_%05lu.bmp", (unsigned long)frame);
    FILE *f = fopen(path, "wb");
    if (!f) return;
    const int W = 400, H = 480, rowb = W * 3, size = 54 + rowb * H;
    u8 hd[54] = { 'B', 'M' };
    hd[2] = size; hd[3] = size >> 8; hd[4] = size >> 16; hd[10] = 54; hd[14] = 40;
    hd[18] = W; hd[19] = W >> 8; hd[22] = H; hd[23] = H >> 8; hd[26] = 1; hd[28] = 24;
    fwrite(hd, 1, 54, f);
    static u8 row[400 * 3];
    for (int y = H - 1; y >= 0; y--) {
        memset(row, 0, sizeof(row));
        int s = y >= 240, sy = y % 240;
        u16 fw, fh;
        const u8 *fb = gfxGetFramebuffer(s ? GFX_BOTTOM : GFX_TOP, GFX_LEFT, &fw, &fh);
        int ox = (W - fh) / 2;
        for (int x = 0; x < fh; x++) memcpy(row + (x + ox) * 3, fb + (x * 240 + (239 - sy)) * 3, 3);
        fwrite(row, 1, rowb, f);
    }
    fclose(f);
}

// For debugging: an engine's display registers and visible sprites.
void port_gpu2d_describe(int engine) {
    const u8 *io = DS(engine ? 0x04001000 : 0x04000000);
    const u8 *oam = DS(engine ? 0x07000400 : 0x07000000);
    port_log("  engine %c: DISPCNT %08lX BG0-3CNT %04X %04X %04X %04X BLDCNT %04X BLDALPHA %04X BLDY %04X WIN %04X/%04X %04X/%04X IN %04X OUT %04X MB %04X\n",
             engine ? 'B' : 'A', rd32(io), rd16(io + 8), rd16(io + 10), rd16(io + 12), rd16(io + 14), rd16(io + 0x50), rd16(io + 0x52),
             rd16(io + 0x54), rd16(io + 0x40), rd16(io + 0x44), rd16(io + 0x42), rd16(io + 0x46), rd16(io + 0x48), rd16(io + 0x4A), rd16(io + 0x6C));
    for (int i = 0; i < 128; i++) {
        u16 a0 = rd16(oam + i * 8), a1 = rd16(oam + i * 8 + 2), a2 = rd16(oam + i * 8 + 4);
        if (!(a0 & 0x100) && (a0 & 0x200)) continue;
        port_log("    obj %d: attr %04X %04X %04X (mode %d prio %d)\n", i, a0, a1, a2, (a0 >> 10) & 3, (a2 >> 10) & 3);
    }
}

// ================================================================ the 3D layer
// The geometry engine's polygons (gx3d.c), drawn into a 256x256 image with depth, which the 2D
// compositor then shows as BG0. DS textures are decoded from the texture slots (the VRAM banks
// mapped for textures, read where the game wrote them: through LCDC) into RGBA8 and cached.

extern const u8 poly_shbin[];
extern const u32 poly_shbin_size;
extern const u8 edge_shbin[];
extern const u32 edge_shbin_size;
static DVLB_s *sDvlb3d, *sDvlbEdge;
static shaderProgram_s sProg3d, sProgEdge;
static C3D_RenderTarget *s3dTarget;
// Edge marking: each opaque pixel's polygon ID (R, as ID * 4) and depth (G, larger = nearer),
// drawn into its own image against the 3D layer's depth buffer.
typedef struct { float x, y, z, w, u0, v0, u1, v1; } VE;
static VE *sVboEdge;
static int s3dReady;

typedef struct { float x, y, z, w, r, g, b, a, s, t; } V3;
#define MAX_V3 24576
static V3 *sVbo3d;

static void attrs_2d(void) {
    C3D_AttrInfo *ai = C3D_GetAttrInfo();
    AttrInfo_Init(ai);
    AttrInfo_AddLoader(ai, 0, GPU_FLOAT, 2);
    AttrInfo_AddLoader(ai, 1, GPU_FLOAT, 2);
    C3D_BufInfo *bi = C3D_GetBufInfo();
    BufInfo_Init(bi);
    BufInfo_Add(bi, sVbo, sizeof(Vtx), 2, 0x10);
}

static void attrs_3d(void) {
    C3D_AttrInfo *ai = C3D_GetAttrInfo();
    AttrInfo_Init(ai);
    AttrInfo_AddLoader(ai, 0, GPU_FLOAT, 4);
    AttrInfo_AddLoader(ai, 1, GPU_FLOAT, 4);
    AttrInfo_AddLoader(ai, 2, GPU_FLOAT, 2);
    C3D_BufInfo *bi = C3D_GetBufInfo();
    BufInfo_Init(bi);
    BufInfo_Add(bi, sVbo3d, sizeof(V3), 3, 0x210);
}

static bool init3d(void) {
    sDvlb3d = DVLB_ParseFile((u32 *)poly_shbin, poly_shbin_size);
    shaderProgramInit(&sProg3d);
    shaderProgramSetVsh(&sProg3d, &sDvlb3d->DVLE[0]);
    sVbo3d = linearAlloc(sizeof(V3) * MAX_V3);
    if (!sVbo3d) return false;
    if (!C3D_TexInitVRAM(&s3dTex, 512, 256, GPU_RGBA8)) return false;
    C3D_TexSetFilter(&s3dTex, GPU_NEAREST, GPU_NEAREST);
    s3dTarget = C3D_RenderTargetCreateFromTex(&s3dTex, GPU_TEXFACE_2D, 0, GPU_RB_DEPTH24_STENCIL8);
    if (!s3dTarget) return false;
    sDvlbEdge = DVLB_ParseFile((u32 *)edge_shbin, edge_shbin_size);
    shaderProgramInit(&sProgEdge);
    shaderProgramSetVsh(&sProgEdge, &sDvlbEdge->DVLE[0]);
    sVboEdge = linearAlloc(sizeof(VE) * 6 * 8);
    if (sVboEdge && C3D_TexInitVRAM(&sIdTex, 512, 256, GPU_RGBA8)) {
        C3D_TexSetFilter(&sIdTex, GPU_NEAREST, GPU_NEAREST);
        C3D_TexSetWrap(&sIdTex, GPU_CLAMP_TO_EDGE, GPU_CLAMP_TO_EDGE);
        // (its own depth buffer: an "equal" test against the 3D pass's fails on alternate
        // columns, the PICA doesn't reproduce depths bit for bit between passes)
        sIdTarget = C3D_RenderTargetCreateFromTex(&sIdTex, GPU_TEXFACE_2D, 0, GPU_RB_DEPTH24_STENCIL8);
    }
    s3dReady = 1;
    return true;
}

// ---------------------------------------------------------------- texture memory
static const u8 *tex_slot(int slot) {
    for (int b = 0; b < 4; b++) {
        u8 c = vramcnt(b);
        if ((c & 0x80) && (c & 3) == 3 && ((c >> 3) & 3) == slot) return DS(kBankLcdc[b]);
    }
    return NULL;
}
static const u8 *tex_ptr(u32 addr) {
    const u8 *s = tex_slot((addr >> 17) & 3);
    return s ? s + (addr & 0x1FFFF) : NULL;
}
static const u8 *pal_ptr(u32 addr) {  // 16 KB palette slots: E covers 0-3, F/G one each (0, 1, 4, 5)
    int slot = (addr >> 14) & 7;
    u8 c = vramcnt(4);
    if ((c & 0x80) && (c & 7) == 3 && slot < 4) return DS(kBankLcdc[4]) + (addr & 0xFFFF);
    for (int b = 5; b <= 6; b++) {
        c = vramcnt(b);
        if ((c & 0x80) && (c & 7) == 3) {
            int ofs = (c >> 3) & 3, s = (ofs & 1) + ((ofs >> 1) & 1) * 4;
            if (s == slot) return DS(kBankLcdc[b]) + (addr & 0x3FFF);
        }
    }
    return NULL;
}

static inline u32 rgba(u16 c, u8 a) {
    u32 r = c & 31, g = (c >> 5) & 31, b = (c >> 10) & 31;
    return (r << 3 | r >> 2) << 24 | (g << 3 | g >> 2) << 16 | (b << 3 | b >> 2) << 8 | a;
}
static inline u32 *tex32_px(u32 *data, int W, int H, int x, int y) {
    int row = (H >> 3) - 1 - (y >> 3);
    return data + (row * (W >> 3) + (x >> 3)) * 64 + sMorton[(7 - (y & 7)) * 8 + (x & 7)];
}

typedef struct {
    u32 key, pltt, hash, hashFrame, used;
    int w, h;
    C3D_Tex tex;
} TexEntry;
#define TEX_CACHE 192
static TexEntry sTexCache[TEX_CACHE];
static u32 sFrameNo;

static u32 tex_bytes(int fmt, int w, int h) {
    static const u8 bpp[8] = { 0, 8, 2, 4, 8, 2, 8, 16 };
    return (u32)w * h * bpp[fmt] / 8;
}
static u32 pal_bytes(int fmt) {
    static const u16 n[8] = { 0, 64, 8, 32, 512, 0x400, 16, 0 };  // (4x4: up to a block's reach; approximate)
    return n[fmt];
}

static void decode_texture(u32 *out, int w, int h, int fmt, int color0, u32 addr, u32 pltt) {
    const u8 *src = tex_ptr(addr);
    u32 paddr = fmt == 2 ? pltt << 3 : pltt << 4;
    const u8 *pal = pal_ptr(paddr);
    if (!src || (fmt != 7 && !pal)) {
        for (int y = 0; y < h; y++) for (int x = 0; x < w; x++) *tex32_px(out, w, h, x, y) = 0xFF00FFFF;  // (missing: magenta)
        return;
    }
    if (fmt == 5) {  // 4x4 compressed: 32-bit texel blocks, 16-bit palette info in slot 1
        u32 iaddr = 0x20000 + ((addr & 0x1FFFF) >> 1) + ((addr >> 17) == 2 ? 0x10000 : 0);
        const u8 *idx = tex_ptr(iaddr);
        for (int by = 0; by < h / 4; by++) {
            for (int bx = 0; bx < w / 4; bx++) {
                int blk = by * (w / 4) + bx;
                u32 bits = rd32(src + blk * 4);
                u16 info = idx ? rd16(idx + blk * 2) : 0;
                const u8 *pb = pal_ptr(paddr + (info & 0x3FFF) * 4);
                u32 c[4] = { 0, 0, 0, 0 };
                if (pb) {
                    u16 c0 = rd16(pb), c1 = rd16(pb + 2);
                    int mode = info >> 14;
                    c[0] = rgba(c0, 255); c[1] = rgba(c1, 255);
                    #define MIX(a, b, wa, wb, d) (((((a) & 31) * (wa) + ((b) & 31) * (wb)) / (d)) | (((((a) >> 5) & 31) * (wa) + (((b) >> 5) & 31) * (wb)) / (d)) << 5 | (((((a) >> 10) & 31) * (wa) + (((b) >> 10) & 31) * (wb)) / (d)) << 10)
                    if (mode == 0) { c[2] = rgba(rd16(pb + 4), 255); c[3] = 0; }
                    else if (mode == 1) { c[2] = rgba((u16)MIX(c0, c1, 1, 1, 2), 255); c[3] = 0; }
                    else if (mode == 2) { c[2] = rgba(rd16(pb + 4), 255); c[3] = rgba(rd16(pb + 6), 255); }
                    else { c[2] = rgba((u16)MIX(c0, c1, 5, 3, 8), 255); c[3] = rgba((u16)MIX(c0, c1, 3, 5, 8), 255); }
                    #undef MIX
                }
                for (int y = 0; y < 4; y++)
                    for (int x = 0; x < 4; x++) *tex32_px(out, w, h, bx * 4 + x, by * 4 + y) = c[(bits >> ((y * 4 + x) * 2)) & 3];
            }
        }
        return;
    }
    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            int i = y * w + x;
            u32 c;
            switch (fmt) {
            case 1: { u8 b = src[i]; int a = b >> 5; c = rgba(rd16(pal + (b & 31) * 2), (u8)((a << 5) | (a << 2) | (a >> 1))); break; }
            case 2: { int v = (src[i >> 2] >> ((i & 3) * 2)) & 3; c = (color0 && !v) ? 0 : rgba(rd16(pal + v * 2), 255); break; }
            case 3: { int v = (src[i >> 1] >> ((i & 1) * 4)) & 15; c = (color0 && !v) ? 0 : rgba(rd16(pal + v * 2), 255); break; }
            case 4: { int v = src[i]; c = (color0 && !v) ? 0 : rgba(rd16(pal + v * 2), 255); break; }
            case 6: { u8 b = src[i]; int a = b >> 3; c = rgba(rd16(pal + (b & 7) * 2), (u8)(a << 3 | a >> 2)); break; }
            case 7: { u16 v = rd16(src + i * 2); c = (v & 0x8000) ? rgba(v, 255) : 0; break; }
            default: c = 0xFFFFFFFF;
            }
            *tex32_px(out, w, h, x, y) = c;
        }
    }
}

static C3D_Tex *texture_for(u32 teximage, u32 pltt) {
    int fmt = (teximage >> 26) & 7;
    if (!fmt) return NULL;
    int w = 8 << ((teximage >> 20) & 7), h = 8 << ((teximage >> 23) & 7);
    u32 key = teximage & 0x3FFFFFFF & ~(0xFu << 16);  // (address, size, format, colour 0: not the wrap bits)
    u32 addr = (teximage & 0xFFFF) << 3;
    TexEntry *e = NULL, *lru = &sTexCache[0];
    for (int i = 0; i < TEX_CACHE; i++) {
        TexEntry *t = &sTexCache[i];
        if (t->tex.data && t->key == key && t->pltt == pltt) { e = t; break; }
        if (t->used < lru->used) lru = t;
    }
    if (!e) {
        e = lru;
        if (e->tex.data && (e->w != w || e->h != h)) { C3D_TexDelete(&e->tex); e->tex.data = NULL; }
        if (!e->tex.data) {
            if (!C3D_TexInit(&e->tex, (u16)w, (u16)h, GPU_RGBA8)) return NULL;
            C3D_TexSetFilter(&e->tex, GPU_NEAREST, GPU_NEAREST);
        }
        e->key = key; e->pltt = pltt; e->w = w; e->h = h; e->hash = 0; e->hashFrame = 0;
    }
    e->used = sFrameNo;
    if (e->hashFrame != sFrameNo) {
        extern uint32_t port_trace3d;
        if (port_trace3d && port_frame_count == port_trace3d) {
            const u8 *src = tex_ptr(addr), *pal = pal_ptr(fmt == 2 ? pltt << 3 : pltt << 4);
            u32 nz = 0, bytes = tex_bytes(fmt, w, h);
            if (src) for (u32 i = 0; i < bytes; i++) nz += src[i] != 0;
            port_log("  tex %08lX pltt %04lX: fmt %d %dx%d addr %05lX src %p (%lu/%lu nonzero) pal %p [%04X %04X]\n", (unsigned long)teximage, (unsigned long)pltt,
                 fmt, w, h, (unsigned long)addr, (const void *)src, (unsigned long)nz, (unsigned long)bytes, (const void *)pal, pal ? rd16(pal) : 0, pal ? rd16(pal + 2) : 0);
        }
  // (the data can change any frame: hashed once a frame)
        e->hashFrame = sFrameNo;
        u32 hsh = 2166136261u;
        const u8 *src = tex_ptr(addr);
        if (src) hsh = hash_bytes(hsh, src, tex_bytes(fmt, w, h) & ~3u);
        const u8 *pal = pal_ptr(fmt == 2 ? pltt << 3 : pltt << 4);
        if (pal && pal_bytes(fmt)) hsh = hash_bytes(hsh, pal, pal_bytes(fmt));
        if (hsh != e->hash || !e->hash) {
            e->hash = hsh | 1;
            decode_texture(e->tex.data, w, h, fmt, (teximage >> 29) & 1, addr, pltt);
            C3D_TexFlush(&e->tex);
        }
    }
    return &e->tex;
}

static GPU_TEXTURE_WRAP_PARAM wrap_of(int repeat, int flip) {
    return !repeat ? GPU_CLAMP_TO_EDGE : flip ? GPU_MIRRORED_REPEAT : GPU_REPEAT;
}

// ---------------------------------------------------------------- drawing a frame
static float sClipX = 1.0f, sClipY = 1.0f;  // clip-space scale (widening)
static void draw_polys(const G3Frame *f, int translucent, u32 disp3d, int *nv) {
    for (u32 i = 0; i < f->np; i++) {
        const G3Poly *p = &f->p[i];
        if (p->translucent != translucent) continue;
        int fmt = (p->teximage >> 26) & 7;
        C3D_Tex *tex = (fmt && (disp3d & 1)) ? texture_for(p->teximage, p->pltt) : NULL;
        int mode = (p->polyattr >> 4) & 3;
        C3D_TexEnv *env = C3D_GetTexEnv(0);
        C3D_TexEnvInit(env);
        C3D_TexEnvInit(C3D_GetTexEnv(1));
        if (tex) {
            C3D_TexSetWrap(tex, wrap_of((p->teximage >> 16) & 1, (p->teximage >> 18) & 1), wrap_of((p->teximage >> 17) & 1, (p->teximage >> 19) & 1));
            C3D_TexBind(0, tex);
            if (mode == 1) {  // decal: texture over vertex colour by texture alpha
                C3D_TexEnvSrc(env, C3D_RGB, GPU_TEXTURE0, GPU_PRIMARY_COLOR, GPU_TEXTURE0);
                C3D_TexEnvOpRgb(env, GPU_TEVOP_RGB_SRC_COLOR, GPU_TEVOP_RGB_SRC_COLOR, GPU_TEVOP_RGB_SRC_ALPHA);
                C3D_TexEnvFunc(env, C3D_RGB, GPU_INTERPOLATE);
                C3D_TexEnvSrc(env, C3D_Alpha, GPU_PRIMARY_COLOR, 0, 0);
                C3D_TexEnvFunc(env, C3D_Alpha, GPU_REPLACE);
            } else {  // modulate (toon/highlight approximated as modulate)
                C3D_TexEnvSrc(env, C3D_Both, GPU_TEXTURE0, GPU_PRIMARY_COLOR, 0);
                C3D_TexEnvFunc(env, C3D_Both, GPU_MODULATE);
            }
        } else {
            C3D_TexEnvSrc(env, C3D_Both, GPU_PRIMARY_COLOR, 0, 0);
            C3D_TexEnvFunc(env, C3D_Both, GPU_REPLACE);
        }
        // depth: translucent polygons write depth only when asked; "equal" test on request
        int depthWrite = !translucent || (p->polyattr & 0x800);
        C3D_DepthTest(true, (p->polyattr & 0x4000) ? GPU_EQUAL : GPU_GREATER, depthWrite ? GPU_WRITE_ALL : GPU_WRITE_COLOR);
        if (translucent && (disp3d & 8)) C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_SRC_ALPHA, GPU_ONE_MINUS_SRC_ALPHA, GPU_ONE, GPU_ONE_MINUS_SRC_ALPHA);
        else blend_off();
        int aref = (disp3d & 4) ? (rd16(DS(0x04000340)) & 31) * 8 : 0;
        C3D_AlphaTest(true, GPU_GREATER, aref);
        if (*nv + (int)p->count > MAX_V3) return;
        float sw = tex ? 1.0f / tex->width : 0, sh = tex ? 1.0f / tex->height : 0;
        for (u32 k = 0; k < p->count; k++) {
            const G3Vtx *v = &f->v[p->first + k];
            V3 *o = &sVbo3d[*nv + k];
            // DS clip z/w in -1..1 (near -1) to the PICA's -w..0, near at -w (depth = -z/w: nearer is larger)
            *o = (V3){ v->x * sClipX, -v->y * sClipY, (v->z - v->w) * 0.5f, v->w, v->r / 255.0f, v->g / 255.0f, v->b / 255.0f, v->a / 255.0f,
                       v->s * sw, v->t * sh };
        }
        C3D_DrawArrays(GPU_TRIANGLES, *nv, (int)p->count);
        *nv += (int)p->count;
    }
}

// ---------------------------------------------------------------- edge marking
// The DS outlines a pixel with the edge colour of its polygon ID's group when one of its four
// neighbours has a different polygon ID and is farther away (the clear plane counts, with the
// clear polygon ID). First the IDs: the opaque polygons again (nearest wins, its own depth
// buffer), with the texture's alpha test.
static void draw_ids(const G3Frame *f, u32 disp3d, int *nv, int translucent) {
    // (depth goes in 8 bits: spread over this frame's own range of opaque depths)
    float zmin = 1, zmax = -1;
    for (u32 i = 0; i < f->np; i++) {
        const G3Poly *p = &f->p[i];
        if (p->translucent) continue;
        for (u32 k = 0; k < p->count; k++) {
            const G3Vtx *v = &f->v[p->first + k];
            if (v->w <= 0) continue;
            float z = v->z / v->w;
            if (z < zmin) zmin = z;
            if (z > zmax) zmax = z;
        }
    }
    if (zmin < -1) zmin = -1;
    if (zmax > 1) zmax = 1;
    float zscale = zmax > zmin ? 0.996f / (zmax - zmin) : 0;
    for (u32 i = 0; i < f->np; i++) {
        const G3Poly *p = &f->p[i];
        if (p->translucent != translucent) continue;
        int fmt = (p->teximage >> 26) & 7;
        C3D_Tex *tex = (fmt && (disp3d & 1)) ? texture_for(p->teximage, p->pltt) : NULL;
        C3D_TexEnv *env = C3D_GetTexEnv(0);
        C3D_TexEnvInit(env);
        C3D_TexEnvSrc(env, C3D_RGB, GPU_PRIMARY_COLOR, 0, 0);
        C3D_TexEnvFunc(env, C3D_RGB, GPU_REPLACE);
        if (tex) {
            C3D_TexSetWrap(tex, wrap_of((p->teximage >> 16) & 1, (p->teximage >> 18) & 1), wrap_of((p->teximage >> 17) & 1, (p->teximage >> 19) & 1));
            C3D_TexBind(0, tex);
            C3D_TexEnvSrc(env, C3D_Alpha, GPU_TEXTURE0, 0, 0);
        } else {
            C3D_TexEnvSrc(env, C3D_Alpha, GPU_PRIMARY_COLOR, 0, 0);
        }
        C3D_TexEnvFunc(env, C3D_Alpha, GPU_REPLACE);
        int aref = (disp3d & 4) ? (rd16(DS(0x04000340)) & 31) * 8 : 0;
        C3D_AlphaTest(true, GPU_GREATER, aref);
        if (*nv + (int)p->count > MAX_V3) return;
        float id = (float)(((p->polyattr >> 24) & 63) * 4) / 255.0f;  // (translucent: only G = 0 is written)
        float sw = tex ? 1.0f / tex->width : 0, sh = tex ? 1.0f / tex->height : 0;
        for (u32 k = 0; k < p->count; k++) {
            const G3Vtx *v = &f->v[p->first + k];
            V3 *o = &sVbo3d[*nv + k];
            float d = v->w > 0 && !translucent ? 0.004f + (zmax - v->z / v->w) * zscale : 0;  // (nearer: larger; 0: never an edge)
            if (d < 0) d = 0;
            if (d > 1) d = 1;
            *o = (V3){ v->x * sClipX, -v->y * sClipY, (v->z - v->w) * 0.5f, v->w, id, d, 0, 1, v->s * sw, v->t * sh };
        }
        C3D_DrawArrays(GPU_TRIANGLES, *nv, (int)p->count);
        *nv += (int)p->count;
    }
}

// Then the outlines, over the 3D image: per neighbour and per sign of the ID difference, a pass
// whose combiners turn (ID difference > 0) and (this pixel nearer) into an alpha of 0 or 1.
static void draw_edges(void) {
    C3D_BindProgram(&sProgEdge);
    C3D_AttrInfo *ai = C3D_GetAttrInfo();
    AttrInfo_Init(ai);
    AttrInfo_AddLoader(ai, 0, GPU_FLOAT, 4);
    AttrInfo_AddLoader(ai, 1, GPU_FLOAT, 2);
    AttrInfo_AddLoader(ai, 2, GPU_FLOAT, 2);
    C3D_BufInfo *bi = C3D_GetBufInfo();
    BufInfo_Init(bi);
    BufInfo_Add(bi, sVboEdge, sizeof(VE), 3, 0x210);
    C3D_SetViewport(0, 0, (u32)sScrW, 240);
    C3D_DepthTest(false, GPU_ALWAYS, GPU_WRITE_COLOR);
    C3D_AlphaTest(true, GPU_GREATER, 0);
    blend_off();
    C3D_TexBind(0, &sIdTex);
    C3D_TexBind(1, &sIdTex);
    // (the edge colour: the table entry of the group most polygons on screen use is close enough
    // for one colour; HGSS sets them all alike)
    u16 ec = rd16(DS(0x04000330));
    u32 r = ec & 31, g = (ec >> 5) & 31, b = (ec >> 10) & 31;
    u32 abgr = 0xFF000000u | (b << 3 | b >> 2) << 16 | (g << 3 | g >> 2) << 8 | (r << 3 | r >> 2);
    const float U = sScrW / 512.0f, V = 240 / 256.0f;
    static const int nb[4][2] = { { 1, 0 }, { -1, 0 }, { 0, 1 }, { 0, -1 } };
    int n = 0;
    for (int d = 0; d < 4; d++) {
        float du = nb[d][0] / 512.0f, dv = nb[d][1] / 256.0f;
        // corners: top-left, top-right, bottom-left, bottom-right (clip y up, negated going in)
        VE c[4] = { { -1, -1, -0.5f, 1, 0, 0, du, dv }, { 1, -1, -0.5f, 1, U, 0, U + du, dv },
                    { -1, 1, -0.5f, 1, 0, V, du, V + dv }, { 1, 1, -0.5f, 1, U, V, U + du, V + dv } };
        static const int idx[6] = { 0, 1, 2, 1, 3, 2 };
        for (int sign = 0; sign < 2; sign++) {
            VE *q = sVboEdge + n;
            for (int k = 0; k < 6; k++) q[k] = c[idx[k]];
            for (int s = 0; s < 6; s++) C3D_TexEnvInit(C3D_GetTexEnv(s));
            C3D_TexEnv *e0 = C3D_GetTexEnv(0);
            // RGB: ID difference in R (this one's minus the neighbour's, or the other way)
            if (sign == 0) C3D_TexEnvSrc(e0, C3D_RGB, GPU_TEXTURE0, GPU_TEXTURE1, 0);
            else C3D_TexEnvSrc(e0, C3D_RGB, GPU_TEXTURE1, GPU_TEXTURE0, 0);
            C3D_TexEnvFunc(e0, C3D_RGB, GPU_SUBTRACT);
            // alpha: how much nearer this pixel is (G: larger is nearer)
            C3D_TexEnvSrc(e0, C3D_Alpha, GPU_TEXTURE0, GPU_TEXTURE1, 0);
            C3D_TexEnvOpAlpha(e0, GPU_TEVOP_A_SRC_G, GPU_TEVOP_A_SRC_G, GPU_TEVOP_A_SRC_ALPHA);
            C3D_TexEnvFunc(e0, C3D_Alpha, GPU_SUBTRACT);
            C3D_TexEnvScale(e0, C3D_Both, GPU_TEVSCALE_4);
            for (int s = 1; s <= 3; s++) {  // (any difference saturates to 1)
                C3D_TexEnv *e = C3D_GetTexEnv(s);
                C3D_TexEnvSrc(e, C3D_Both, GPU_PREVIOUS, 0, 0);
                C3D_TexEnvFunc(e, C3D_Both, GPU_REPLACE);
                C3D_TexEnvScale(e, C3D_Both, GPU_TEVSCALE_4);
            }
            C3D_TexEnv *e4 = C3D_GetTexEnv(4);
            C3D_TexEnvSrc(e4, C3D_RGB, GPU_CONSTANT, 0, 0);
            C3D_TexEnvFunc(e4, C3D_RGB, GPU_REPLACE);
            C3D_TexEnvSrc(e4, C3D_Alpha, GPU_PREVIOUS, GPU_PREVIOUS, 0);
            C3D_TexEnvOpAlpha(e4, GPU_TEVOP_A_SRC_R, GPU_TEVOP_A_SRC_ALPHA, GPU_TEVOP_A_SRC_ALPHA);
            C3D_TexEnvFunc(e4, C3D_Alpha, GPU_MODULATE);
            C3D_TexEnvColor(e4, abgr);
            C3D_DrawArrays(GPU_TRIANGLES, n, 6);
            n += 6;
        }
    }
    for (int s = 1; s < 6; s++) C3D_TexEnvInit(C3D_GetTexEnv(s));
    C3D_TexBind(1, NULL);
}

// Whether this frame's 3D is a scene to widen: the 3D layer on the top screen, over the whole DS
// viewport, with opaque polygons covering all four corners of the DS frame (the overworld: the
// map is the scene, 2D windows and text in front), as opposed to 3D sprites over 2D backgrounds
// (battles, menus). A change has to hold for a few frames (no flicker through scene changes).
static int covers(const G3Frame *f, float px, float py) {
    for (u32 i = 0; i < f->np; i++) {
        const G3Poly *p = &f->p[i];
        if (p->translucent || ((p->polyattr >> 4) & 3) == 3) continue;
        for (u32 k = 0; k + 2 < p->count; k += 3) {
            const G3Vtx *a = &f->v[p->first + k], *b = a + 1, *c = a + 2;
            if (a->w <= 0 || b->w <= 0 || c->w <= 0) continue;
            float ax = a->x / a->w, ay = a->y / a->w, bx = b->x / b->w, by = b->y / b->w, cx = c->x / c->w, cy = c->y / c->w;
            float d1 = (bx - ax) * (py - ay) - (by - ay) * (px - ax);
            float d2 = (cx - bx) * (py - by) - (cy - by) * (px - bx);
            float d3 = (ax - cx) * (py - cy) - (ay - cy) * (px - cx);
            if ((d1 >= 0 && d2 >= 0 && d3 >= 0) || (d1 <= 0 && d2 <= 0 && d3 <= 0)) return 1;
        }
    }
    return 0;
}

static int want_wide(const G3Frame *f, int screenW) {
    if (port_wide_mode == 0 || screenW != 400) return 0;
    if (f->viewport[0] != 0 || f->viewport[2] != 255 || f->viewport[1] != 0 || f->viewport[3] != 191) return 0;
    u32 dispcnt = rd32(DS(0x04000000));
    if (!(dispcnt & 8) || !(dispcnt & 0x100)) return 0;
    if (port_wide_mode == 2) return 1;
    // (corners in clip space, y up, a few pixels in)
    const float ix = 1 - 4 / 128.0f, iy = 1 - 4 / 96.0f;
    return covers(f, -ix, iy) && covers(f, ix, iy) && covers(f, -ix, -iy) && covers(f, ix, -iy);
}

static int wide_stable(int want) {
    static int state, count;
    if (want == state) { count = 0; return state; }
    if (++count >= 4) { state = want; count = 0; }
    return state;
}

static void render3d(int screenW) {
    const G3Frame *f = port_g3_frame();
    layout(screenW);
    sWide = 0;
    if (!f || !s3dReady) return;
    sWide = wide_stable(want_wide(f, screenW));
    port_g3_wide = sWide ? sW / 256.0f : 1.0f;
    port_g3_wide_y = sWide ? sH / 192.0f : 1.0f;
    sFrameNo++;
    u32 disp3d = *(volatile u16 *)DS(0x04000060);
    u32 clear = *(volatile u32 *)DS(0x04000350);
    int ca = (clear >> 16) & 31;
    u32 cc = rgba((u16)(clear & 0x7FFF), (u8)(ca << 3 | ca >> 2));
    C3D_RenderTargetClear(s3dTarget, C3D_CLEAR_ALL, cc, 0);
    C3D_FrameDrawOn(s3dTarget);
    C3D_BindProgram(&sProg3d);
    attrs_3d();
    // the DS viewport (x1, y1 from the bottom, x2, y2) on the 256x192 picture. (Measured with a test
    // square: the render target's viewport y counts from the picture's top, and its y axis points
    // down, so vertices go in with y negated.)
    // In screen pixels (the image covers the whole screen: DS pixel x is at (x - sL) * sScale).
    int x1 = f->viewport[0], y1 = f->viewport[1], x2 = f->viewport[2], y2 = f->viewport[3];
    float vx = (x1 - sL) * sScale, vy = (191 - y2 - sT) * sScale, vw = (x2 - x1 + 1) * sScale, vh = (y2 - y1 + 1) * sScale;
    sClipX = sClipY = 1.0f;
    if (sWide) { sClipX = vw / sScrW; sClipY = vh / 240; vx = vy = 0; vw = (float)sScrW; vh = 240; }
    // (C3D_FrameDrawOn resets the viewport to the whole target: set again after each switch)
    const u32 vpx = (u32)(vx + 0.5f), vpy = (u32)(vy + 0.5f), vpw = (u32)(vw + 0.5f), vph = (u32)(vh + 0.5f);
    C3D_SetViewport(vpx, vpy, vpw, vph);
    C3D_SetScissor(GPU_SCISSOR_DISABLE, 0, 0, 0, 0);
    C3D_StencilTest(false, GPU_ALWAYS, 0, 0, 0);
    C3D_CullFace(GPU_CULL_NONE);
    int nv = 0;
    draw_polys(f, 0, disp3d, &nv);
    if ((disp3d & 0x20) && sIdTarget && port_edge_mode) {
        // the IDs of what's visible, then the outlines over the opaque picture. (The DS marks
        // edges from the opaque pixels' IDs; translucent polygons blend over the result.)
        int ida = (clear >> 24) & 63;
        C3D_RenderTargetClear(sIdTarget, C3D_CLEAR_ALL, (u32)(ida * 4) << 24 | 0xFF, 0);
        C3D_FrameDrawOn(sIdTarget);
        C3D_SetViewport(vpx, vpy, vpw, vph);
        C3D_DepthTest(true, GPU_GREATER, GPU_WRITE_ALL);
        draw_ids(f, disp3d, &nv, 0);
        // a translucent pixel over an opaque one clears its edge flag (the opaque ID stays): G = 0
        C3D_DepthTest(true, GPU_GREATER, GPU_WRITE_GREEN);
        draw_ids(f, disp3d, &nv, 1);
        C3D_FrameDrawOn(s3dTarget);
        draw_edges();
        C3D_BindProgram(&sProg3d);
        attrs_3d();
        C3D_SetViewport(vpx, vpy, vpw, vph);
    }
    draw_polys(f, 1, disp3d, &nv);
    extern u32 port_test3d;
    if (port_test3d) {  // (orientation check: a red square in the DS picture's top-left quarter)
        C3D_TexEnv *env = C3D_GetTexEnv(0);
        C3D_TexEnvInit(env);
        C3D_TexEnvSrc(env, C3D_Both, GPU_PRIMARY_COLOR, 0, 0);
        C3D_TexEnvFunc(env, C3D_Both, GPU_REPLACE);
        C3D_DepthTest(false, GPU_ALWAYS, GPU_WRITE_COLOR);
        C3D_AlphaTest(false, GPU_ALWAYS, 0);
        blend_off();
        C3D_SetViewport((u32)(-sL * sScale), (u32)(-sT * sScale), (u32)(256 * sScale), (u32)(192 * sScale));
        static const float q[6][2] = { { -1, 0 }, { 0, 0 }, { -1, 1 }, { 0, 0 }, { 0, 1 }, { -1, 1 } };
        for (int k = 0; k < 6; k++) sVbo3d[nv + k] = (V3){ q[k][0], -q[k][1], -0.5f, 1, 1, 0, 0, 1, 0, 0 };
        C3D_DrawArrays(GPU_TRIANGLES, nv, 6);
        nv += 6;
    }
    s3dHave = 1;
    // back to the 2D compositor's state
    C3D_BindProgram(&sProg);
    attrs_2d();
    C3D_TexEnvInit(C3D_GetTexEnv(1));
}
