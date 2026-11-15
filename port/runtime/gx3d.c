// The DS geometry engine: the game's 3D commands in, polygons out, in the DS's own 20.12 / 4.12
// fixed-point maths (matrices the game reads back, e.g. the clip matrix, come out exactly).
//
// Commands arrive two ways: writes to the command ports (0x04000440-0x040005FF, through
// port_g3_write, which every reg_G3_* assignment in the SDK and game was rewritten to) and the
// packed command FIFO (0x04000400: MI_SendGXCommand's display lists, GX_SendFifo*).
//
// Conventions follow GBATEK: row vectors (v' = v * M), MULT is M = N * M, the clip matrix is
// position * projection, primitives are triangles, quads and their strips.
#include <math.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include "gx3d.h"
#include "port_ds.h"
#include "port.h"

extern float port_g3_wide, port_g3_wide_y;
void *port_linear_alloc(size_t bytes);

typedef int32_t s32;
typedef int16_t s16;
typedef int64_t s64;
typedef uint32_t u32;
typedef uint16_t u16;
typedef uint8_t u8;

#define IO(off) ((volatile u32 *)(uintptr_t)(0x04000000 + PORT_DS_DELTA + (off)))

static void worker_start(void);
static void g3_sync(void);           // (the game waits for the geometry engine's core: see below)
static int vblank_impl(u32 swap, u32 vp);
static void *sFrameLock;             // (the frames' rotation against the renderer taking the shown one)
static volatile int sDrawing = -1;   // (the frame the renderer took last: its CPU, then the GPU, read it until it takes the next)
int port_core2_thread(void (*fn)(void *), void *arg, int below_render);
void *port_event_new(void);
void port_event_wait(void *e);
void port_event_signal(void *e);
void *port_lock_new(void);
void port_lock(void *l);
void port_unlock(void *l);

// ---------------------------------------------------------------- matrices (4x4, 20.12, row-major)
typedef s32 Mtx[16];

static void mtx_identity(Mtx m) {
    memset(m, 0, sizeof(Mtx));
    m[0] = m[5] = m[10] = m[15] = 1 << 12;
}

// d = a * b
static void mtx_mul(Mtx d, const Mtx a, const Mtx b) {
    Mtx r;
    for (int i = 0; i < 4; i++) {
        for (int j = 0; j < 4; j++) {
            s64 s = 0;
            for (int k = 0; k < 4; k++) s += (s64)a[i * 4 + k] * b[k * 4 + j];
            r[i * 4 + j] = (s32)(s >> 12);
        }
    }
    memcpy(d, r, sizeof(Mtx));
}

static Mtx sProj, sPos, sVec, sTex, sClip;
static float sClipF[16];  // the clip matrix pre-scaled for float vertices: x * [0..11] + [12..15]
static Mtx sProjStack[1], sPosStack[32], sVecStack[32], sTexStack[1];
static int sPosSp, sProjSp, sMode;
static int sClipDirty = 1;

// The clip matrix (position x projection), computed when something needs it: for vertices in
// float (from the two matrices directly, every matrix change before a vertex), in the DS's fixed
// point only when the game reads it back or tests against it (update_clip_fixed).
static int sClipFixedDirty = 1;
static void update_clip(void) {
    if (!sClipDirty) return;
    sClipDirty = 0;
    sClipFixedDirty = 1;
    float P[16], Q[16];
    for (int i = 0; i < 16; i++) { P[i] = (float)sPos[i]; Q[i] = (float)sProj[i]; }
    // (vertices are 4.12 integers: clip = (v * M) >> 24, and the translation row (M << 12) >> 24;
    // M itself is (P * Q) >> 12)
    for (int i = 0; i < 4; i++) {
        const float k = i < 3 ? 1.0f / (4096.0f * 16777216.0f) : 1.0f / (4096.0f * 4096.0f);
        for (int j = 0; j < 4; j++)
            sClipF[i * 4 + j] = (P[i * 4] * Q[j] + P[i * 4 + 1] * Q[4 + j] + P[i * 4 + 2] * Q[8 + j] + P[i * 4 + 3] * Q[12 + j]) * k;
    }
}
static void update_clip_fixed(void) {
    update_clip();
    if (!sClipFixedDirty) return;
    mtx_mul(sClip, sPos, sProj);
    sClipFixedDirty = 0;
}

// The result registers the game reads (the clip matrix at 0x640, the direction matrix at 0x680),
// written only when it reads them: G3X_GetClipMtx / G3X_GetVectorMtx are wrapped (port/build.py).
static void publish_matrices(void) {
    update_clip_fixed();
    volatile u32 *c = IO(0x640);
    for (int i = 0; i < 16; i++) c[i] = (u32)sClip[i];
    volatile u32 *v = IO(0x680);
    for (int r = 0; r < 3; r++)
        for (int k = 0; k < 3; k++) v[r * 3 + k] = (u32)sVec[r * 4 + k];
}

// The matrices the current mode's operation applies to.
static void apply(void (*op)(Mtx, const void *), const void *arg, int vecToo) {
    switch (sMode) {
    case 0: op(sProj, arg); break;
    case 1: op(sPos, arg); break;
    case 2: op(sPos, arg); if (vecToo) op(sVec, arg); break;
    case 3: op(sTex, arg); break;
    }
    sClipDirty = 1;
}

static void op_load(Mtx m, const void *arg) { memcpy(m, arg, sizeof(Mtx)); }
static void op_mult(Mtx m, const void *arg) { mtx_mul(m, (const s32 *)arg, m); }
static void op_identity(Mtx m, const void *arg) { (void)arg; mtx_identity(m); }
// (SCALE and TRANS as the full multiply would do them, touching only what changes: S * M scales
// rows 0-2; T * M adds tx row0 + ty row1 + tz row2 to row 3. The same sums, the same >> 12.)
static void op_scale(Mtx m, const void *arg) {
    const s32 *sc = arg;
    for (int r = 0; r < 3; r++)
        for (int c = 0; c < 4; c++) m[r * 4 + c] = (s32)(((s64)sc[r] * m[r * 4 + c]) >> 12);
}
static void op_trans(Mtx m, const void *arg) {
    const s32 *t = arg;
    for (int c = 0; c < 4; c++)
        m[12 + c] = (s32)(((s64)t[0] * m[c] + (s64)t[1] * m[4 + c] + (s64)t[2] * m[8 + c] + ((s64)m[12 + c] << 12)) >> 12);
}

// ---------------------------------------------------------------- vertex state
static u32 sColor = 0x7FFF;             // vertex colour, 15-bit
static s32 sVx, sVy, sVz;              // the last vertex (4.12), for VTX_XY / XZ / YZ / DIFF
static s32 sS, sT;                     // texture coordinates (12.4 texels), as given
static s32 sTs, sTt;                   // transformed
static int sTexSrc;                    // (1: sTs / sTt are sS / sT through the texture matrix, TEXCOORD in mode 1)
// TEXCOORD in mode 1: the coordinates through the texture matrix
static inline void tex_mode1(s32 s, s32 t, s32 *ts, s32 *tt) {
    *ts = (s32)(((s64)s * sTex[0] + (s64)t * sTex[4] + sTex[8] + sTex[12]) >> 12);
    *tt = (s32)(((s64)s * sTex[1] + (s64)t * sTex[5] + sTex[9] + sTex[13]) >> 12);
}
static u32 sPolyAttr, sPolyAttrNext, sTexImage, sPltt;
static int sPrim = -1;                 // primitive type of the current BEGIN_VTXS
static u32 sDifAmb, sSpeEmi, sLightColor[4];
static s32 sLightVec[4][3];            // transformed light directions (1.0.9 scaled to .12)
static u8 sShininess[128];
static int sUseShininess;

// ---------------------------------------------------------------- output
#define MAX_VTX 24576
#define MAX_POLY 8192
// Three frames, in rotation: the one the game is building, the one shown (being drawn), and the
// one shown before it, which the GPU may still be drawing from (it runs a frame behind the CPU:
// the render core and the GPU work while the game goes on), so it isn't written until the next
// swap. With two, the game wrote into vertices the GPU was still drawing: broken 3D on hardware.
static G3Frame sFrames[3];
static int sBuild = 0, sShown = 1, sHaveShown, sSwapPending;
static G3Vtx sPrimVtx[4];    // vertices of the primitive being assembled
static int sPrimN, sPrimCount;

