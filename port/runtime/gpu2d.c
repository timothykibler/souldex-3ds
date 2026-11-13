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
static C3D_RenderTarget *sTop, *sBottom, *sTopR;  // (sTopR: the top screen's right eye, stereoscopic 3D)
static int sStereo;  // this frame draws two eyes (stereoscopic 3D, below)
static int sRepeat;  // composing the top screen again (right eye): this frame's planes and sprites as they are
static void stereo_decide(int scene3d);

// ---------------------------------------------------------------- vertices
typedef struct { float x, y, u, v; } Vtx;
#define MAX_VTX (64 * 1024)
static Vtx *sVbo;
static int sVtxUsed;
static float sOX, sOY;  // (offset added to every quad: 0, the projection places the DS picture)

// Screen layout, two ways (the player's choice, in the ZL+ZR menu: settings.txt):
// - 1:1 (default): the DS picture's exact pixels in the middle of each screen, black around.
// - fill: the DS picture fills each screen's height, 1.25x (256x192 -> 320x240). 1.25 isn't a
//   whole number, so no fill can keep pixels exact: the screens are composed at twice their
//   size (sSS) and halved by the display transfer (a 2x2 box filter), so the 2D, drawn at 2.5x,
//   ends up at 1.25x with even pixels and slightly soft edges. Optionally the top screen's
//   sides continue the picture's edge columns (side fill) instead of black.
// Either way, on the top screen the overworld's 3D can be widened to fill all of it (400x240):
// the DS frustum is extended (clip x and y scaled by the DS picture's share of the screen), so
// what's inside the DS frame lands exactly where it would, at the same scale as the 2D, and the
// rest shows more of the scene; the 3D is drawn at the screen's own resolution.
// (debug.txt "scale1": 1:1 whatever the setting, for the comparison with the software renderer.)
int port_wide_mode = 1;    // 0 never, 1 when the scene is 3D-backed, 2 always (debug.txt "wide N")
int port_edge_mode = 1;    // edge marking (debug.txt "edge 0" turns it off)
int port_scale1;
int port_display_fill;     // 1.25x fill (else 1:1)
int port_side_fill;        // in fill mode, the top screen's sides continue the picture
int port_display_scaled(void) { return port_display_fill && !port_scale1; }
float port_g3_wide = 1.0f, port_g3_wide_y = 1.0f;  // view size over the DS's (the geometry engine's box test)
static float sScale = 1.25f;
static int sSS = 1;  // composition supersampling: 2 in fill mode, 1 (exact) in 1:1 mode
static float sL, sT, sW, sH;  // the screen being drawn, in DS pixels: left, top, width, height
static int sScrW;             // its width in screen pixels
static int sWide;             // this frame's 3D layer is widened
static int sMargins;          // the top screen's sides continue the DS picture's edge columns (not widened)
static C3D_Tex sShade;        // (the sides' darkening: alpha rising outwards)
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
    // what the text plane was last built from, to redraw only the tiles that changed
    u16 *map;        // map entries (w/8 * h/8)
    u8 *chars;       // tile data (1024 tiles)
    u32 *used;       // per tile: the colour indices its pixels use (256 bits; bit 0, transparent, never)
    u32 *seen;       // per tile: the frame its data was last compared
    u16 pal[256];    // palette as converted
    u16 *extpal;     // extended palette as read (16 x 256 colours)
    u32 cb, sb;
    int bpp8, built;
    const u8 *ext;
} Plane;
static u32 sPlaneFrame;
u64 gProfPlanes, gProfSprites, gProf3d, gProf3dTex, gProf3dWide, gProf3dIds;  // CPU ticks in BG planes, the sprite sheet, the 3D layer
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