// Display-list recording (see "display-list cache" below): while a list runs for the first time,
// its vertices in model space and its calls to emit_triangles are written down.
typedef struct { u32 attr, tex, pltt; u16 idx[4]; u8 n; } DlEmit;
typedef struct { G3Vtx v; s16 s, t; u32 texsrc; } DlVtx;  // (texsrc: s, t through the texture matrix at replay)
#define REC_MAXV 4096
#define REC_MAXE 4096
#define NO_IDX 0xFFFFu
static int sRecording, sRecBad, sRecImpure;
static int sRecTsSet, sRecTsRead, sRecTexRead, sRecVecRead;  // (what a list reads of the state it starts from)
static u32 sRecNV, sRecNE;
static DlVtx *sRecV;            // (x, y, z: the vertex as given, before any matrix)
static DlEmit *sRecE;
static u16 sPrimIdx[4];         // (sPrimVtx's vertices' indices in sRecV)

static void frames_init(void) {
    for (int i = 0; i < 3; i++) {
        sFrames[i].v = port_linear_alloc(sizeof(G3Vtx) * MAX_VTX);  // (drawn from where it is: GPU-visible)
        sFrames[i].p = malloc(sizeof(G3Poly) * MAX_POLY);
    }
}

static int alpha_of(u32 attr) { return (attr >> 16) & 31; }

int port_g3_prof;  // (debug.txt "g3prof N", timing by ablation: 1 no polygon assembly, 2 no vertex work, 3 decode only)
static void emit_triangles(const G3Vtx *const *v, int n) {  // (pointers: the strips' vertices aren't copied to reorder them)
    if (sRecording) {  // (written down whatever happens next: culling depends on the matrices)
        if (sRecNE >= REC_MAXE) sRecBad = 1;
        else {
            DlEmit *e = &sRecE[sRecNE++];
            e->attr = sPolyAttr; e->tex = sTexImage; e->pltt = sPltt; e->n = (u8)n;
            for (int i = 0; i < n; i++) if ((e->idx[i] = sPrimIdx[v[i] - sPrimVtx]) == NO_IDX) sRecBad = 1;
        }
    }
    if (port_g3_prof == 1) return;
    // n = 3 (triangle) or 4 (quad, as two triangles)
    G3Frame *f = &sFrames[sBuild];
    if (!f->v) return;
    int alpha = alpha_of(sPolyAttr);
    if (alpha == 0) return;  // (wireframe: not drawn yet)
    if (((sPolyAttr >> 4) & 3) == 3) return;  // (shadow polygons: not yet)
    // culling: DS front faces are those drawn clockwise on screen, where y points down; here y
    // points up (clip space), so clockwise on screen is a positive area
    int allw = v[0]->w > 0 && v[1]->w > 0 && v[2]->w > 0 && (n == 3 || v[3]->w > 0);
    if (allw) {
        // (the projected triangle's area times w0 w1 w2 > 0: its sign without dividing; a quad is
        // planar and convex, its first three corners have its winding)
        const G3Vtx *a = v[0], *b = v[1], *c = v[2];
        float area = (a->x * b->y - b->x * a->y) * c->w + (b->x * c->y - c->x * b->y) * a->w + (c->x * a->y - a->x * c->y) * b->w;
        int front = area > 0;
        if (front && !(sPolyAttr & 0x80)) return;
        if (!front && !(sPolyAttr & 0x40)) return;
    }
    int ntri = n == 4 ? 2 : 1;
    if (f->nv + ntri * 3 > MAX_VTX || f->np >= MAX_POLY) return;
    // a new polygon record unless this continues the last one with the same state
    G3Poly *p = f->np ? &f->p[f->np - 1] : NULL;
    int texfmt = (sTexImage >> 26) & 7;
    int translucent = alpha < 31 || texfmt == 1 || texfmt == 6;
    if (!p || p->polyattr != sPolyAttr || p->teximage != sTexImage || p->pltt != sPltt ||
        p->first + p->count != f->nv) {
        p = &f->p[f->np++];
        p->polyattr = sPolyAttr; p->teximage = sTexImage; p->pltt = sPltt;
        p->first = f->nv; p->count = 0; p->translucent = (u8)translucent;
        p->s0 = p->t0 = 1e9f; p->s1 = p->t1 = -1e9f;
    }
    // (what the renderer needs per polygon and frame, while the values are at hand: texture
    // bounds, and the opaque z/w range kept as fractions, compared without dividing)
    for (int i = 0; i < n; i++) {
        const G3Vtx *a = v[i];
        if (a->s < p->s0) p->s0 = a->s;
        if (a->s > p->s1) p->s1 = a->s;
        if (a->t < p->t0) p->t0 = a->t;
        if (a->t > p->t1) p->t1 = a->t;
        if (!translucent && a->w > 0) {
            if (a->z * f->zd0 < f->zn0 * a->w) { f->zn0 = a->z; f->zd0 = a->w; }
            if (a->z * f->zd1 > f->zn1 * a->w) { f->zn1 = a->z; f->zd1 = a->w; }
        }
    }
    static const int quadIdx[6] = { 0, 1, 2, 0, 2, 3 };
    for (int i = 0; i < ntri * 3; i++) f->v[f->nv++] = *v[n == 4 ? quadIdx[i] : i];
    p->count += ntri * 3;
}

// Vertices to whole DS pixels, as the DS's rasterizer has them (its screen coordinates are
// integers): the map's separately built pieces meet at corners a fraction of a pixel apart, and
// the 3DS GPU, placing them exactly, left hairline cracks between them (the clear colour through,
// outlined black by edge marking). Rounded to the same pixel, the corners coincide as on the DS.
// (In clip space, x / w and y / w to the viewport's pixel grid and back; far outside the screen,
// left alone. debug.txt "snap 0" turns it off, to compare.)
int port_g3_snap = 0;  // (off: it didn't close the holes (see hole_fill in gpu2d.c) and cost a fifth of the geometry engine's time)
static void snap_to_pixels(G3Vtx *v) {
    const u8 *vp = sFrames[sBuild].viewport;
    float hw = (vp[2] - vp[0] + 1) * 0.5f, hh = (vp[3] - vp[1] + 1) * 0.5f;
    float nx = v->x / v->w, ny = v->y / v->w;
    if (nx < -8 || nx > 8 || ny < -8 || ny > 8 || hw <= 0 || hh <= 0) return;
    v->x = (floorf((nx + 1) * hw + 0.5f) / hw - 1) * v->w;
    v->y = (floorf((ny + 1) * hh + 0.5f) / hh - 1) * v->w;
}

// A vertex through the clip matrix (one function for the geometry engine and the display-list
// cache's replay, so both give the same bits).
static inline void transform(G3Vtx *v, float fx, float fy, float fz) {
    const float *M = sClipF;
    v->x = fx * M[0] + fy * M[4] + fz * M[8] + M[12];
    v->y = fx * M[1] + fy * M[5] + fz * M[9] + M[13];
    v->z = fx * M[2] + fy * M[6] + fz * M[10] + M[14];
    v->w = fx * M[3] + fy * M[7] + fz * M[11] + M[15];
    if (port_g3_snap && v->w > 0) snap_to_pixels(v);
}

static void add_vertex(s32 x, s32 y, s32 z) {
    sVx = x; sVy = y; sVz = z;
    if (sPrim < 0 || port_g3_prof == 2) return;
    if (sClipDirty) update_clip();
    G3Vtx v;
    static u32 sLastColor = ~0u, sLastAttr = ~0u;
    static u8 sR, sG, sB, sA;  // (the colour and alpha as bytes, converted when they change)
    // (in float: what the GPU draws needn't match the DS's fixed point to the last bit, and the
    // 64-bit products and their conversions were most of the geometry engine's time)
    const float fx = (float)x, fy = (float)y, fz = (float)z;
    transform(&v, fx, fy, fz);
    if (sColor != sLastColor) {
        sLastColor = sColor;
        int r = sColor & 31, g = (sColor >> 5) & 31, b = (sColor >> 10) & 31;
        sR = (u8)(r << 3 | r >> 2); sG = (u8)(g << 3 | g >> 2); sB = (u8)(b << 3 | b >> 2);
    }
    if (sPolyAttr != sLastAttr) { sLastAttr = sPolyAttr; int a = alpha_of(sPolyAttr); sA = (u8)(a << 3 | a >> 2); }
    v.r = sR; v.g = sG; v.b = sB; v.a = sA;
    s32 ts = sTs, tt = sTt;
    if ((sTexImage >> 30) != 3) { if (!sRecTsSet) sRecTsRead = 1; }
    else {  // texture coordinates from the vertex
        sRecTexRead = 1;
        ts = sS + (s32)(((s64)x * sTex[0] + (s64)y * sTex[4] + (s64)z * sTex[8]) >> 24);
        tt = sT + (s32)(((s64)x * sTex[1] + (s64)y * sTex[5] + (s64)z * sTex[9]) >> 24);
    }
    v.s = ts * (1.0f / 16.0f); v.t = tt * (1.0f / 16.0f);

    u16 idx = NO_IDX;
    if (sRecording) {
        if (sRecNV >= REC_MAXV) sRecBad = 1;
        else {
            DlVtx *m = &sRecV[sRecNV];
            m->v = v; m->v.x = fx; m->v.y = fy; m->v.z = fz; m->v.w = 1;
            m->texsrc = (sTexImage >> 30) != 3 && sTexSrc && sRecTsSet; m->s = (s16)sS; m->t = (s16)sT;
            idx = (u16)sRecNV++;
        }
    }
    sPrimIdx[sPrimN] = idx;
    sPrimVtx[sPrimN++] = v;
    sPrimCount++;
    const G3Vtx *P = sPrimVtx;
    switch (sPrim) {
    case 0:  // triangles
        if (sPrimN == 3) { const G3Vtx *t[3] = { P, P + 1, P + 2 }; emit_triangles(t, 3); sPrimN = 0; }
        break;
    case 1:  // quads
        if (sPrimN == 4) { const G3Vtx *q[4] = { P, P + 1, P + 2, P + 3 }; emit_triangles(q, 4); sPrimN = 0; }
        break;
    case 2:  // triangle strip: each new vertex with the two before it, winding alternating
        if (sPrimN == 3) {
            const int odd = sPrimCount & 1;
            const G3Vtx *t[3] = { odd ? P : P + 1, odd ? P + 1 : P, P + 2 };
            emit_triangles(t, 3);
            sPrimVtx[0] = sPrimVtx[1]; sPrimVtx[1] = sPrimVtx[2]; sPrimN = 2;
            sPrimIdx[0] = sPrimIdx[1]; sPrimIdx[1] = sPrimIdx[2];
        }
        break;
    case 3:  // quad strip: each new pair with the pair before it
        if (sPrimN == 4) {
            const G3Vtx *q[4] = { P, P + 1, P + 3, P + 2 };
            emit_triangles(q, 4);
            sPrimVtx[0] = sPrimVtx[2]; sPrimVtx[1] = sPrimVtx[3]; sPrimN = 2;
            sPrimIdx[0] = sPrimIdx[2]; sPrimIdx[1] = sPrimIdx[3];
        }
        break;
    }
}

// ---------------------------------------------------------------- lighting (at NORMAL)
static void normal(u32 p) {
    s32 nx = (s32)(p << 22) >> 22, ny = (s32)(p << 12) >> 22, nz = (s32)(p << 2) >> 22;  // 1.0.9
    if ((sTexImage >> 30) == 2) {  // texture coordinates from the normal
        sTexSrc = 0; sRecTsSet = 1; sRecTexRead = 1;
        sTs = sS + (s32)(((s64)nx * sTex[0] + (s64)ny * sTex[4] + (s64)nz * sTex[8]) >> 21);
        sTt = sT + (s32)(((s64)nx * sTex[1] + (s64)ny * sTex[5] + (s64)nz * sTex[9]) >> 21);
    }
    sRecVecRead = 1;
    // the normal through the direction matrix (to .12)
    float n[3];
    for (int k = 0; k < 3; k++) n[k] = ((float)nx * sVec[0 * 4 + k] + (float)ny * sVec[1 * 4 + k] + (float)nz * sVec[2 * 4 + k]) * (1.0f / (512.0f * 4096.0f));
    float dif[3] = { (float)(sDifAmb & 31), (float)((sDifAmb >> 5) & 31), (float)((sDifAmb >> 10) & 31) };
    float amb[3] = { (float)((sDifAmb >> 16) & 31), (float)((sDifAmb >> 21) & 31), (float)((sDifAmb >> 26) & 31) };
    float spe[3] = { (float)(sSpeEmi & 31), (float)((sSpeEmi >> 5) & 31), (float)((sSpeEmi >> 10) & 31) };
    float emi[3] = { (float)((sSpeEmi >> 16) & 31), (float)((sSpeEmi >> 21) & 31), (float)((sSpeEmi >> 26) & 31) };
    float out[3] = { emi[0], emi[1], emi[2] };
    for (int i = 0; i < 4; i++) {
        if (!(sPolyAttr & (1u << i))) continue;
        float L[3] = { sLightVec[i][0] * (1.0f / 4096.0f), sLightVec[i][1] * (1.0f / 4096.0f), sLightVec[i][2] * (1.0f / 4096.0f) };
        float d = -(L[0] * n[0] + L[1] * n[1] + L[2] * n[2]);
        if (d < 0) d = 0;
        float H[3] = { (L[0] + 0) / 2, (L[1] + 0) / 2, (L[2] - 1) / 2 };
        float sp = -(H[0] * n[0] + H[1] * n[1] + H[2] * n[2]);
        if (sp < 0) sp = 0;
        sp = sp * sp;
        if (sp > 1) sp = 1;
        if (sUseShininess) sp = sShininess[(int)(sp * 127)] / 256.0f;
        float lc[3] = { (float)(sLightColor[i] & 31), (float)((sLightColor[i] >> 5) & 31), (float)((sLightColor[i] >> 10) & 31) };
        for (int c = 0; c < 3; c++) out[c] += (dif[c] * d + spe[c] * sp + amb[c]) * lc[c] * (1.0f / 31.0f);
    }
    int r = out[0] > 31 ? 31 : (int)out[0], g = out[1] > 31 ? 31 : (int)out[1], b = out[2] > 31 ? 31 : (int)out[2];
    sColor = (u32)(r | g << 5 | b << 10);
}

// ---------------------------------------------------------------- tests
static void box_test(const u32 *p) {
    s32 x = (int16_t)p[0], y = (int16_t)(p[0] >> 16), z = (int16_t)p[1];
    s32 w = (int16_t)(p[1] >> 16), h = (int16_t)p[2], d = (int16_t)(p[2] >> 16);
    update_clip_fixed();
    // visible unless all 8 corners are outside the same clip plane
    int outside[6] = { 1, 1, 1, 1, 1, 1 };
    for (int c = 0; c < 8; c++) {
        s64 vx = x + ((c & 1) ? w : 0), vy = y + ((c & 2) ? h : 0), vz = z + ((c & 4) ? d : 0);
        s64 cl[4];
        for (int k = 0; k < 4; k++) cl[k] = vx * sClip[k] + vy * sClip[4 + k] + vz * sClip[8 + k] + ((s64)sClip[12 + k] << 12);
        // (x against the widened view: what shows at the sides of the top screen is drawn too)
        s64 wx = port_g3_wide > 1.0f ? (s64)(cl[3] * port_g3_wide) : cl[3];
        s64 wy = port_g3_wide_y > 1.0f ? (s64)(cl[3] * port_g3_wide_y) : cl[3];
        outside[0] &= cl[0] < -wx; outside[1] &= cl[0] > wx;
        outside[2] &= cl[1] < -wy; outside[3] &= cl[1] > wy;
        outside[4] &= cl[2] < -cl[3]; outside[5] &= cl[2] > cl[3];
    }
    int vis = !(outside[0] | outside[1] | outside[2] | outside[3] | outside[4] | outside[5]);
    volatile u32 *stat = IO(0x600);
    *stat = (*stat & ~2u) | (vis ? 2u : 0);
}