// Text BG: the whole map (256 or 512 square) into its plane. Each frame the map, the tiles it
// uses and the palette are compared with what the plane was drawn from, and only tiles whose map
// entry, pixels or used colours changed are drawn again (a palette fade or colour cycle redraws
// just the tiles using those colours, also for 256-colour and extended-palette BGs).
static Plane *text_plane(const Eng *e, int bg) {
    u16 cnt = rd16(e->io + 8 + bg * 2);
    int size = cnt >> 14;
    int w = (size & 1) ? 512 : 256, h = (size & 2) ? 512 : 256;
    u32 cb = char_base(e, cnt), sb = screen_base(e, cnt);
    int bpp8 = (cnt >> 7) & 1;
    const u8 *ext = (bpp8 && (e->dispcnt & (1u << 30))) ? ext_palette(e, (bg < 2 && (cnt & 0x2000)) ? bg + 2 : bg) : NULL;
    Plane *p = &sPlane[!e->main][bg];
    if (sRepeat) return p->built ? p : NULL;
    if (!plane_alloc(p, w, h, true)) return NULL;
    if (!p->map) {
        p->map = malloc(64 * 64 * 2);
        p->chars = malloc(1024 * 64);
        p->used = malloc(1024 * 32);
        p->seen = calloc(1024, 4);
        p->extpal = malloc(16 * 256 * 2);
        if (!p->map || !p->chars || !p->used || !p->seen || !p->extpal) return NULL;
        p->built = 0;
    }
    u16 *data = p->tex.data;
    int ntx = w / 8, nty = h / 8;
    u32 tilebytes = bpp8 ? 64 : 32;
    int full = !p->built || p->cb != cb || p->sb != sb || p->bpp8 != bpp8 || p->ext != ext || p->hash != (u32)(w << 16 | h);
    // which colours changed: one 256-bit set, or one per extended sub-palette
    const u16 *pal = sPal[!e->main][0];
    static u32 chg[16][8];
    int anyChg = 0;
    if (ext) {
        memset(chg, 0, sizeof(chg));
        for (int i = 0; i < 4096; i++) {
            u16 c = rd16(ext + i * 2);
            if (c != p->extpal[i]) { chg[i >> 8][(i >> 5) & 7] |= 1u << (i & 31); p->extpal[i] = c; anyChg = 1; }
        }
    } else {
        memset(chg[0], 0, sizeof(chg[0]));
        for (int i = 0; i < 256; i++)
            if (pal[i] != p->pal[i]) { chg[0][i >> 5] |= 1u << (i & 31); p->pal[i] = pal[i]; anyChg = 1; }
    }
    if (full && !ext) memcpy(p->pal, pal, sizeof(p->pal));
    if (full && ext) for (int i = 0; i < 4096; i++) p->extpal[i] = rd16(ext + i * 2);
    p->cb = cb; p->sb = sb; p->bpp8 = bpp8; p->ext = ext; p->hash = (u32)(w << 16 | h); p->built = 1;
    u32 frame = ++sPlaneFrame;
    static u8 tileChanged[1024];
    int drawn = 0;
    for (int ty = 0; ty < nty; ty++) {
        for (int tx = 0; tx < ntx; tx++) {
            u32 block = (u32)((ty >> 5) * (w >> 8) + (tx >> 5));
            u16 ent = rd16(e->bgv + ((sb + block * 0x800 + ((ty & 31) * 32 + (tx & 31)) * 2) & (e->bgsize - 1)));
            u32 tile = ent & 0x3FF;
            const u8 *src = e->bgv + ((cb + tile * tilebytes) & (e->bgsize - 1));
            if (p->seen[tile] != frame) {  // (each tile's pixels compared once a frame)
                p->seen[tile] = frame;
                u8 *mine = p->chars + tile * 64;
                tileChanged[tile] = full || memcmp(mine, src, tilebytes) != 0;
                if (tileChanged[tile]) {
                    memcpy(mine, src, tilebytes);
                    u32 *u = p->used + tile * 8;
                    memset(u, 0, 32);
                    for (u32 i = 0; i < tilebytes; i++) {
                        if (bpp8) u[mine[i] >> 5] |= 1u << (mine[i] & 31);
                        else u[0] |= 1u << (mine[i] & 15) | 1u << (mine[i] >> 4);
                    }
                    u[0] &= ~1u;
                }
            }
            u16 *old = &p->map[ty * ntx + tx];
            int redraw = full || *old != ent || tileChanged[tile];
            if (!redraw && anyChg) {
                const u32 *u = p->used + tile * 8;
                if (!bpp8) redraw = ((chg[0][ent >> 13] >> ((ent >> 12 & 1) * 16)) & 0xFFFF & u[0]) != 0;
                else {
                    const u32 *c = chg[ext ? ent >> 12 : 0];
                    for (int k = 0; k < 8 && !redraw; k++) redraw = (c[k] & u[k]) != 0;
                }
            }
            if (!redraw) continue;
            *old = ent;
            drawn++;
            const u16 *pal16 = bpp8 ? pal : pal + (ent >> 12) * 16;
            const u8 *pal256 = ext ? ext + (ent >> 12) * 512 : NULL;
            put_tile(tex_tile(data, w, h, tx, ty), src, bpp8, (ent >> 10) & 1, (ent >> 11) & 1, pal16, pal256);
        }
    }
    if (drawn) {
        gGpu2dRebuilds++;
        C3D_TexFlush(&p->tex);
    }
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
    if (sRepeat) return p->tex.data ? p : NULL;
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

// ---------------------------------------------------------------- sprite sheet
// Every OAM entry has its own 64x64 cell (a 1024x512 sheet, 16 x 8 cells), drawn again only when
// what it shows changed: its attributes, its tile bytes or its palette (a hash of those).
#define SHEET_W 1024
#define SHEET_H 512
static C3D_Tex sSheet[2];
static u32 sCellKey[2][128][2];  // [engine][OAM index]: attributes, data hash (0: empty)

static bool sheet_alloc(int e) {
    if (sSheet[e].data) return true;
    if (!C3D_TexInit(&sSheet[e], SHEET_W, SHEET_H, GPU_RGBA5551)) return false;
    C3D_TexSetFilter(&sSheet[e], GPU_NEAREST, GPU_NEAREST);
    C3D_TexSetWrap(&sSheet[e], GPU_CLAMP_TO_EDGE, GPU_CLAMP_TO_EDGE);
    memset(sCellKey[e], 0, sizeof(sCellKey[e]));
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
    if (sRepeat) return;
    int ei = !e->main;
    sNumSpr[ei] = 0;
    if (!(e->dispcnt & 0x1000) && !(e->dispcnt & 0x8000)) return;
    if (!sheet_alloc(ei)) return;
    u16 *sheet = sSheet[ei].data;
    int sheetDirty = 0;
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
        int dstW = SHEET_W, dstH = SHEET_H;
        C3D_Tex *atex = NULL;
        if (affine) {
            atex = &sAffTex[ei][i];
            if (atex->data && (atex->width != w || atex->height != h)) { C3D_TexDelete(atex); atex->data = NULL; }
            if (!atex->data) {
                if (!C3D_TexInit(atex, (u16)w, (u16)h, GPU_RGBA5551)) continue;
                C3D_TexSetFilter(atex, GPU_NEAREST, GPU_NEAREST);
                C3D_TexSetWrap(atex, GPU_CLAMP_TO_BORDER, GPU_CLAMP_TO_BORDER);
                atex->border = 0;
            }
            dst = atex->data; dstW = w; dstH = h;
        } else {
            cx = (i & 15) * 64; cy = (i >> 4) * 64;  // (its own cell)
        }
        int ix = cx, iy = cy;
        int bpp8 = (a0 >> 13) & 1;
        u32 tile = a2 & 0x3FF;
        int pal = a2 >> 12;
        int hf = !affine && ((a1 >> 12) & 1), vf = !affine && ((a1 >> 13) & 1);
        const u16 *pal16 = bpp8 ? sPal[ei][1] : sPal[ei][1] + pal * 16;
        const u8 *pal256 = (bpp8 && ext) ? ext + pal * 512 : NULL;
        // drawn again only if what the cell shows changed
        u32 k0 = (u32)(a0 & 0xE300) | (u32)(affine ? 0 : a1 & 0x3000) << 16 | (u32)(a1 & 0xC000) << 2;
        u32 k1 = (u32)(a2 & 0xF3FF) | (u32)map1d << 16 | bshift << 17 | (u32)(ext != NULL) << 19;
        u32 tb = bpp8 ? 64 : 32, hsh = 2166136261u;
        if (map1d) hsh = hash_vram(hsh, e->objv, e->objsize, tile << (5 + bshift), (u32)(w / 8) * (h / 8) * tb);
        else for (int ty = 0; ty < h / 8; ty++) hsh = hash_vram(hsh, e->objv, e->objsize, tile * 32 + (u32)ty * 1024, (u32)(w / 8) * tb);
        hsh = pal256 ? hash_bytes(hsh, pal256, 512) : hash_bytes(hsh, (const u8 *)pal16, bpp8 ? 512 : 32);
        hsh = (hsh ^ k1) | 1;
        u32 *key = sCellKey[ei][i] ;
        int same = key[0] == k0 && key[1] == hsh && !(affine && sAffTex[ei][i].data != atex->data);
        if (!same) { key[0] = k0; key[1] = hsh; }
        for (int ty = 0; ty < h / 8 && !same; ty++) {
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
        if (!same) { if (atex) C3D_TexFlush(atex); else sheetDirty = 1; }
        if (affine) {
            const u8 *p = e->oam + ((a1 >> 9) & 31) * 32;
            s->m[0] = (int16_t)rd16(p + 6) / 256.0f; s->m[1] = (int16_t)rd16(p + 14) / 256.0f;
            s->m[2] = (int16_t)rd16(p + 22) / 256.0f; s->m[3] = (int16_t)rd16(p + 30) / 256.0f;
        }
    }
    if (sheetDirty) C3D_TexFlush(&sSheet[ei]);
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
// Stereoscopic 3D: the right eye's 3D layer and ID image, and which ones the compositor uses
static C3D_Tex s3dTexR, sIdTexR;
static C3D_Tex *s3dCur = &s3dTex, *sIdCur = &sIdTex;
static void render3d(int screenW);
static void flush_edge_vertices(void);
static bool init3d(void);

static void bg_quads(void *arg) {
    BgDraw *d = arg;
    const Eng *e = d->e;
    if (d->kind == 3) {  // the 3D layer: rendered at the screen's own resolution, the whole screen
        C3D_TexBind(0, port_edge_mode >= 2 && sIdTarget ? sIdCur : s3dCur);
        if (sWide) scissor_ds(1);
        quad(sL, sT, sL + sW, sT + sH, 0, 0, sScrW / 512.0f, 240 / 256.0f);
        if (sWide) scissor_ds(0);
        if (sMargins) {  // (its edge columns, in screen pixels)
            float uL = ((0 - sL) * sScale + 0.5f) / 512, uR = ((256 - sL) * sScale - 0.5f) / 512;
            scissor_ds(1);
            quad(sL, 0, 0, 192, uL, (0 - sT) * sScale / 256, uL, (192 - sT) * sScale / 256);
            quad(256, 0, sL + sW, 192, uR, (0 - sT) * sScale / 256, uR, (192 - sT) * sScale / 256);
            scissor_ds(0);
        }
        return;
    }
    Plane *p = d->p;
    C3D_TexBind(0, &p->tex);
    if (d->kind == 0) {
        float hofs = rd16(e->io + 0x10 + d->bg * 4) & 0x1FF, vofs = rd16(e->io + 0x12 + d->bg * 4) & 0x1FF;
        quad(0, 0, 256, 192, hofs / p->w, vofs / p->h, (hofs + 256) / p->w, (vofs + 192) / p->h);
        if (sMargins) {  // (each row's outermost pixel, carried out to the screen's edges)
            float uL = (hofs + 0.5f) / p->w, uR = (hofs + 255.5f) / p->w;
            scissor_ds(1);
            quad(sL, 0, 0, 192, uL, vofs / p->h, uL, (vofs + 192) / p->h);
            quad(256, 0, sL + sW, 192, uR, vofs / p->h, uR, (vofs + 192) / p->h);
            scissor_ds(0);
        }
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
        if (sMargins) {  // (the edge columns: the source point of x = 0 and x = 255, down the rows)
            float e0[2][2], e1[2][2];
            for (int k = 0; k < 2; k++) {
                float sy = k ? 191.5f : -0.5f;
                e0[k][0] = (X + pb * sy) / p->w; e0[k][1] = (Y + pd * sy) / p->h;
                e1[k][0] = (X + pa * 255 + pb * sy) / p->w; e1[k][1] = (Y + pc * 255 + pd * sy) / p->h;
            }
            const float L[4][2] = { { sL, 0 }, { 0, 0 }, { sL, 192 }, { 0, 192 } };
            const float R[4][2] = { { 256, 0 }, { sL + sW, 0 }, { 256, 192 }, { sL + sW, 192 } };
            const float uvL[4][2] = { { e0[0][0], e0[0][1] }, { e0[0][0], e0[0][1] }, { e0[1][0], e0[1][1] }, { e0[1][0], e0[1][1] } };
            const float uvR[4][2] = { { e1[0][0], e1[0][1] }, { e1[0][0], e1[0][1] }, { e1[1][0], e1[1][1] }, { e1[1][0], e1[1][1] } };
            scissor_ds(1);
            quad4(L, uvL);
            quad4(R, uvR);
            scissor_ds(0);
        }
    }
}

typedef struct { int ei; const Spr *s; } SprDraw;
static void spr_quads(void *arg) {
    SprDraw *d = arg;
    const Spr *s = d->s;
    C3D_TexBind(0, &sSheet[d->ei]);
    const float SW = SHEET_W, SH = SHEET_H;
    if (!s->affine) {
        // (a sliver of a texel in from the cell's edge: at non-integer scales a pixel on the quad's
        // edge mustn't sample the next cell)
        const float in = 1.0f / 64;
        quad(s->x, s->y, s->x + s->w, s->y + s->h, (s->sx + in) / SW, (s->sy + in) / SH, (s->sx + s->w - in) / SW, (s->sy + s->h - in) / SH);
        if (sMargins && (s->x <= 0 || s->x + s->w >= 256)) {  // (a sprite at an edge: its edge column carried out)
            scissor_ds(1);
            if (s->x <= 0) {
                float u = (s->sx + (0 - s->x) + 0.5f) / SW;
                quad(sL, s->y, 0, s->y + s->h, u, (s->sy + in) / SH, u, (s->sy + s->h - in) / SH);
            }
            if (s->x + s->w >= 256) {
                float u = (s->sx + (255 - s->x) + 0.5f) / SW;
                quad(256, s->y, sL + sW, s->y + s->h, u, (s->sy + in) / SH, u, (s->sy + s->h - in) / SH);
            }
            scissor_ds(0);
        }
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
    if (!sRepeat) {
        port_render2d_engine_sw(ei, img);
        for (int y = 0; y < 192; y++)
            for (int x = 0; x < 256; x++) *tex_px(fb.data, 256, 256, x, y) = sColor[img[y * 256 + x] & 0x7FFF];
        C3D_TexFlush(&fb);
    }
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
    if (whole) { C3D_SetScissor(GPU_SCISSOR_NORMAL, 0, 0, 240 * sSS, (u32)sScrW * sSS); return; }
    int x0 = (int)(-sL * sScale * sSS + 0.5f), y0 = (int)(-sT * sScale * sSS + 0.5f);
    int w = (int)(256 * sScale * sSS + 0.5f), h = (int)(192 * sScale * sSS + 0.5f);
    C3D_SetScissor(GPU_SCISSOR_NORMAL, (u32)(240 * sSS - (y0 + h)), (u32)x0, (u32)(240 * sSS - y0), (u32)(x0 + w));
}

static void layout(int screenW) {
    sScale = port_display_scaled() ? 1.25f : 1.0f;
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
    sMargins = screenW == 400 && !wide3d && sScale > 1.0f && port_side_fill;  // (the top screen's sides, when the 3D doesn't fill them)
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
    if (wide3d || sMargins) { scissor_ds(1); flat(bgr8_abgr(backdrop), (int)sL - 1, (int)sT - 1, (int)(sL + sW) + 1, (int)(sT + sH) + 1); scissor_ds(0); }
    else flat(bgr8_abgr(backdrop), 0, 0, 256, 192);
    C3D_StencilTest(false, GPU_ALWAYS, 0, 0, 0);
    if (dmode >= 2) { engine_from_software(ei); return; }
    { u64 t = svcGetSystemTick(); sprites_build(e); gProfSprites += svcGetSystemTick() - t; }
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
        u64 tp = svcGetSystemTick();
        Plane *p = kind == 0 ? text_plane(e, bg) : affine_plane(e, bg, kind == 2, &wrap);
        gProfPlanes += svcGetSystemTick() - tp;
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
        if (wide3d || sMargins) { scissor_ds(1); fill((int)sL - 1, (int)sT - 1, (int)(sL + sW) + 1, (int)(sT + sH) + 1); scissor_ds(0); }
        else fill(0, 0, 256, 192);
        blend_off();
    }
    C3D_StencilTest(false, GPU_ALWAYS, 0, 0, 0);
    if (sMargins && sShade.data) {  // the sides darken outwards (they're a continuation, not the picture)
        C3D_AlphaTest(false, GPU_ALWAYS, 0);
        C3D_TexEnv *env = C3D_GetTexEnv(0);
        C3D_TexEnvInit(env);
        C3D_TexEnvSrc(env, C3D_Both, GPU_TEXTURE0, 0, 0);
        C3D_TexEnvFunc(env, C3D_Both, GPU_REPLACE);
        C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_SRC_ALPHA, GPU_ONE_MINUS_SRC_ALPHA, GPU_ZERO, GPU_ONE);
        C3D_TexBind(0, &sShade);
        scissor_ds(1);
        quad(sL, sT, 0, sT + sH, 1, 0, 0, 1);         // (u: 0 at the picture, 1 at the screen's edge)
        quad(256, sT, sL + sW, sT + sH, 0, 0, 1, 1);
        scissor_ds(0);
        blend_off();
    }
}

// ---------------------------------------------------------------- frames
static bool sReady;

// The display transfer from a composed screen to its framebuffer: halved (2x2 box filter) when
// it was composed at twice the size.
static u32 screen_transfer(void) {
    return GX_TRANSFER_FLIP_VERT(0) | GX_TRANSFER_OUT_TILED(0) | GX_TRANSFER_RAW_COPY(0) |
           GX_TRANSFER_IN_FORMAT(GX_TRANSFER_FMT_RGB8) | GX_TRANSFER_OUT_FORMAT(GX_TRANSFER_FMT_RGB8) |
           GX_TRANSFER_SCALING(sSS == 2 ? GX_TRANSFER_SCALE_XY : GX_TRANSFER_SCALE_NO);
}

// The screens' render targets at a supersampling factor (made again when the display mode
// changes: VRAM holds one size at a time).
static void stereo_free(void);
static bool make_targets(int ss) {
    stereo_free();  // (its VRAM: the larger targets may need it)
    if (sTop) C3D_RenderTargetDelete(sTop);
    if (sBottom) C3D_RenderTargetDelete(sBottom);
    sSS = ss;
    sTop = C3D_RenderTargetCreate(240 * ss, 400 * ss, GPU_RB_RGB8, GPU_RB_DEPTH24_STENCIL8);
    sBottom = C3D_RenderTargetCreate(240 * ss, 320 * ss, GPU_RB_RGB8, GPU_RB_DEPTH24_STENCIL8);
    if (!sTop || !sBottom) { port_log("gpu: no screen targets (vram free %lu)\n", vramSpaceFree()); return false; }
    C3D_RenderTargetSetOutput(sTop, GFX_TOP, GFX_LEFT, screen_transfer());
    C3D_RenderTargetSetOutput(sBottom, GFX_BOTTOM, GFX_LEFT, screen_transfer());
    return true;
}

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
    // (the screens at twice their size, 24-bit colour: 16-bit rounds each blend step, and layered
    // blends drift from the DS's. Each has its own depth-stencil buffer: citro3d clears every
    // buffer at the frame's start, so a shared one would reach the second screen used.)
    if (!make_targets(2)) return false;  // (the larger size first: the 3D layer's targets come after)
    if (!init3d()) port_log("gpu: no 3D layer\n");
    if (C3D_TexInit(&sShade, 64, 8, GPU_RGBA4)) {  // (black, alpha 0 -> about 60 % across)
        C3D_TexSetFilter(&sShade, GPU_LINEAR, GPU_LINEAR);
        C3D_TexSetWrap(&sShade, GPU_CLAMP_TO_EDGE, GPU_CLAMP_TO_EDGE);
        for (int y = 0; y < 8; y++)
            for (int x = 0; x < 64; x++) *tex_px(sShade.data, 64, 8, x, y) = (u16)((x * x * 10 + 63 * 63 / 2) / (63 * 63));
        C3D_TexFlush(&sShade);
    }
    port_log("gpu: VRAM free %lu\n", (unsigned long)vramSpaceFree());
    port_log("gpu: ready\n");
    sReady = true;
    return true;
}