static void pos_test(const u32 *p) {
    s32 x = (int16_t)p[0], y = (int16_t)(p[0] >> 16), z = (int16_t)p[1];
    sVx = x; sVy = y; sVz = z;
    update_clip_fixed();
    volatile u32 *r = IO(0x620);
    for (int k = 0; k < 4; k++)
        r[k] = (u32)(s32)(((s64)x * sClip[k] + (s64)y * sClip[4 + k] + (s64)z * sClip[8 + k] + ((s64)sClip[12 + k] << 12)) >> 12);
}

static void vec_test(u32 p) {
    s32 nx = (s32)(p << 22) >> 22, ny = (s32)(p << 12) >> 22, nz = (s32)(p << 2) >> 22;
    volatile u16 *r = (volatile u16 *)IO(0x630);
    for (int k = 0; k < 3; k++) r[k] = (u16)(s32)(((s64)nx * sVec[k] + (s64)ny * sVec[4 + k] + (s64)nz * sVec[8 + k]) >> 9);
}

// ---------------------------------------------------------------- commands
static const u8 kParams[256] = {
    [0x10] = 1, [0x11] = 0, [0x12] = 1, [0x13] = 1, [0x14] = 1, [0x15] = 0, [0x16] = 16, [0x17] = 12,
    [0x18] = 16, [0x19] = 12, [0x1A] = 9, [0x1B] = 3, [0x1C] = 3,
    [0x20] = 1, [0x21] = 1, [0x22] = 1, [0x23] = 2, [0x24] = 1, [0x25] = 1, [0x26] = 1, [0x27] = 1,
    [0x28] = 1, [0x29] = 1, [0x2A] = 1, [0x2B] = 1,
    [0x30] = 1, [0x31] = 1, [0x32] = 1, [0x33] = 1, [0x34] = 32,
    [0x40] = 1, [0x41] = 0, [0x50] = 1, [0x60] = 1, [0x70] = 3, [0x71] = 2, [0x72] = 1,
};

// The commands a cached display list may hold: they read the matrices but don't change them, and
// leave no result for the game to read (no tests, no swap, no viewport). The rest are 1.
static const u8 kPure[256] = {
    [0x00] = 1, [0x10] = 1, [0x20] = 1, [0x21] = 1, [0x22] = 1, [0x23] = 1, [0x24] = 1, [0x25] = 1,
    [0x26] = 1, [0x27] = 1, [0x28] = 1, [0x29] = 1, [0x2A] = 1, [0x2B] = 1,
    [0x30] = 1, [0x31] = 1, [0x32] = 1, [0x33] = 1, [0x34] = 1, [0x40] = 1, [0x41] = 1,
};

static void run_cmd(u8 cmd, const u32 *p);

static u32 sHist[128], sTraceLeft;
static void run(u8 cmd, const u32 *p) {
    sHist[cmd & 127]++;
    if (sTraceLeft) {
        sTraceLeft--;
        int n = kParams[cmd];
        port_log("  g3 %02X:", cmd);
        for (int i = 0; i < n && i < 16; i++) port_log(" %08lX", (unsigned long)p[i]);
        port_log("\n");
    }
    if (port_g3_prof == 3) return;
    if (sRecording && !kPure[cmd]) sRecImpure = 1;
    run_cmd(cmd, p);
}
void port_g3_hist_report(void) {  // (the command mix since the last report, per frame)
    u32 tot = 0;
    for (int i = 0; i < 128; i++) tot += sHist[i];
    port_log("  g3 commands per frame: %lu;", (unsigned long)(tot / 600));
    for (int i = 0; i < 128; i++) if (sHist[i] / 600 >= 20) port_log(" %02X:%lu", i, (unsigned long)(sHist[i] / 600));
    port_log("\n");
    memset(sHist, 0, sizeof(sHist));
}

// (defined before its uses: an undeclared g3_sync_r became an implicit call, which the build's
// automatic stubs answered with an empty function, so no read waited for the geometry engine)
uint32_t gG3Syncs[8];  // (per reason: clip, vector, box, pos, vec, levels, trace)
#define g3_sync_r(r) do { gG3Syncs[r]++; g3_sync(); } while (0)
int __real_G3X_GetClipMtx(void *m);
int __wrap_G3X_GetClipMtx(void *m) { g3_sync_r(0); publish_matrices(); return __real_G3X_GetClipMtx(m); }
int __real_G3X_GetBoxTestResult(void *in);
int __wrap_G3X_GetBoxTestResult(void *in) { g3_sync_r(2); return __real_G3X_GetBoxTestResult(in); }
int __real_G3X_GetPositionTestResult(void *v, void *w);
int __wrap_G3X_GetPositionTestResult(void *v, void *w) { g3_sync_r(3); return __real_G3X_GetPositionTestResult(v, w); }
int __real_G3X_GetVectorTestResult(void *v);
int __wrap_G3X_GetVectorTestResult(void *v) { g3_sync_r(4); return __real_G3X_GetVectorTestResult(v); }
int __real_G3X_GetMtxStackLevelPV(void *l);
int __wrap_G3X_GetMtxStackLevelPV(void *l) { g3_sync_r(5); return __real_G3X_GetMtxStackLevelPV(l); }
int __real_G3X_GetMtxStackLevelPJ(void *l);
int __wrap_G3X_GetMtxStackLevelPJ(void *l) { g3_sync_r(5); return __real_G3X_GetMtxStackLevelPJ(l); }
int __real_G3X_GetVectorMtx(void *m);
int __wrap_G3X_GetVectorMtx(void *m) { g3_sync_r(1); publish_matrices(); return __real_G3X_GetVectorMtx(m); }

static void run_cmd(u8 cmd, const u32 *p) {
    Mtx m;
    switch (cmd) {
    case 0x10: sMode = p[0] & 3; break;
    case 0x11:  // push
        if (sMode == 0) { memcpy(sProjStack[0], sProj, sizeof(Mtx)); sProjSp = 1; }
        else if (sMode == 3) memcpy(sTexStack[0], sTex, sizeof(Mtx));
        else if (sPosSp < 31) { memcpy(sPosStack[sPosSp], sPos, sizeof(Mtx)); memcpy(sVecStack[sPosSp], sVec, sizeof(Mtx)); sPosSp++; }
        break;
    case 0x12: {  // pop
        if (sMode == 0) { memcpy(sProj, sProjStack[0], sizeof(Mtx)); sProjSp = 0; }
        else if (sMode == 3) memcpy(sTex, sTexStack[0], sizeof(Mtx));
        else {
            int n = (s32)(p[0] << 26) >> 26;
            sPosSp -= n;
            if (sPosSp < 0) sPosSp = 0;
            if (sPosSp > 31) sPosSp = 31;
            memcpy(sPos, sPosStack[sPosSp], sizeof(Mtx));
            memcpy(sVec, sVecStack[sPosSp], sizeof(Mtx));
        }
        sClipDirty = 1;
        break;
    }
    case 0x13: {  // store
        int n = p[0] & 31;
        if (sMode == 0) memcpy(sProjStack[0], sProj, sizeof(Mtx));
        else if (sMode == 3) memcpy(sTexStack[0], sTex, sizeof(Mtx));
        else if (n < 31) { memcpy(sPosStack[n], sPos, sizeof(Mtx)); memcpy(sVecStack[n], sVec, sizeof(Mtx)); }
        break;
    }
    case 0x14: {  // restore
        int n = p[0] & 31;
        if (sMode == 0) memcpy(sProj, sProjStack[0], sizeof(Mtx));
        else if (sMode == 3) memcpy(sTex, sTexStack[0], sizeof(Mtx));
        else if (n < 31) { memcpy(sPos, sPosStack[n], sizeof(Mtx)); memcpy(sVec, sVecStack[n], sizeof(Mtx)); }
        sClipDirty = 1;
        break;
    }
    case 0x15: apply(op_identity, NULL, 1); break;
    case 0x16: for (int i = 0; i < 16; i++) m[i] = (s32)p[i]; apply(op_load, m, 1); break;
    case 0x17:
        for (int r = 0; r < 4; r++) { for (int c = 0; c < 3; c++) m[r * 4 + c] = (s32)p[r * 3 + c]; m[r * 4 + 3] = r == 3 ? 1 << 12 : 0; }
        apply(op_load, m, 1);
        break;
    case 0x18: for (int i = 0; i < 16; i++) m[i] = (s32)p[i]; apply(op_mult, m, 1); break;
    case 0x19:
        for (int r = 0; r < 4; r++) { for (int c = 0; c < 3; c++) m[r * 4 + c] = (s32)p[r * 3 + c]; m[r * 4 + 3] = r == 3 ? 1 << 12 : 0; }
        apply(op_mult, m, 1);
        break;
    case 0x1A:
        mtx_identity(m);
        for (int r = 0; r < 3; r++) for (int c = 0; c < 3; c++) m[r * 4 + c] = (s32)p[r * 3 + c];
        apply(op_mult, m, 1);
        break;
    case 0x1B:  // scale (in mode 2, only the position matrix)
        m[0] = (s32)p[0]; m[1] = (s32)p[1]; m[2] = (s32)p[2];
        apply(op_scale, m, 0);
        break;
    case 0x1C:
        m[0] = (s32)p[0]; m[1] = (s32)p[1]; m[2] = (s32)p[2];
        apply(op_trans, m, 1);
        break;
    case 0x20: sColor = p[0] & 0x7FFF; break;
    case 0x21: normal(p[0]); break;
    case 0x22:
        sS = (int16_t)p[0]; sT = (int16_t)(p[0] >> 16);
        sTs = sS; sTt = sT;
        sTexSrc = (sTexImage >> 30) == 1;
        if (sTexSrc) tex_mode1(sS, sT, &sTs, &sTt);
        sRecTsSet = 1;
        break;
    case 0x23: add_vertex((int16_t)p[0], (int16_t)(p[0] >> 16), (int16_t)p[1]); break;
    case 0x24: add_vertex((s32)(p[0] << 22) >> 16, (s32)(p[0] << 12) >> 16, (s32)(p[0] << 2) >> 16); break;
    case 0x25: add_vertex((int16_t)p[0], (int16_t)(p[0] >> 16), sVz); break;
    case 0x26: add_vertex((int16_t)p[0], sVy, (int16_t)(p[0] >> 16)); break;
    case 0x27: add_vertex(sVx, (int16_t)p[0], (int16_t)(p[0] >> 16)); break;
    case 0x28:
        // (1.0.9 differences scaled by 1/8: the raw 10-bit value is already in 4.12 units)
        add_vertex(sVx + ((s32)(p[0] << 22) >> 22), sVy + ((s32)(p[0] << 12) >> 22), sVz + ((s32)(p[0] << 2) >> 22));
        break;
    case 0x29: sPolyAttrNext = p[0]; break;
    case 0x2A: sTexImage = p[0]; sTs = sS; sTt = sT; sTexSrc = 0; sRecTsSet = 1; break;
    case 0x2B: sPltt = p[0] & 0x1FFF; break;
    case 0x30:
        sDifAmb = p[0];
        if (p[0] & 0x8000) sColor = p[0] & 0x7FFF;
        break;
    case 0x31: sSpeEmi = p[0]; sUseShininess = (p[0] >> 15) & 1; break;
    case 0x32: {  // light vector, through the direction matrix
        int i = p[0] >> 30;
        sRecVecRead = 1;
        s32 lx = (s32)(p[0] << 22) >> 22, ly = (s32)(p[0] << 12) >> 22, lz = (s32)(p[0] << 2) >> 22;
        for (int k = 0; k < 3; k++)
            sLightVec[i][k] = (s32)(((s64)lx * sVec[k] + (s64)ly * sVec[4 + k] + (s64)lz * sVec[8 + k]) >> 9);
        break;
    }
    case 0x33: sLightColor[p[0] >> 30] = p[0] & 0x7FFF; break;
    case 0x34: for (int i = 0; i < 32; i++) for (int k = 0; k < 4; k++) sShininess[i * 4 + k] = (u8)(p[i] >> (k * 8)); break;
    case 0x40: sPrim = p[0] & 3; sPrimN = 0; sPrimCount = 0; sPolyAttr = sPolyAttrNext; break;
    case 0x41: sPrim = -1; break;
    case 0x50: sFrames[sBuild].swap = p[0]; sSwapPending = 1; break;
    case 0x60: memcpy(sFrames[sBuild].viewport, p, 4); break;
    case 0x70: box_test(p); break;
    case 0x71: pos_test(p); break;
    case 0x72: vec_test(p[0]); break;
    default: break;
    }
}

// ---------------------------------------------------------------- the two ways in
static u8 sPortCmd;         // command port being filled
static u32 sPortParams[32];
static int sPortN;

static u8 sFifoCmds[4];     // packed FIFO: the commands of the current command word
static int sFifoNCmd, sFifoCur;
static u32 sFifoParams[32];
static int sFifoN;
static u32 sFifoWords;

static void init_once(void) {
    static int done;
    if (done) return;
    done = 1;
    mtx_identity(sProj); mtx_identity(sPos); mtx_identity(sVec); mtx_identity(sTex);
    frames_init();
    for (int i = 0; i < 3; i++) {
        sFrames[i].zn0 = sFrames[i].zd0 = sFrames[i].zd1 = 1; sFrames[i].zn1 = -1;
        sFrames[i].viewport[2] = 255; sFrames[i].viewport[3] = 191;
    }
    // (written straight by the game's assembly: a value no game writes marks "nothing new")
    *IO(0x540) = 0xFFFFFFFFu;
    *IO(0x580) = 0xFFFFFFFFu;
    worker_start();
}

// Moves to the next command of the packed word that needs parameters (running those that don't).
static void fifo_advance(void) {
    while (sFifoCur < sFifoNCmd) {
        u8 c = sFifoCmds[sFifoCur];
        if (kParams[c] == 0) {
            if (c) run(c, NULL);
            sFifoCur++;
            continue;
        }
        return;
    }
    sFifoNCmd = 0;
}

static void fifo_write_impl(u32 word) {
    sFifoWords++;
    if (sFifoNCmd == 0) {
        // a command word: up to four commands, lowest byte first
        for (int i = 0; i < 4; i++) sFifoCmds[i] = (u8)(word >> (i * 8));
        sFifoNCmd = 4;
        while (sFifoNCmd > 0 && sFifoCmds[sFifoNCmd - 1] == 0) sFifoNCmd--;  // (trailing no-ops)
        sFifoCur = 0;
        sFifoN = 0;
        fifo_advance();
        return;
    }
    u8 c = sFifoCmds[sFifoCur];
    sFifoParams[sFifoN++] = word;
    if (sFifoN >= kParams[c]) {
        run(c, sFifoParams);
        sFifoN = 0;
        sFifoCur++;
        fifo_advance();
    }
}

static void fifo_decode(const u32 *src, u32 words) {
    const u32 *s = src, *end = s + words;
    // (a packed command word still waiting for parameters: finished word by word first)
    while (s < end && sFifoNCmd) fifo_write_impl(*s++);
    // The rest decoded in place: each command word, then each of its commands run with its
    // parameters where they lie (no call and no copy per word: decoding was 3 of the field's
    // 8.4 ms in the geometry engine). A command whose parameters run past the end is left to
    // port_gx_fifo_write, in the same state it would have reached word by word.
    while (s < end) {
        u32 word = *s++;
        sFifoWords++;
        u8 cmds[4] = { (u8)word, (u8)(word >> 8), (u8)(word >> 16), (u8)(word >> 24) };
        int n = 4;
        while (n > 0 && cmds[n - 1] == 0) n--;  // (trailing no-ops)
        for (int i = 0; i < n; i++) {
            u8 c = cmds[i];
            int np = kParams[c];
            if (np == 0) {
                if (c) run(c, NULL);
                continue;
            }
            if (end - s < np) {  // (its parameters continue in a later transfer)
                memcpy(sFifoCmds, cmds, 4);
                sFifoNCmd = n; sFifoCur = i; sFifoN = 0;
                while (s < end) fifo_write_impl(*s++);
                return;
            }
            run(c, s);
            s += np;
            sFifoWords += np;
        }
    }
}