// The bottom screen lent to a text console (the cheat menu) and given back.
void port_gpu2d_bottom_console(int console) {
    if (!sReady) return;
    if (console) {
        C3D_FrameSync();
        C3D_RenderTargetSetOutput(NULL, GFX_BOTTOM, GFX_LEFT, 0);
    } else {
        gfxSetScreenFormat(GFX_BOTTOM, GSP_BGR8_OES);
        gfxSetDoubleBuffering(GFX_BOTTOM, true);
        C3D_RenderTargetSetOutput(sBottom, GFX_BOTTOM, GFX_LEFT, screen_transfer());
    }
}

// Draws both engines onto their screens (engine A on top when POWCNT1 bit 15 says so).
static int sReadable;  // (screenshot mode: single-buffered screens, so what's shown can be read)

u64 gGpu2dBusyTicks;
u64 gGpu2dWaitTicks;  // waiting in C3D_FrameBegin (the GPU's previous frame, the frame-rate limit)  // CPU time building the frame (not waiting for the GPU or the VBlank)
u64 gGpu2dMaxTicks;   // the slowest frame
u32 gGpu2dRebuilds;   // BG textures rebuilt

void port_gpu2d_frame(void) {
    if (!sReady) return;
    if ((port_display_scaled() ? 2 : 1) != sSS) {  // (the display mode changed: targets of the other size)
        C3D_FrameBegin(C3D_FRAME_SYNCDRAW);  // (the GPU done with the old ones)
        C3D_FrameEnd(0);
        C3D_FrameBegin(C3D_FRAME_SYNCDRAW);
        C3D_FrameEnd(0);
        make_targets(port_display_scaled() ? 2 : 1);
    }
    u64 tw = svcGetSystemTick();
    C3D_FrameBegin(C3D_FRAME_SYNCDRAW);
    u64 t0 = svcGetSystemTick();
    gGpu2dWaitTicks += t0 - tw;
    sVtxUsed = 0;
    C3D_BindProgram(&sProg);
    static Eng a, b;
    a = (Eng){ 1, DS(0x04000000), DS(0x05000000), DS(0x05000200), DS(0x07000000), DS(0x06000000), DS(0x06400000), 0x80000, 0x40000, 0 };
    b = (Eng){ 0, DS(0x04001000), DS(0x05000400), DS(0x05000600), DS(0x07000400), DS(0x06200000), DS(0x06600000), 0x20000, 0x20000, 0 };
    int a_on_top = (rd16(DS(0x04000304)) >> 15) & 1;
    stereo_decide(a_on_top && (rd32(DS(0x04000000)) & 0x108) == 0x108);  // (engine A's 3D layer shown, on the top screen)
    { u64 t = svcGetSystemTick(); render3d(a_on_top ? 400 : 320); gProf3d += svcGetSystemTick() - t; }
    C3D_RenderTargetClear(sTop, C3D_CLEAR_ALL, 0, 0);  // (0: black in any colour format)
    C3D_RenderTargetClear(sBottom, C3D_CLEAR_ALL, 0, 0);
    render_engine(&a, a_on_top ? sTop : sBottom, a_on_top ? 400 : 320);
    render_engine(&b, a_on_top ? sBottom : sTop, a_on_top ? 320 : 400);
    if (sStereo && sTopR) {  // the right eye: the top screen again, with the right eye's 3D layer
        C3D_RenderTargetClear(sTopR, C3D_CLEAR_ALL, 0, 0);
        s3dCur = &s3dTexR; sIdCur = &sIdTexR; sRepeat = 1;
        render_engine(&a, sTopR, 400);
        s3dCur = &s3dTex; sIdCur = &sIdTex; sRepeat = 0;
    }
    u64 dt = svcGetSystemTick() - t0;
    gGpu2dBusyTicks += dt;
    if (dt > gGpu2dMaxTicks) gGpu2dMaxTicks = dt;
    // (vertices the CPU wrote this frame, out of its data cache before the GPU reads them)
    GSPGPU_FlushDataCache(sVbo, sVtxUsed * sizeof(Vtx));
    flush_edge_vertices();
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
    // (the frame just drawn, all the way to the framebuffers: C3D_FrameBegin waits for the whole
    // previous queue, its display transfers included; an empty frame transfers nothing)
    C3D_FrameBegin(C3D_FRAME_SYNCDRAW);
    C3D_FrameEnd(0);
    C3D_FrameBegin(C3D_FRAME_SYNCDRAW);
    C3D_FrameEnd(0);
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
    for (int eye = 0; eye < 1 + (sStereo != 0); eye++) {  // (with stereo also full_N_r: the right eye on top)
    char path[96];
    snprintf(path, sizeof(path), "sdmc:/3ds/soulsilver-3ds/full_%05lu%s.bmp", (unsigned long)frame, eye ? "_r" : "");
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
        const u8 *fb = gfxGetFramebuffer(s ? GFX_BOTTOM : GFX_TOP, s || !eye ? GFX_LEFT : GFX_RIGHT, &fw, &fh);
        int ox = (W - fh) / 2;
        for (int x = 0; x < fh; x++) memcpy(row + (x + ox) * 3, fb + (x * 240 + (239 - sy)) * 3, 3);
        fwrite(row, 1, rowb, f);
    }
    fclose(f);
    }
}

// For debugging: an engine's display registers and visible sprites.
void port_gpu2d_describe(int engine) {
    {   // what each BG's map, tiles and palette hold (non-zero bytes)
        Eng e = engine ? (Eng){ 0, DS(0x04001000), DS(0x05000400), DS(0x05000600), DS(0x07000400), DS(0x06200000), DS(0x06600000), 0x20000, 0x20000, 0 }
                       : (Eng){ 1, DS(0x04000000), DS(0x05000000), DS(0x05000200), DS(0x07000000), DS(0x06000000), DS(0x06400000), 0x80000, 0x40000, 0 };
        e.dispcnt = rd32(e.io);
        u32 pz = 0;
        for (int i = 0; i < 512; i++) pz += e.bgpal[i] != 0;
        port_log("  engine %c BG palette: %lu/512 bytes non-zero\n", engine ? 'B' : 'A', (unsigned long)pz);
        for (int bg = 0; bg < 4; bg++) {
            u16 cnt = rd16(e.io + 8 + bg * 2);
            u32 sb = screen_base(&e, cnt), cb = char_base(&e, cnt), mz = 0, cz = 0;
            for (u32 i = 0; i < 0x1000; i++) mz += e.bgv[(sb + i) & (e.bgsize - 1)] != 0;
            for (u32 i = 0; i < 0x8000; i++) cz += e.bgv[(cb + i) & (e.bgsize - 1)] != 0;
            port_log("    BG%d: map @%05lX %lu/4096 non-zero, tiles @%05lX %lu/32768 non-zero\n", bg, (unsigned long)sb, (unsigned long)mz, (unsigned long)cb, (unsigned long)cz);
        }
    }
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
// The geometry engine's polygons (gx3d.c), drawn into an image of the screen's size with depth,
// which the 2D compositor then shows as BG0. The vertices are drawn where the geometry engine
// wrote them (GPU-visible memory): the vertex shaders (poly.v.pica, polyid.v.pica) do what's
// left of the conversion, so the CPU touches no vertex. DS textures are decoded from the texture
// slots (the VRAM banks mapped for textures, read where the game wrote them: through LCDC).

extern const u8 poly_shbin[];
extern const u32 poly_shbin_size;
extern const u8 polyid_shbin[];
extern const u32 polyid_shbin_size;
static DVLB_s *sDvlbId;
static shaderProgram_s sProgId;
static int sLocXform, sLocTc, sLocXformId, sLocTcId, sLocIdp, sLocStereo, sLocStereoId;
extern const u8 edge_shbin[];
extern const u32 edge_shbin_size;
static DVLB_s *sDvlb3d, *sDvlbEdge;
static shaderProgram_s sProg3d, sProgEdge;
static C3D_RenderTarget *s3dTarget, *s3dTargetR, *sIdTargetR;
// Edge marking: each opaque pixel's polygon ID (R, as ID * 4) and depth (G, larger = nearer),
// drawn into its own image against the 3D layer's depth buffer.
typedef struct { float x, y, z, w, u0, v0, u1, v1; } VE;
static VE *sVboEdge;
static int s3dReady;
static void flush_edge_vertices(void) { if (sVboEdge) GSPGPU_FlushDataCache(sVboEdge, sizeof(VE) * 6 * 8); }


static void attrs_2d(void) {
    C3D_AttrInfo *ai = C3D_GetAttrInfo();
    AttrInfo_Init(ai);
    AttrInfo_AddLoader(ai, 0, GPU_FLOAT, 2);
    AttrInfo_AddLoader(ai, 1, GPU_FLOAT, 2);
    C3D_BufInfo *bi = C3D_GetBufInfo();
    BufInfo_Init(bi);
    BufInfo_Add(bi, sVbo, sizeof(Vtx), 2, 0x10);
}

// The geometry engine's vertices as they are (G3Vtx): position float4, colour 4 bytes, texels float2.
static void attrs_3d(const G3Frame *f) {
    C3D_AttrInfo *ai = C3D_GetAttrInfo();
    AttrInfo_Init(ai);
    AttrInfo_AddLoader(ai, 0, GPU_FLOAT, 4);
    AttrInfo_AddLoader(ai, 1, GPU_UNSIGNED_BYTE, 4);
    AttrInfo_AddLoader(ai, 2, GPU_FLOAT, 2);
    C3D_BufInfo *bi = C3D_GetBufInfo();
    BufInfo_Init(bi);
    BufInfo_Add(bi, f->v, sizeof(G3Vtx), 3, 0x210);
}

static bool init3d(void) {
    sDvlb3d = DVLB_ParseFile((u32 *)poly_shbin, poly_shbin_size);
    shaderProgramInit(&sProg3d);
    shaderProgramSetVsh(&sProg3d, &sDvlb3d->DVLE[0]);
    sLocXform = shaderInstanceGetUniformLocation(sProg3d.vertexShader, "xform");
    sLocTc = shaderInstanceGetUniformLocation(sProg3d.vertexShader, "tcscale");
    sLocStereo = shaderInstanceGetUniformLocation(sProg3d.vertexShader, "stereo");
    sDvlbId = DVLB_ParseFile((u32 *)polyid_shbin, polyid_shbin_size);
    shaderProgramInit(&sProgId);
    shaderProgramSetVsh(&sProgId, &sDvlbId->DVLE[0]);
    sLocXformId = shaderInstanceGetUniformLocation(sProgId.vertexShader, "xform");
    sLocTcId = shaderInstanceGetUniformLocation(sProgId.vertexShader, "tcscale");
    sLocIdp = shaderInstanceGetUniformLocation(sProgId.vertexShader, "idp");
    sLocStereoId = shaderInstanceGetUniformLocation(sProgId.vertexShader, "stereo");
    if (!C3D_TexInitVRAM(&s3dTex, 512, 256, GPU_RGBA8)) return false;
    C3D_TexSetFilter(&s3dTex, GPU_NEAREST, GPU_NEAREST);
    s3dTarget = C3D_RenderTargetCreateFromTex(&s3dTex, GPU_TEXFACE_2D, 0, GPU_RB_DEPTH24_STENCIL8);
    if (!s3dTarget) return false;
    sDvlbEdge = DVLB_ParseFile((u32 *)edge_shbin, edge_shbin_size);
    shaderProgramInit(&sProgEdge);
    shaderProgramSetVsh(&sProgEdge, &sDvlbEdge->DVLE[0]);
    sVboEdge = linearAlloc(sizeof(VE) * 6 * 8);
    if (sVboEdge && C3D_TexInitVRAM(&sIdTex, 512, 256, GPU_RGB8)) {  // (ID and depth: no alpha)
        C3D_TexSetFilter(&sIdTex, GPU_NEAREST, GPU_NEAREST);
        C3D_TexSetWrap(&sIdTex, GPU_CLAMP_TO_EDGE, GPU_CLAMP_TO_EDGE);
        // (its own depth buffer: an "equal" test against the 3D pass's fails on alternate
        // columns, the PICA doesn't reproduce depths bit for bit between passes)
        sIdTarget = C3D_RenderTargetCreateFromTex(&sIdTex, GPU_TEXFACE_2D, 0, GPU_RB_DEPTH16);
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
// Decoded textures, cached per (texture, palette). Only the part the frame's polygons use is
// decoded (their texture coordinates' bounds, in 8x8 tiles; the whole texture when they repeat),
// and a cached part is decoded again when the texel rows or the palette it came from change.
// Formats with 1-bit alpha decode exactly to RGBA5551; A3I5 and A5I3 need RGBA8.
typedef struct {
    u32 key, pltt, hash, hashFrame, used;
    int w, h, fmt;
    int r0, r1, c0, c1;  // decoded tile rows [r0, r1), columns [c0, c1) (empty: r0 == r1)
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
static int tex_wide(int fmt) { return fmt == 1 || fmt == 6; }  // (RGBA8 texels)

// Tile rows [r0, r1) x columns [c0, c1) of the texture into its texels.
static void decode_texture(void *out, int w, int h, int fmt, int color0, u32 addr, u32 pltt, int r0, int r1, int c0, int c1) {
    const u8 *src = tex_ptr(addr);
    u32 paddr = fmt == 2 ? pltt << 3 : pltt << 4;
    const u8 *pal = pal_ptr(paddr);
    int wide = tex_wide(fmt);
    #define TILE(tx, ty) ((H8 - 1 - (ty)) * W8 + (tx)) * 64
    const int W8 = w >> 3, H8 = h >> 3;
    if (!src || (fmt != 7 && !pal)) {
        for (int ty = r0; ty < r1; ty++) for (int tx = c0; tx < c1; tx++) for (int k = 0; k < 64; k++) {
            if (wide) ((u32 *)out)[TILE(tx, ty) + k] = 0xFF00FFFF; else ((u16 *)out)[TILE(tx, ty) + k] = 0xF83F;  // (missing: magenta)
        }
        return;
    }
    if (fmt == 5) {  // 4x4 compressed: 32-bit texel blocks, 16-bit palette info in slot 1
        u32 iaddr = 0x20000 + ((addr & 0x1FFFF) >> 1) + ((addr >> 17) == 2 ? 0x10000 : 0);
        const u8 *idx = tex_ptr(iaddr);
        u16 *o = out;
        for (int by = r0 * 2; by < r1 * 2; by++) {
            for (int bx = c0 * 2; bx < c1 * 2; bx++) {
                int blk = by * (w / 4) + bx;
                u32 bits = rd32(src + blk * 4);
                u16 info = idx ? rd16(idx + blk * 2) : 0;
                const u8 *pb = pal_ptr(paddr + (info & 0x3FFF) * 4);
                u16 c[4] = { 0, 0, 0, 0 };
                if (pb) {
                    u16 k0 = rd16(pb), k1 = rd16(pb + 2);
                    int mode = info >> 14;
                    c[0] = sColor[k0 & 0x7FFF]; c[1] = sColor[k1 & 0x7FFF];
                    #define MIX(a, b, wa, wb, d) (((((a) & 31) * (wa) + ((b) & 31) * (wb)) / (d)) | (((((a) >> 5) & 31) * (wa) + (((b) >> 5) & 31) * (wb)) / (d)) << 5 | (((((a) >> 10) & 31) * (wa) + (((b) >> 10) & 31) * (wb)) / (d)) << 10)
                    if (mode == 0) c[2] = sColor[rd16(pb + 4) & 0x7FFF];
                    else if (mode == 1) c[2] = sColor[MIX(k0, k1, 1, 1, 2) & 0x7FFF];
                    else if (mode == 2) { c[2] = sColor[rd16(pb + 4) & 0x7FFF]; c[3] = sColor[rd16(pb + 6) & 0x7FFF]; }
                    else { c[2] = sColor[MIX(k0, k1, 5, 3, 8) & 0x7FFF]; c[3] = sColor[MIX(k0, k1, 3, 5, 8) & 0x7FFF]; }
                    #undef MIX
                }
                u16 *t = o + TILE(bx >> 1, by >> 1);
                for (int y = 0; y < 4; y++)
                    for (int x = 0; x < 4; x++)
                        t[sMorton[(7 - ((by & 1) * 4 + y)) * 8 + (bx & 1) * 4 + x]] = c[(bits >> ((y * 4 + x) * 2)) & 3];
            }
        }
        return;
    }
    // the palette (or, for the 8-bit formats with alpha, every byte value) to texels, once
    static u32 lut32[256];
    static u16 lut16[256];
    int nlut = fmt == 2 ? 4 : fmt == 3 ? 16 : 256;
    for (int v = 0; fmt != 7 && v < nlut; v++) {
        if (fmt == 1) { int a = v >> 5; lut32[v] = rgba(rd16(pal + (v & 31) * 2), (u8)((a << 5) | (a << 2) | (a >> 1))); }
        else if (fmt == 6) { int a = v >> 3; lut32[v] = rgba(rd16(pal + (v & 7) * 2), (u8)(a << 3 | a >> 2)); }
        else lut16[v] = (color0 && !v) ? 0 : sColor[rd16(pal + v * 2) & 0x7FFF];
    }
    for (int ty = r0; ty < r1; ty++) {
        for (int tx = c0; tx < c1; tx++) {
            u32 base = TILE(tx, ty);
            for (int y = 0; y < 8; y++) {
                const u8 *m = sMorton + (7 - y) * 8;
                u32 i0 = (u32)(ty * 8 + y) * w + tx * 8;  // (first texel of this tile row)
                switch (fmt) {
                case 1: case 6: { const u8 *s8 = src + i0; u32 *o = (u32 *)out + base; for (int x = 0; x < 8; x++) o[m[x]] = lut32[s8[x]]; break; }
                case 4: { const u8 *s8 = src + i0; u16 *o = (u16 *)out + base; for (int x = 0; x < 8; x++) o[m[x]] = lut16[s8[x]]; break; }
                case 3: { const u8 *s4 = src + (i0 >> 1); u16 *o = (u16 *)out + base;
                          for (int x = 0; x < 8; x += 2) { u8 b = s4[x >> 1]; o[m[x]] = lut16[b & 15]; o[m[x + 1]] = lut16[b >> 4]; } break; }
                case 2: { const u8 *s2 = src + (i0 >> 2); u16 *o = (u16 *)out + base;
                          for (int x = 0; x < 8; x++) o[m[x]] = lut16[(s2[x >> 2] >> ((x & 3) * 2)) & 3]; break; }
                case 7: { const u8 *s16 = src + i0 * 2; u16 *o = (u16 *)out + base;
                          for (int x = 0; x < 8; x++) { u16 v = rd16(s16 + x * 2); o[m[x]] = (v & 0x8000) ? sColor[v & 0x7FFF] : 0; } break; }
                }
            }
        }
    }
    #undef TILE
}

// The texture for a polygon, decoded at least over texels [s0, s1) x [t0, t1) (in texels).
static C3D_Tex *texture_for_(u32 teximage, u32 pltt, int s0, int s1, int t0, int t1);
static C3D_Tex *texture_for(u32 teximage, u32 pltt, int s0, int s1, int t0, int t1) {
    u64 t = svcGetSystemTick();
    C3D_Tex *r = texture_for_(teximage, pltt, s0, s1, t0, t1);
    gProf3dTex += svcGetSystemTick() - t;
    return r;
}
static C3D_Tex *texture_for_(u32 teximage, u32 pltt, int s0, int s1, int t0, int t1) {
    int fmt = (teximage >> 26) & 7;
    if (!fmt) return NULL;
    int w = 8 << ((teximage >> 20) & 7), h = 8 << ((teximage >> 23) & 7);
    u32 key = teximage & 0x3FFFFFFF & ~(0xFu << 16);  // (address, size, format, colour 0: not the wrap bits)
    u32 addr = (teximage & 0xFFFF) << 3;
    // the tiles needed (a texel's margin either side)
    int c0 = (s0 - 1) >> 3, c1 = (s1 + 8) >> 3, r0 = (t0 - 1) >> 3, r1 = (t1 + 8) >> 3;
    if (c0 < 0) c0 = 0;
    if (r0 < 0) r0 = 0;
    if (c1 > w >> 3) c1 = w >> 3;
    if (r1 > h >> 3) r1 = h >> 3;
    if (fmt == 5) { r0 = c0 = 0; r1 = h >> 3; c1 = w >> 3; }
    if (c0 >= c1 || r0 >= r1) { r0 = c0 = 0; r1 = h >> 3; c1 = w >> 3; }
    TexEntry *e = NULL, *lru = &sTexCache[0];
    for (int i = 0; i < TEX_CACHE; i++) {
        TexEntry *t = &sTexCache[i];
        if (t->tex.data && t->key == key && t->pltt == pltt) { e = t; break; }
        if (t->used < lru->used) lru = t;
    }
    if (!e) {
        e = lru;
        if (e->tex.data && (e->w != w || e->h != h || tex_wide(e->fmt) != tex_wide(fmt))) { C3D_TexDelete(&e->tex); e->tex.data = NULL; }
        if (!e->tex.data) {
            if (!C3D_TexInit(&e->tex, (u16)w, (u16)h, tex_wide(fmt) ? GPU_RGBA8 : GPU_RGBA5551)) return NULL;
            C3D_TexSetFilter(&e->tex, GPU_NEAREST, GPU_NEAREST);
        }
        e->key = key; e->pltt = pltt; e->w = w; e->h = h; e->fmt = fmt; e->hash = 0; e->hashFrame = 0;
        e->r0 = e->r1 = e->c0 = e->c1 = 0;
    }
    e->used = sFrameNo;
    int have = e->r0 < e->r1 && r0 >= e->r0 && r1 <= e->r1 && c0 >= e->c0 && c1 <= e->c1;
    if (e->hashFrame == sFrameNo && have) return &e->tex;
    // the region to keep decoded: what was, and what's asked for now
    if (e->r0 < e->r1) {
        if (e->r0 < r0) r0 = e->r0;
        if (e->r1 > r1) r1 = e->r1;
        if (e->c0 < c0) c0 = e->c0;
        if (e->c1 > c1) c1 = e->c1;
    }
    // (the data can change any frame: its rows and the palette hashed once a frame)
    u32 hsh = 2166136261u;
    const u8 *src = tex_ptr(addr);
    if (src) {
        u32 rowb = tex_bytes(fmt, w, 8);  // (8 texel rows)
        if (fmt == 5) hsh = hash_bytes(hsh, src, tex_bytes(fmt, w, h) & ~3u);
        else hsh = hash_bytes(hsh, src + r0 * rowb, ((r1 - r0) * rowb) & ~3u);
    }
    const u8 *pal = pal_ptr(fmt == 2 ? pltt << 3 : pltt << 4);
    if (pal && pal_bytes(fmt)) hsh = hash_bytes(hsh, pal, pal_bytes(fmt));
    hsh |= 1;
    int same = hsh == e->hash && r0 == e->r0 && r1 == e->r1 && c0 == e->c0 && c1 == e->c1;
    e->hashFrame = sFrameNo;
    if (!same) {
        e->hash = hsh;
        e->r0 = r0; e->r1 = r1; e->c0 = c0; e->c1 = c1;
        decode_texture(e->tex.data, w, h, fmt, (teximage >> 29) & 1, addr, pltt, r0, r1, c0, c1);
        C3D_TexFlush(&e->tex);
    }
    return &e->tex;
}

// A polygon's texture-coordinate bounds (texels, from the geometry engine); beyond the texture
// (repeating): all of it on that axis.
static void poly_st_bounds(const G3Frame *f, const G3Poly *p, int *s0, int *s1, int *t0, int *t1) {
    (void)f;
    int w = 8 << ((p->teximage >> 20) & 7), h = 8 << ((p->teximage >> 23) & 7);
    *s0 = (int)p->s0; *s1 = (int)p->s1 + 1; *t0 = (int)p->t0; *t1 = (int)p->t1 + 1;
    if (p->s0 < 0 || p->s1 > w) { *s0 = 0; *s1 = w; }
    if (p->t0 < 0 || p->t1 > h) { *t0 = 0; *t1 = h; }
}

static GPU_TEXTURE_WRAP_PARAM wrap_of(int repeat, int flip) {
    return !repeat ? GPU_CLAMP_TO_EDGE : flip ? GPU_MIRRORED_REPEAT : GPU_REPEAT;
}

// ---------------------------------------------------------------- drawing a frame
static float sClipX = 1.0f, sClipY = 1.0f;  // clip-space scale (widening)
static void draw_polys(const G3Frame *f, int translucent, u32 disp3d) {
    for (u32 i = 0; i < f->np; i++) {
        const G3Poly *p = &f->p[i];
        if (p->translucent != translucent) continue;
        int fmt = (p->teximage >> 26) & 7;
        C3D_Tex *tex = NULL;
        if (fmt && (disp3d & 1)) {
            int s0, s1, t0, t1;
            poly_st_bounds(f, p, &s0, &s1, &t0, &t1);
            tex = texture_for(p->teximage, p->pltt, s0, s1, t0, t1);
        }
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
        if (tex) C3D_FVUnifSet(GPU_VERTEX_SHADER, sLocTc, 1.0f / tex->width, 1.0f / tex->height, 0, 0);
        C3D_DrawArrays(GPU_TRIANGLES, (int)p->first, (int)p->count);
    }
}

// ---------------------------------------------------------------- edge marking
// The DS outlines a pixel with the edge colour of its polygon ID's group when one of its four
// neighbours has a different polygon ID and is farther away (the clear plane counts, with the
// clear polygon ID). First the IDs: the opaque polygons again (nearest wins, its own depth
// buffer), with the texture's alpha test.
static void draw_ids(const G3Frame *f, u32 disp3d, int translucent) {
    // (depth goes in 8 bits: spread over this frame's own range of opaque depths)
    float zmin = f->zn0 / f->zd0, zmax = f->zn1 / f->zd1;
    if (zmin < -1) zmin = -1;
    if (zmax > 1) zmax = 1;
    float zscale = zmax > zmin ? 0.996f / (zmax - zmin) : 0;
    // (depth = 0.004 + (zmax - z/w) * zscale = A + B * z/w; translucent: 0, never an edge)
    float A = translucent ? 0 : 0.004f + zmax * zscale, B = translucent ? 0 : -zscale;
    for (u32 i = 0; i < f->np; i++) {
        const G3Poly *p = &f->p[i];
        if (p->translucent != translucent) continue;
        int fmt = (p->teximage >> 26) & 7;
        C3D_Tex *tex = NULL;
        if (fmt && (disp3d & 1)) {
            int s0, s1, t0, t1;
            poly_st_bounds(f, p, &s0, &s1, &t0, &t1);
            tex = texture_for(p->teximage, p->pltt, s0, s1, t0, t1);
        }
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
        float id = (float)(((p->polyattr >> 24) & 63) * 4) / 255.0f;  // (translucent: only G = 0 is written)
        C3D_FVUnifSet(GPU_VERTEX_SHADER, sLocIdp, id, A, B, 0);
        if (tex) C3D_FVUnifSet(GPU_VERTEX_SHADER, sLocTcId, 1.0f / tex->width, 1.0f / tex->height, 0, 0);
        C3D_DrawArrays(GPU_TRIANGLES, (int)p->first, (int)p->count);
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
    C3D_TexBind(0, sIdCur);
    C3D_TexBind(1, sIdCur);
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
    // (corners in clip space, y up, a few pixels in; one corner a frame, the last result of each kept)
    static const float cx[4] = { -1, 1, -1, 1 }, cy[4] = { 1, 1, -1, -1 };
    static u8 hit[4];
    static int next;
    const float ix = 1 - 4 / 128.0f, iy = 1 - 4 / 96.0f;
    hit[next] = (u8)covers(f, cx[next] * ix, cy[next] * iy);
    next = (next + 1) & 3;
    return hit[0] && hit[1] && hit[2] && hit[3];
}

static int wide_stable(int want) {
    static int state, count;
    if (want == state) { count = 0; return state; }
    if (++count >= 4) { state = want; count = 0; }
    return state;
}

// ---------------------------------------------------------------- stereoscopic 3D
// With the 3D slider up, the top screen is drawn once per eye: the 2D layers at the screen plane,
// the 3D layer with a horizontal shift growing with depth, x += shift * (w - convergence). The
// convergence is the nearest on-screen depth of the frame (smoothed): the nearest geometry sits
// at the screen plane, the rest goes into the screen, as the 3DS's own games mostly do (and a
// flat, orthographic scene, all at one w, stays flat). The right eye has its own 3D layer, ID image
// and screen (citro3d clears every buffer at the frame's start, so nothing is shared): about
// 2.3 MB of VRAM, there in 1:1 mode only (the fill mode's 2x targets take it).
float port_stereo_force = -1;   // debug.txt "stereo X": the slider at X (0..1), for testing
// The switch for it all (the menu's "3D" row, settings.txt "3d on|off"; a build with
// -DPORT_NO_STEREO starts with it off): off, the slider does nothing and nothing is allocated.
#ifdef PORT_NO_STEREO
int port_stereo_enabled = 0;
#else
int port_stereo_enabled = 1;
#endif
static int sStereoFail;         // (no VRAM: not tried again until the display mode changes)
static float sShiftPx;          // the farthest depth's shift per eye, in screen pixels
static float sConv, sFar;
static int sFlat;        // (this frame's 3D is orthographic: no depth)
static int sRangeFresh;  // (the next frame takes the depth range at once: stereo just came on)

static void stereo_free(void) {
    if (sTopR) { C3D_RenderTargetDelete(sTopR); sTopR = NULL; }
    if (s3dTargetR) { C3D_RenderTargetDelete(s3dTargetR); s3dTargetR = NULL; C3D_TexDelete(&s3dTexR); }
    if (sIdTargetR) { C3D_RenderTargetDelete(sIdTargetR); sIdTargetR = NULL; C3D_TexDelete(&sIdTexR); }
    sStereoFail = 0;
    if (sStereo) { sStereo = 0; gfxSet3D(false); }
}

static bool stereo_alloc(void) {
    if (sTopR) return true;
    if (sStereoFail || !s3dReady) return false;
    sTopR = C3D_RenderTargetCreate(240, 400, GPU_RB_RGB8, GPU_RB_DEPTH24_STENCIL8);
    if (sTopR && C3D_TexInitVRAM(&s3dTexR, 512, 256, GPU_RGBA8)) {
        C3D_TexSetFilter(&s3dTexR, GPU_NEAREST, GPU_NEAREST);
        s3dTargetR = C3D_RenderTargetCreateFromTex(&s3dTexR, GPU_TEXFACE_2D, 0, GPU_RB_DEPTH24_STENCIL8);
        if (!s3dTargetR) C3D_TexDelete(&s3dTexR);
    }
    if (s3dTargetR && sIdTarget && C3D_TexInitVRAM(&sIdTexR, 512, 256, GPU_RGB8)) {
        C3D_TexSetFilter(&sIdTexR, GPU_NEAREST, GPU_NEAREST);
        C3D_TexSetWrap(&sIdTexR, GPU_CLAMP_TO_EDGE, GPU_CLAMP_TO_EDGE);
        sIdTargetR = C3D_RenderTargetCreateFromTex(&sIdTexR, GPU_TEXFACE_2D, 0, GPU_RB_DEPTH16);
        if (!sIdTargetR) C3D_TexDelete(&sIdTexR);
    }
    if (!sTopR || !s3dTargetR || (sIdTarget && !sIdTargetR)) {
        port_log("gpu: no VRAM for the right eye (free %lu)\n", (unsigned long)vramSpaceFree());
        stereo_free();
        sStereoFail = 1;
        return false;
    }
    C3D_RenderTargetSetOutput(sTopR, GFX_TOP, GFX_RIGHT, screen_transfer());
    port_log("gpu: right eye ready (VRAM free %lu)\n", (unsigned long)vramSpaceFree());
    return true;
}

// Whether this frame is drawn in stereo: the slider up, 1:1 mode, a 3D scene on the top screen
// (held for a few frames before changing: the parallax barrier doesn't flicker through scene changes).
static void stereo_decide(int scene3d) {
    float slider = port_stereo_force >= 0 ? port_stereo_force : osGet3DSliderState();
    if (!port_stereo_enabled) {  // (switched off: its VRAM back; C3D_FrameBegin has waited for the GPU)
        if (sTopR || sStereo) stereo_free();
        return;
    }
    int want = slider > 0.02f && sSS == 1 && scene3d;
    static int count;
    if (want == sStereo) count = 0;
    else if (++count >= 4 || !want) {
        count = 0;
        if (want && !stereo_alloc()) want = 0;
        if (want != sStereo) { sStereo = want; gfxSet3D(want); sRangeFresh = 1; }
    }
    sShiftPx = 3.5f * slider;  // (at the slider's top: 7 pixels between the eyes for the farthest)
}

// The frame's depth range: the nearest and farthest z/w among on-screen vertices, smoothed. The
// parallax is linear in z/w (as a real pair of cameras' is, in 1/w), scaled to the range: the
// nearest point at the screen plane, the farthest sShiftPx behind it, in a shallow scene (the
// field's high camera) as in a deep one, as the 3DS's games set their cameras per scene. (An
// orthographic scene, all at one w, stays flat: the battles', whose Pokémon stand on 2D platforms
// that a depth would detach them from.)
static void stereo_convergence(const G3Frame *f) {
    float m = 2, M = -2, wm = 1e30f, wM = 0;
    for (u32 i = 0; i < f->nv; i++) {
        const G3Vtx *v = &f->v[i];
        if (v->w > 0 && v->x <= v->w && -v->x <= v->w && v->y <= v->w && -v->y <= v->w && v->z <= v->w && -v->z <= v->w) {
            float z = v->z / v->w;
            if (z < m) m = z;
            if (z > M) M = z;
            if (v->w < wm) wm = v->w;
            if (v->w > wM) wM = v->w;
        }
    }
    if (m > M) return;
    sFlat = wM - wm <= wM * 0.001f;  // (orthographic)
    if (sRangeFresh) { sConv = m; sFar = M; sRangeFresh = 0; return; }
    sConv += (m - sConv) * 0.2f;
    sFar += (M - sFar) * 0.2f;
}

// One eye's 3D layer (eye: 0 mono, -1 left, 1 right) into its own target.
static void render3d_eye(const G3Frame *f, int eye, float vx, float vy, float vw, float vh, u32 disp3d, u32 clear) {
    C3D_RenderTarget *t = eye > 0 ? s3dTargetR : s3dTarget, *idt = eye > 0 ? sIdTargetR : sIdTarget;
    int ca = (clear >> 16) & 31;
    u32 cc = rgba((u16)(clear & 0x7FFF), (u8)(ca << 3 | ca >> 2));
    // (per unit of z/w, in clip x of this viewport: the farthest point, at sFar, gets sShiftPx)
    float range = sFar - sConv;
    if (range < 1e-4f) range = 1e-4f;
    float shift = eye && !sFlat ? eye * sShiftPx * 2.0f / vw / range : 0.0f;
    C3D_RenderTargetClear(t, C3D_CLEAR_ALL, cc, 0);
    C3D_FrameDrawOn(t);
    C3D_BindProgram(&sProg3d);
    attrs_3d(f);
    // (C3D_FrameDrawOn resets the viewport to the whole target: set again after each switch)
    const u32 vpx = (u32)(vx + 0.5f), vpy = (u32)(vy + 0.5f), vpw = (u32)(vw + 0.5f), vph = (u32)(vh + 0.5f);
    C3D_SetViewport(vpx, vpy, vpw, vph);
    C3D_FVUnifSet(GPU_VERTEX_SHADER, sLocXform, sClipX, -sClipY, 0, 0);  // (y flipped: see render3d)
    C3D_FVUnifSet(GPU_VERTEX_SHADER, sLocStereo, shift, sConv, range, 0);
    C3D_SetScissor(GPU_SCISSOR_DISABLE, 0, 0, 0, 0);
    C3D_StencilTest(false, GPU_ALWAYS, 0, 0, 0);
    C3D_CullFace(GPU_CULL_NONE);
    sIdCur = eye > 0 ? &sIdTexR : &sIdTex;
    draw_polys(f, 0, disp3d);
    if ((disp3d & 0x20) && idt && port_edge_mode) {
        // the IDs of what's visible, then the outlines over the opaque picture. (The DS marks
        // edges from the opaque pixels' IDs; translucent polygons blend over the result.)
        int ida = (clear >> 24) & 63;
        C3D_RenderTargetClear(idt, C3D_CLEAR_ALL, (u32)(ida * 4) << 16, 0);  // (the target's own format: RGB8, R in bits 16-23)
        C3D_FrameDrawOn(idt);
        C3D_SetViewport(vpx, vpy, vpw, vph);
        C3D_DepthTest(true, GPU_GREATER, GPU_WRITE_ALL);
        u64 tid = svcGetSystemTick();
        C3D_BindProgram(&sProgId);
        C3D_FVUnifSet(GPU_VERTEX_SHADER, sLocXformId, sClipX, -sClipY, 0, 0);
        C3D_FVUnifSet(GPU_VERTEX_SHADER, sLocStereoId, shift, sConv, range, 0);
        draw_ids(f, disp3d, 0);
        // a translucent pixel over an opaque one clears its edge flag (the opaque ID stays): G = 0
        C3D_DepthTest(true, GPU_GREATER, GPU_WRITE_GREEN);
        draw_ids(f, disp3d, 1);
        gProf3dIds += svcGetSystemTick() - tid;
        C3D_FrameDrawOn(t);
        draw_edges();
        C3D_BindProgram(&sProg3d);
        attrs_3d(f);
        C3D_FVUnifSet(GPU_VERTEX_SHADER, sLocXform, sClipX, -sClipY, 0, 0);
        C3D_FVUnifSet(GPU_VERTEX_SHADER, sLocStereo, shift, sConv, range, 0);
        C3D_SetViewport(vpx, vpy, vpw, vph);
    }
    draw_polys(f, 1, disp3d);
}

static void render3d(int screenW) {
    const G3Frame *f = port_g3_frame();
    layout(screenW);
    sWide = 0;
    if (!f || !s3dReady) return;
    { u64 t = svcGetSystemTick(); sWide = wide_stable(want_wide(f, screenW)); gProf3dWide += svcGetSystemTick() - t; }
    port_g3_wide = sWide ? sW / 256.0f : 1.0f;
    port_g3_wide_y = sWide ? sH / 192.0f : 1.0f;
    sFrameNo++;
    u32 disp3d = *(volatile u16 *)DS(0x04000060);
    u32 clear = *(volatile u32 *)DS(0x04000350);
    GSPGPU_FlushDataCache(f->v, f->nv * sizeof(G3Vtx));  // (written by the CPU; read by the GPU)
    // the DS viewport (x1, y1 from the bottom, x2, y2) on the 256x192 picture. (Measured with a test
    // square: the render target's viewport y counts from the picture's top, and its y axis points
    // down, so vertices go in with y negated.)
    // In screen pixels (the image covers the whole screen: DS pixel x is at (x - sL) * sScale).
    int x1 = f->viewport[0], y1 = f->viewport[1], x2 = f->viewport[2], y2 = f->viewport[3];
    float vx = (x1 - sL) * sScale, vy = (191 - y2 - sT) * sScale, vw = (x2 - x1 + 1) * sScale, vh = (y2 - y1 + 1) * sScale;
    sClipX = sClipY = 1.0f;
    if (sWide) { sClipX = vw / sScrW; sClipY = vh / 240; vx = vy = 0; vw = (float)sScrW; vh = 240; }
    if (sStereo) {
        stereo_convergence(f);
        render3d_eye(f, -1, vx, vy, vw, vh, disp3d, clear);
        render3d_eye(f, 1, vx, vy, vw, vh, disp3d, clear);
    } else {
        render3d_eye(f, 0, vx, vy, vw, vh, disp3d, clear);
    }
    sIdCur = &sIdTex;
    s3dHave = 1;
    // back to the 2D compositor's state
    C3D_BindProgram(&sProg);
    attrs_2d();
    C3D_TexEnvInit(C3D_GetTexEnv(1));
}