// ---------------------------------------------------------------- display-list cache
// The game draws the same models every frame: the map's blocks, buildings, trees, the same NPCs.
// A display list (one send of words to the FIFO) that holds only vertex and material commands
// (kPure), sent again with the same words and the same starting state, gives the same vertices in
// model space, the same colours (lighting included), the same texture coordinates and the same
// calls to emit_triangles. So the first run is written down, and later ones only put the recorded
// vertices through the current clip matrix and emit them, skipping the decoding, the lighting and
// the strips' assembly. The clip matrix isn't part of the key: the camera moves every frame while
// walking, and instanced models (trees) share one entry. The direction and texture matrices are
// (lighting and texture coordinates depend on them); they change only when a model turns.
// The replay gives the same bits as running the list (the same transform, the same emits).
// debug.txt "dlcache 0" turns it off.
int port_dl_cache = 1;

typedef struct {  // the state a list's commands read or change (besides the clip matrix)
    u32 color; s32 vx, vy, vz, s, t;
    u32 polyattr, polyattrNext, teximage, pltt;
    s32 prim, primN, primCount, mode;
    u32 difamb, speemi, lightColor[4];
    s32 lightVec[4][3];
    s32 useShininess, texsrc;
    u8 shininess[128];
    // (compared only when the list reads them: the rest from here on; read only, the matrices)
    s32 ts, tt;
    Mtx vec, tex;
} VState;
#define DL_READS_TS  1
#define DL_READS_VEC 2
#define DL_READS_TEX 4
#define DL_SETS_TS   8  // (otherwise the coordinates it ends with are those it started with)

static void vstate_save(VState *v) {
    memset(v, 0, offsetof(VState, ts));  // (compared with memcmp)
    v->color = sColor; v->vx = sVx; v->vy = sVy; v->vz = sVz; v->s = sS; v->t = sT; v->ts = sTs; v->tt = sTt;
    v->polyattr = sPolyAttr; v->polyattrNext = sPolyAttrNext; v->teximage = sTexImage; v->pltt = sPltt;
    v->prim = sPrim; v->primN = sPrimN; v->primCount = sPrimCount; v->mode = sMode;
    v->difamb = sDifAmb; v->speemi = sSpeEmi;
    memcpy(v->lightColor, sLightColor, sizeof(sLightColor)); memcpy(v->lightVec, sLightVec, sizeof(sLightVec));
    v->useShininess = sUseShininess; v->texsrc = sTexSrc; memcpy(v->shininess, sShininess, sizeof(sShininess));
    memcpy(v->vec, sVec, sizeof(Mtx)); memcpy(v->tex, sTex, sizeof(Mtx));
}
static void vstate_load(const VState *v) {
    sColor = v->color; sVx = v->vx; sVy = v->vy; sVz = v->vz; sS = v->s; sT = v->t; sTs = v->ts; sTt = v->tt;
    sPolyAttr = v->polyattr; sPolyAttrNext = v->polyattrNext; sTexImage = v->teximage; sPltt = v->pltt;
    sPrim = v->prim; sPrimN = v->primN; sPrimCount = v->primCount; sMode = v->mode;
    sDifAmb = v->difamb; sSpeEmi = v->speemi;
    memcpy(sLightColor, v->lightColor, sizeof(sLightColor)); memcpy(sLightVec, v->lightVec, sizeof(sLightVec));
    sUseShininess = v->useShininess; sTexSrc = v->texsrc; memcpy(sShininess, v->shininess, sizeof(sShininess));
}

typedef struct DlEntry {
    struct DlEntry *next;
    u32 hash, words, nv, ne, bytes, reads;
    VState start, end;
    u16 endIdx[4];       // (the primitive left pending at the end: its vertices)
    DlVtx *v;            // nv vertices in model space
    DlEmit *e;           // ne emits
    u32 *data;           // the list's words
} DlEntry;

#define DL_BUCKETS 1024
#define DL_BUDGET (3u << 20)   // (bytes; past it, everything is dropped and recorded afresh)
#define DL_VARIANTS 8          // (entries per list: one per starting state, e.g. lighting)
#define DL_MIN_WORDS 16        // (smaller sends: the matrices and materials the 3D library buffers)
static DlEntry *sDl[DL_BUCKETS];
static u32 sDlBytes;
static u32 sImpure[256];       // (hashes of lists seen to hold other commands: not recorded again)
static VState sCur;
static G3Vtx *sReplay;
uint32_t gDlStats[6];          // hits, recorded, impure, not cacheable, words replayed, words sent

static void dl_flush(void) {
    for (int b = 0; b < DL_BUCKETS; b++) {
        for (DlEntry *e = sDl[b], *n; e; e = n) { n = e->next; free(e); }
        sDl[b] = NULL;
    }
    sDlBytes = 0;
}

static u32 dl_hash(const u32 *w, u32 n) {
    u32 a = n * 0x9E3779B1u, b = 0x85EBCA6Bu;
    u32 i = 0;
    for (; i + 1 < n; i += 2) { a = (a ^ w[i]) * 0x01000193u; b = (b ^ w[i + 1]) * 0x01000193u; }
    if (i < n) a = (a ^ w[i]) * 0x01000193u;
    return a ^ (b * 0x2C1B3C6Du) ^ (b >> 15);
}

static void dl_replay(const DlEntry *e) {
    if (sClipDirty) update_clip();
    for (u32 i = 0; i < e->nv; i++) {
        G3Vtx *o = &sReplay[i];
        const DlVtx *r = &e->v[i];
        *o = r->v;
        transform(o, r->v.x, r->v.y, r->v.z);
        if (r->texsrc) { s32 ts, tt; tex_mode1(r->s, r->t, &ts, &tt); o->s = ts * (1.0f / 16.0f); o->t = tt * (1.0f / 16.0f); }
    }
    for (u32 i = 0; i < e->ne; i++) {
        const DlEmit *m = &e->e[i];
        sPolyAttr = m->attr; sTexImage = m->tex; sPltt = m->pltt;
        const G3Vtx *pv[4];
        for (int k = 0; k < m->n; k++) pv[k] = &sReplay[m->idx[k]];
        emit_triangles(pv, m->n);
    }
    s32 ts0 = sTs, tt0 = sTt;
    vstate_load(&e->end);
    if (!(e->reads & DL_SETS_TS)) { sTs = ts0; sTt = tt0; }
    else if (sTexSrc) tex_mode1(sS, sT, &sTs, &sTt);  // (the current texture matrix's, not the recorded one)
    for (int k = 0; k < sPrimN; k++) { sPrimVtx[k] = sReplay[e->endIdx[k]]; sPrimIdx[k] = NO_IDX; }
    sFifoWords += e->words;
}

// Runs a list from the cache or records it; 0: not for the cache (decode it as usual).
static int dl_run(const u32 *src, u32 words) {
    if (!port_dl_cache || port_g3_prof || sTraceLeft || sFifoNCmd || words < DL_MIN_WORDS || words > 0xFFFF) return 0;
    if (!sRecV) {
        sRecV = malloc(sizeof(DlVtx) * REC_MAXV);
        sRecE = malloc(sizeof(DlEmit) * REC_MAXE);
        sReplay = malloc(sizeof(G3Vtx) * REC_MAXV);
        if (!sRecV || !sRecE || !sReplay) { port_dl_cache = 0; return 0; }
    }
    gDlStats[5] += words;
    u32 h = dl_hash(src, words);
    if (sImpure[h & 255] == h) { gDlStats[2]++; return 0; }
    vstate_save(&sCur);
    DlEntry **bucket = &sDl[h & (DL_BUCKETS - 1)];
    int variants = 0;
    for (DlEntry *e = *bucket; e; e = e->next) {
        if (e->hash != h || e->words != words) continue;
        variants++;
        if (memcmp(&e->start, &sCur, offsetof(VState, ts))) continue;
        if ((e->reads & DL_READS_TS) && (e->start.ts != sCur.ts || e->start.tt != sCur.tt)) continue;
        if ((e->reads & DL_READS_VEC) && memcmp(e->start.vec, sCur.vec, sizeof(Mtx))) continue;
        if ((e->reads & DL_READS_TEX) && memcmp(e->start.tex, sCur.tex, sizeof(Mtx))) continue;
        if (memcmp(e->data, src, words * 4)) continue;
        dl_replay(e);
        gDlStats[0]++;
        gDlStats[4] += words;
        return 1;
    }
    // first time with this state: run it, writing it down
    sRecording = 1; sRecBad = sRecImpure = 0; sRecNV = sRecNE = 0;
    sRecTsSet = sRecTsRead = sRecTexRead = sRecVecRead = 0;
    for (int k = 0; k < 4; k++) sPrimIdx[k] = NO_IDX;  // (a primitive begun before the list: not in the record)
    fifo_decode(src, words);
    sRecording = 0;
    if (sRecImpure) { sImpure[h & 255] = h; gDlStats[2]++; return 1; }
    if (sRecBad || sFifoNCmd || variants >= DL_VARIANTS) { gDlStats[3]++; return 1; }
    u16 endIdx[4] = { NO_IDX, NO_IDX, NO_IDX, NO_IDX };
    for (int k = 0; k < sPrimN; k++) if ((endIdx[k] = sPrimIdx[k]) == NO_IDX) { gDlStats[3]++; return 1; }
    u32 bytes = sizeof(DlEntry) + sRecNV * sizeof(DlVtx) + sRecNE * sizeof(DlEmit) + words * 4;
    if (sDlBytes + bytes > DL_BUDGET) dl_flush();
    DlEntry *e = malloc(bytes);
    if (!e) { dl_flush(); gDlStats[3]++; return 1; }
    e->hash = h; e->words = words; e->nv = sRecNV; e->ne = sRecNE; e->bytes = bytes;
    e->reads = (sRecTsRead ? DL_READS_TS : 0) | (sRecVecRead ? DL_READS_VEC : 0) | (sRecTexRead ? DL_READS_TEX : 0) | (sRecTsSet ? DL_SETS_TS : 0);
    e->start = sCur;
    vstate_save(&e->end);
    memcpy(e->endIdx, endIdx, sizeof(endIdx));
    e->v = (DlVtx *)(e + 1);
    e->e = (DlEmit *)(e->v + sRecNV);
    e->data = (u32 *)(e->e + sRecNE);
    memcpy(e->v, sRecV, sRecNV * sizeof(DlVtx));
    memcpy(e->e, sRecE, sRecNE * sizeof(DlEmit));
    memcpy(e->data, src, words * 4);
    e->next = *bucket;
    *bucket = e;
    sDlBytes += bytes;
    gDlStats[1]++;
    return 1;
}

uint64_t gG3Ticks;  // (profiling: CPU time in the geometry engine, display lists and register writes)
uint64_t port_ctr_ticks(void);
static void fifo_send_impl(const u32 *src, u32 words) {
    uint64_t t = port_ctr_ticks();
    if (!dl_run(src, words)) fifo_decode(src, words);
    gG3Ticks += port_ctr_ticks() - t;
}

u32 port_gx_fifo_words(void) { return sFifoWords; }

// A write to a command port (0x440-0x5FF).
static void port_cmd_impl(u32 off, u32 value) {
    u8 cmd = (u8)((off - 0x400) >> 2);
    if (cmd != sPortCmd) { sPortCmd = cmd; sPortN = 0; }
    if (kParams[cmd] == 0) { run(cmd, NULL); return; }
    sPortParams[sPortN++] = value;
    if (sPortN >= kParams[cmd]) { run(cmd, sPortParams); sPortN = 0; }
}

// ---------------------------------------------------------------- the geometry engine's own core
// On the New 3DS the commands are queued and run on core 2 (beside the render thread) while the
// game goes on: the geometry engine was most of an update's CPU time in the field, and the game
// only needs its results at a few points, which wait for it to catch up (g3_sync): the box,
// position and vector tests, the matrix stack levels, the clip and direction matrices. The
// VBlank's frame swap goes through the queue in order, so the geometry of a frame can finish
// while the game builds the next (one frame later on screen at most). Elsewhere (the original
// 3DS: no spare core) everything runs on the game's thread as before. debug.txt "g3thread 0"
// turns it off; "g3thread 2" runs it below the render thread's priority (default: above it).
//
// The queue: records of words in a ring, never wrapping inside a record (a pad record skips the
// ring's tail). A FIFO record's words are decoded where they lie.
#define RING_WORDS (1u << 16)
#define REC_FIFO   0x01000000u  // | n: n FIFO words follow
#define REC_PORT   0x02000000u  // | offset: a command port write, its value follows
#define REC_VBLANK 0x03000000u  // swap register, viewport register follow
#define REC_PAD    0x0F000000u  // the rest of the ring unused
// Where the engine runs: 0 on the game's thread, 1 core 2 above the render thread, 2 below it.
// debug.txt "g3thread N" sets it; otherwise the menu's "3D engine" row (settings.txt "geometry"),
// read at start, so a change there applies from the next start.
int port_g3_thread = -1;
static int sWorker;                    // (the worker is running)
static u32 *sRing;
static volatile u32 sHead, sTail;      // (words written by the game / consumed by the worker, ever)
static volatile int sWorkerSleeping, sGameWaiting;
static void *sWorkEvent, *sProgressEvent;
uint64_t gG3QueueWait;                 // (the game waiting for the worker: a full ring or a sync)

static u32 sMainFpscr;  // (the game thread's floating-point mode, for the worker's to match)
static void worker_main(void *arg) {
    (void)arg;
    __asm__ volatile("vmsr fpscr, %0" : : "r"(sMainFpscr));
    { u32 f; __asm__ volatile("vmrs %0, fpscr" : "=r"(f)); port_log("geometry engine: fpscr %08lX (game thread's)\n", (unsigned long)f); }
    for (;;) {
        while (sTail == sHead) {
            sWorkerSleeping = 1;
            __atomic_thread_fence(__ATOMIC_SEQ_CST);
            if (sTail != sHead) { sWorkerSleeping = 0; break; }
            port_event_wait(sWorkEvent);
            sWorkerSleeping = 0;
        }
        __atomic_thread_fence(__ATOMIC_ACQUIRE);
        u32 at = sTail & (RING_WORDS - 1);
        u32 h = sRing[at], n;
        switch (h & 0xFF000000u) {
        case REC_FIFO: n = h & 0xFFFFFF; fifo_send_impl(&sRing[at + 1], n); n++; break;
        case REC_PORT: port_cmd_impl(h & 0xFFFF, sRing[at + 1]); n = 2; break;
        case REC_VBLANK: vblank_impl(sRing[at + 1], sRing[at + 2]); n = 3; break;
        default: n = RING_WORDS - at; break;  // (pad)
        }
        __atomic_thread_fence(__ATOMIC_RELEASE);
        sTail += n;
        __atomic_thread_fence(__ATOMIC_SEQ_CST);
        if (sGameWaiting) { sGameWaiting = 0; port_event_signal(sProgressEvent); }
    }
}

// The game's thread waits until the worker has done all but `left` words of the ring.
static void wait_worker(u32 maxPending) {
    if (sHead - sTail <= maxPending) return;
    uint64_t t0 = port_ctr_ticks();
    while (sHead - sTail > maxPending) {
        sGameWaiting = 1;
        __atomic_thread_fence(__ATOMIC_SEQ_CST);
        if (sHead - sTail <= maxPending) { sGameWaiting = 0; break; }
        port_event_wait(sProgressEvent);
    }
    gG3QueueWait += port_ctr_ticks() - t0;
}
static void g3_sync(void) { if (sWorker) wait_worker(0); }
int port_g3_running(void) { return sWorker ? port_g3_thread : 0; }  // (for the menu)

// One record: a header and fixed words, then (FIFO) `n` words from `data`.
static void enqueue(const u32 *rec, u32 nrec, const u32 *data, u32 n) {
    u32 total = nrec + n;
    u32 at = sHead & (RING_WORDS - 1);
    if (at + total > RING_WORDS) {  // (not enough room before the ring's end: pad it)
        wait_worker(at);  // (room for the pad: the words from here to the end)
        sRing[at] = REC_PAD;
        __atomic_thread_fence(__ATOMIC_RELEASE);
        sHead += RING_WORDS - at;
        at = 0;
    }
    wait_worker(RING_WORDS - total);
    memcpy(&sRing[at], rec, nrec * 4);
    if (n) memcpy(&sRing[at + nrec], data, n * 4);
    __atomic_thread_fence(__ATOMIC_RELEASE);
    sHead += total;
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    if (sWorkerSleeping) { sWorkerSleeping = 0; port_event_signal(sWorkEvent); }
}

static void worker_start(void) {
    extern int port_g3_setting;
    if (port_g3_thread < 0) port_g3_thread = port_g3_setting;
    if (!port_g3_thread) { port_log("geometry engine: on the game's thread\n"); return; }
    sRing = malloc(RING_WORDS * 4);
    sWorkEvent = port_event_new();
    sProgressEvent = port_event_new();
    sFrameLock = port_lock_new();
    if (!sRing || !sWorkEvent || !sProgressEvent || !sFrameLock) { port_log("geometry engine: on the game's thread (no memory)\n"); return; }
    __asm__ volatile("vmrs %0, fpscr" : "=r"(sMainFpscr));
    sWorker = port_core2_thread(worker_main, NULL, port_g3_thread == 2);
    port_log(sWorker ? "geometry engine: on core 2 (%s the render thread)\n" : "geometry engine: on the game's thread\n", port_g3_thread == 2 ? "below" : "above");
}

void port_gx_fifo_write(u32 word) {
    init_once();
    if (sWorker) { u32 rec[1] = { REC_FIFO | 1 }; enqueue(rec, 1, &word, 1); return; }
    fifo_write_impl(word);
}

uint32_t gG3Sends;  // (calls, per log period)
void port_gx_fifo_send(const void *src, void *dest, u32 bytes) {
    (void)dest;
    init_once();
    gG3Sends++;
    const u32 *s = src;
    u32 n = bytes / 4;
    if (!sWorker) { fifo_send_impl(s, n); return; }
    while (n) {  // (in pieces of at most a sixteenth of the ring)
        u32 k = n > RING_WORDS / 16 ? RING_WORDS / 16 : n;
        u32 rec[1] = { REC_FIFO | k };
        enqueue(rec, 1, s, k);
        s += k; n -= k;
    }
}

// Every write to a DS 3D register (the rewritten reg_G3_* / reg_G3X_* assignments).
void port_g3_write(unsigned long addr, unsigned long value) {
    init_once();
    u32 off = (u32)addr - (0x04000000 + PORT_DS_DELTA);
    if (off >= 0x400 && off < 0x440) { port_gx_fifo_write((u32)value); return; }
    if (off >= 0x440 && off < 0x600) {
        if (sWorker) { u32 rec[2] = { REC_PORT | off, (u32)value }; enqueue(rec, 2, NULL, 0); return; }
        port_cmd_impl(off, (u32)value);
        return;
    }
    *IO(off) = (u32)value;  // (rendering registers: just stored, the renderer reads them)
}

// ---------------------------------------------------------------- frames
// The frame to draw (the renderer, once per picture, after the GPU finished the last).
const G3Frame *port_g3_frame(void) {
    if (sFrameLock) port_lock(sFrameLock);
    const G3Frame *f = NULL;
    if (sHaveShown) { sDrawing = sShown; f = &sFrames[sShown]; }
    if (sFrameLock) port_unlock(sFrameLock);
    return f;
}

int port_g3_vblank(void) {
    init_once();
    extern uint32_t port_trace3d;
    if (port_trace3d && port_frame_count == port_trace3d) {
        g3_sync();
        port_log("3D trace, frame %lu: command counts:", (unsigned long)port_frame_count);
        for (int i = 0; i < 128; i++) if (sHist[i]) port_log(" %02X:%lu", i, (unsigned long)sHist[i]);
        port_log("\nprojection:");
        for (int i = 0; i < 16; i++) port_log(" %ld", (long)sProj[i]);
        port_log("\n");
        // the frame being shown: triangles with a vertex far off screen, behind or near the camera
        const G3Frame *f = &sFrames[sShown];
        int shown = 0;
        port_log("frame: %lu polygons, %lu vertices\n", (unsigned long)f->np, (unsigned long)f->nv);
        for (u32 pi = 0; pi < f->np; pi++) {
            const G3Poly *p = &f->p[pi];
            float x0 = 9, y0 = 9, x1 = -9, y1 = -9, z0 = 9, z1 = -9;
            for (u32 k = 0; k < p->count; k++) {
                const G3Vtx *v = &f->v[p->first + k];
                if (v->w <= 0) continue;
                float x = v->x / v->w, y = v->y / v->w, z = v->z / v->w;
                if (x < x0) x0 = x; if (x > x1) x1 = x; if (y < y0) y0 = y; if (y > y1) y1 = y; if (z < z0) z0 = z; if (z > z1) z1 = z;
            }
            port_log("  P%lu attr %08lX tex %08lX pltt %04lX n %lu x %.2f..%.2f y %.2f..%.2f z %.3f..%.3f a %u\n", (unsigned long)pi, (unsigned long)p->polyattr,
                     (unsigned long)p->teximage, (unsigned long)p->pltt, (unsigned long)p->count, x0, x1, y0, y1, z0, z1, f->v[p->first].a);
        }
        for (u32 pi = 0; pi < f->np && shown < 30; pi++) {
            const G3Poly *p = &f->p[pi];
            for (u32 k = 0; k < p->count; k += 3) {
                int odd = 0;
                for (int j = 0; j < 3; j++) {
                    const G3Vtx *v = &f->v[p->first + k + j];
                    if (v->w <= 0.05f || fabsf(v->x) > 3 * v->w || fabsf(v->y) > 3 * v->w) odd = 1;
                }
                if (!odd) continue;
                port_log("  poly %lu attr %08lX tex %08lX tri %lu:", (unsigned long)pi, (unsigned long)p->polyattr, (unsigned long)p->teximage, (unsigned long)k / 3);
                for (int j = 0; j < 3; j++) {
                    const G3Vtx *v = &f->v[p->first + k + j];
                    port_log(" (%.2f %.2f %.2f %.2f)", v->x, v->y, v->z, v->w);
                }
                port_log("\n");
                if (++shown >= 30) break;
            }
        }
        sTraceLeft = 400;  // and the next frame's first commands
    }
    // (the game's assembly also writes SWAP_BUFFERS and VIEWPORT straight into their registers:
    // seen here, on the game's thread, and passed on in order with the commands)
    volatile u32 *swapReg = IO(0x540), *vpReg = IO(0x580);
    u32 swap = *swapReg, vp = *vpReg;
    *swapReg = 0xFFFFFFFFu;
    *vpReg = 0xFFFFFFFFu;
    if (sWorker) { u32 rec[3] = { REC_VBLANK, swap, vp }; enqueue(rec, 3, NULL, 0); return 0; }
    return vblank_impl(swap, vp);
}

// The VBlank: a SWAP_BUFFERS since the last one shows the built frame.
static int vblank_impl(u32 swap, u32 vp) {
    if (swap != 0xFFFFFFFFu) { sFrames[sBuild].swap = swap; sSwapPending = 1; }
    if (vp != 0xFFFFFFFFu) memcpy(sFrames[sBuild].viewport, &vp, 4);
    if (!sSwapPending) return 0;
    sSwapPending = 0;
    // (the built frame is shown; building goes on in one neither shown nor being drawn: with the
    // geometry engine on its own core, it can swap twice while one frame is drawn)
    if (sFrameLock) port_lock(sFrameLock);
    int next = 0;
    while (next == sBuild || next == sDrawing) next++;
    sShown = sBuild;
    sBuild = next;
    sHaveShown = 1;
    if (sFrameLock) port_unlock(sFrameLock);
    G3Frame *f = &sFrames[sBuild];
    f->nv = 0;
    f->np = 0;
    f->zn0 = 1; f->zd0 = 1; f->zn1 = -1; f->zd1 = 1;
    memcpy(f->viewport, sFrames[sShown].viewport, 4);
    return 1;
}
