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
#include <stdlib.h>
#include <string.h>
#include "gx3d.h"
#include "port_ds.h"
#include "port.h"

typedef int32_t s32;
typedef int64_t s64;
typedef uint32_t u32;
typedef uint16_t u16;
typedef uint8_t u8;

#define IO(off) ((volatile u32 *)(uintptr_t)(0x04000000 + PORT_DS_DELTA + (off)))

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
static Mtx sProjStack[1], sPosStack[32], sVecStack[32], sTexStack[1];
static int sPosSp, sProjSp, sMode;
static int sClipDirty = 1;

// The result registers the game reads: the clip matrix (0x640) and the direction matrix (0x680).
static void publish_matrices(void) {
    if (!sClipDirty) return;
    mtx_mul(sClip, sPos, sProj);
    sClipDirty = 0;
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

// ---------------------------------------------------------------- vertex state
static u32 sColor = 0x7FFF;             // vertex colour, 15-bit
static s32 sVx, sVy, sVz;              // the last vertex (4.12), for VTX_XY / XZ / YZ / DIFF
static s32 sS, sT;                     // texture coordinates (12.4 texels), as given
static s32 sTs, sTt;                   // transformed
static u32 sPolyAttr, sPolyAttrNext, sTexImage, sPltt;
static int sPrim = -1;                 // primitive type of the current BEGIN_VTXS
static u32 sDifAmb, sSpeEmi, sLightColor[4];
static s32 sLightVec[4][3];            // transformed light directions (1.0.9 scaled to .12)
static u8 sShininess[128];
static int sUseShininess;

// ---------------------------------------------------------------- output
#define MAX_VTX 24576
#define MAX_POLY 8192
static G3Frame sFrames[2];   // [building, shown]
static int sBuild = 0, sHaveShown, sSwapPending;
static G3Vtx sPrimVtx[4];    // vertices of the primitive being assembled
static int sPrimN, sPrimCount;

static void frames_init(void) {
    for (int i = 0; i < 2; i++) {
        sFrames[i].v = malloc(sizeof(G3Vtx) * MAX_VTX);
        sFrames[i].p = malloc(sizeof(G3Poly) * MAX_POLY);
    }
}

static int alpha_of(u32 attr) { return (attr >> 16) & 31; }

static void emit_triangles(const G3Vtx *v, int n) {
    // n = 3 (triangle) or 4 (quad, as two triangles)
    G3Frame *f = &sFrames[sBuild];
    if (!f->v) return;
    int alpha = alpha_of(sPolyAttr);
    if (alpha == 0) return;  // (wireframe: not drawn yet)
    if (((sPolyAttr >> 4) & 3) == 3) return;  // (shadow polygons: not yet)
    // culling: DS front faces are those drawn clockwise on screen, where y points down; here y
    // points up (clip space), so clockwise on screen is a positive area
    float area = 0;
    int allw = 1;
    for (int i = 0; i < n; i++) if (v[i].w <= 0) allw = 0;
    if (allw) {
        for (int i = 0; i < n; i++) {
            const G3Vtx *a = &v[i], *b = &v[(i + 1) % n];
            area += (a->x / a->w) * (b->y / b->w) - (b->x / b->w) * (a->y / a->w);
        }
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
    }
    static const int quadIdx[6] = { 0, 1, 2, 0, 2, 3 };
    for (int i = 0; i < ntri * 3; i++) f->v[f->nv++] = v[n == 4 ? quadIdx[i] : i];
    p->count += ntri * 3;
}

static void add_vertex(s32 x, s32 y, s32 z) {
    sVx = x; sVy = y; sVz = z;
    if (sPrim < 0) return;
    if (sClipDirty) publish_matrices();
    G3Vtx v;
    s64 cx = (s64)x * sClip[0] + (s64)y * sClip[4] + (s64)z * sClip[8] + ((s64)sClip[12] << 12);
    s64 cy = (s64)x * sClip[1] + (s64)y * sClip[5] + (s64)z * sClip[9] + ((s64)sClip[13] << 12);
    s64 cz = (s64)x * sClip[2] + (s64)y * sClip[6] + (s64)z * sClip[10] + ((s64)sClip[14] << 12);
    s64 cw = (s64)x * sClip[3] + (s64)y * sClip[7] + (s64)z * sClip[11] + ((s64)sClip[15] << 12);
    v.x = (float)cx / (1 << 24); v.y = (float)cy / (1 << 24); v.z = (float)cz / (1 << 24); v.w = (float)cw / (1 << 24);
    int r = sColor & 31, g = (sColor >> 5) & 31, b = (sColor >> 10) & 31;
    v.r = (u8)(r << 3 | r >> 2); v.g = (u8)(g << 3 | g >> 2); v.b = (u8)(b << 3 | b >> 2);
    int a = alpha_of(sPolyAttr);
    v.a = (u8)(a << 3 | a >> 2);
    s32 ts = sTs, tt = sTt;
    if ((sTexImage >> 30) == 3) {  // texture coordinates from the vertex
        ts = sS + (s32)(((s64)x * sTex[0] + (s64)y * sTex[4] + (s64)z * sTex[8]) >> 24);
        tt = sT + (s32)(((s64)x * sTex[1] + (s64)y * sTex[5] + (s64)z * sTex[9]) >> 24);
    }
    v.s = ts / 16.0f; v.t = tt / 16.0f;

    sPrimVtx[sPrimN++] = v;
    sPrimCount++;
    switch (sPrim) {
    case 0:  // triangles
        if (sPrimN == 3) { emit_triangles(sPrimVtx, 3); sPrimN = 0; }
        break;
    case 1:  // quads
        if (sPrimN == 4) { emit_triangles(sPrimVtx, 4); sPrimN = 0; }
        break;
    case 2:  // triangle strip: each new vertex with the two before it, winding alternating
        if (sPrimN == 3) {
            G3Vtx t[3];
            if (sPrimCount & 1) { t[0] = sPrimVtx[0]; t[1] = sPrimVtx[1]; t[2] = sPrimVtx[2]; }
            else { t[0] = sPrimVtx[1]; t[1] = sPrimVtx[0]; t[2] = sPrimVtx[2]; }
            emit_triangles(t, 3);
            sPrimVtx[0] = sPrimVtx[1]; sPrimVtx[1] = sPrimVtx[2]; sPrimN = 2;
        }
        break;
    case 3:  // quad strip: each new pair with the pair before it
        if (sPrimN == 4) {
            G3Vtx q[4] = { sPrimVtx[0], sPrimVtx[1], sPrimVtx[3], sPrimVtx[2] };
            emit_triangles(q, 4);
            sPrimVtx[0] = sPrimVtx[2]; sPrimVtx[1] = sPrimVtx[3]; sPrimN = 2;
        }
        break;
    }
}

// ---------------------------------------------------------------- lighting (at NORMAL)
static void normal(u32 p) {
    s32 nx = (s32)(p << 22) >> 22, ny = (s32)(p << 12) >> 22, nz = (s32)(p << 2) >> 22;  // 1.0.9
    if ((sTexImage >> 30) == 2) {  // texture coordinates from the normal
        sTs = sS + (s32)(((s64)nx * sTex[0] + (s64)ny * sTex[4] + (s64)nz * sTex[8]) >> 21);
        sTt = sT + (s32)(((s64)nx * sTex[1] + (s64)ny * sTex[5] + (s64)nz * sTex[9]) >> 21);
    }
    if (sClipDirty) publish_matrices();
    // the normal through the direction matrix (to .12)
    float n[3];
    for (int k = 0; k < 3; k++) n[k] = ((float)nx * sVec[0 * 4 + k] + (float)ny * sVec[1 * 4 + k] + (float)nz * sVec[2 * 4 + k]) / (512.0f * 4096.0f);
    float dif[3] = { (float)(sDifAmb & 31), (float)((sDifAmb >> 5) & 31), (float)((sDifAmb >> 10) & 31) };
    float amb[3] = { (float)((sDifAmb >> 16) & 31), (float)((sDifAmb >> 21) & 31), (float)((sDifAmb >> 26) & 31) };
    float spe[3] = { (float)(sSpeEmi & 31), (float)((sSpeEmi >> 5) & 31), (float)((sSpeEmi >> 10) & 31) };
    float emi[3] = { (float)((sSpeEmi >> 16) & 31), (float)((sSpeEmi >> 21) & 31), (float)((sSpeEmi >> 26) & 31) };
    float out[3] = { emi[0], emi[1], emi[2] };
    for (int i = 0; i < 4; i++) {
        if (!(sPolyAttr & (1u << i))) continue;
        float L[3] = { sLightVec[i][0] / 4096.0f, sLightVec[i][1] / 4096.0f, sLightVec[i][2] / 4096.0f };
        float d = -(L[0] * n[0] + L[1] * n[1] + L[2] * n[2]);
        if (d < 0) d = 0;
        float H[3] = { (L[0] + 0) / 2, (L[1] + 0) / 2, (L[2] - 1) / 2 };
        float sp = -(H[0] * n[0] + H[1] * n[1] + H[2] * n[2]);
        if (sp < 0) sp = 0;
        sp = sp * sp;
        if (sp > 1) sp = 1;
        if (sUseShininess) sp = sShininess[(int)(sp * 127)] / 256.0f;
        float lc[3] = { (float)(sLightColor[i] & 31), (float)((sLightColor[i] >> 5) & 31), (float)((sLightColor[i] >> 10) & 31) };
        for (int c = 0; c < 3; c++) out[c] += (dif[c] * d + spe[c] * sp + amb[c]) * lc[c] / 31.0f;
    }
    int r = out[0] > 31 ? 31 : (int)out[0], g = out[1] > 31 ? 31 : (int)out[1], b = out[2] > 31 ? 31 : (int)out[2];
    sColor = (u32)(r | g << 5 | b << 10);
}

// ---------------------------------------------------------------- tests
static void box_test(const u32 *p) {
    s32 x = (int16_t)p[0], y = (int16_t)(p[0] >> 16), z = (int16_t)p[1];
    s32 w = (int16_t)(p[1] >> 16), h = (int16_t)p[2], d = (int16_t)(p[2] >> 16);
    if (sClipDirty) publish_matrices();
    // visible unless all 8 corners are outside the same clip plane
    int outside[6] = { 1, 1, 1, 1, 1, 1 };
    for (int c = 0; c < 8; c++) {
        s64 vx = x + ((c & 1) ? w : 0), vy = y + ((c & 2) ? h : 0), vz = z + ((c & 4) ? d : 0);
        s64 cl[4];
        for (int k = 0; k < 4; k++) cl[k] = vx * sClip[k] + vy * sClip[4 + k] + vz * sClip[8 + k] + ((s64)sClip[12 + k] << 12);
        outside[0] &= cl[0] < -cl[3]; outside[1] &= cl[0] > cl[3];
        outside[2] &= cl[1] < -cl[3]; outside[3] &= cl[1] > cl[3];
        outside[4] &= cl[2] < -cl[3]; outside[5] &= cl[2] > cl[3];
    }
    int vis = !(outside[0] | outside[1] | outside[2] | outside[3] | outside[4] | outside[5]);
    volatile u32 *stat = IO(0x600);
    *stat = (*stat & ~2u) | (vis ? 2u : 0);
}

static void pos_test(const u32 *p) {
    s32 x = (int16_t)p[0], y = (int16_t)(p[0] >> 16), z = (int16_t)p[1];
    sVx = x; sVy = y; sVz = z;
    if (sClipDirty) publish_matrices();
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

static void run_cmd(u8 cmd, const u32 *p);

// (the game reads the clip and direction matrices back right after matrix commands, so they're
// kept current after every command that can change them)
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
    run_cmd(cmd, p);
    if (cmd >= 0x10 && cmd <= 0x1C && sClipDirty) publish_matrices();
}

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
        mtx_identity(m);
        m[0] = (s32)p[0]; m[5] = (s32)p[1]; m[10] = (s32)p[2];
        apply(op_mult, m, 0);
        break;
    case 0x1C:
        mtx_identity(m);
        m[12] = (s32)p[0]; m[13] = (s32)p[1]; m[14] = (s32)p[2];
        apply(op_mult, m, 1);
        break;
    case 0x20: sColor = p[0] & 0x7FFF; break;
    case 0x21: normal(p[0]); break;
    case 0x22:
        sS = (int16_t)p[0]; sT = (int16_t)(p[0] >> 16);
        sTs = sS; sTt = sT;
        if ((sTexImage >> 30) == 1) {
            sTs = (s32)(((s64)sS * sTex[0] + (s64)sT * sTex[4] + sTex[8] + sTex[12]) >> 12);
            sTt = (s32)(((s64)sS * sTex[1] + (s64)sT * sTex[5] + sTex[9] + sTex[13]) >> 12);
        }
        break;
    case 0x23: add_vertex((int16_t)p[0], (int16_t)(p[0] >> 16), (int16_t)p[1]); break;
    case 0x24: add_vertex((s32)(p[0] << 22) >> 16, (s32)(p[0] << 12) >> 16, (s32)(p[0] << 2) >> 16); break;
    case 0x25: add_vertex((int16_t)p[0], (int16_t)(p[0] >> 16), sVz); break;
    case 0x26: add_vertex((int16_t)p[0], sVy, (int16_t)(p[0] >> 16)); break;
    case 0x27: add_vertex(sVx, (int16_t)p[0], (int16_t)(p[0] >> 16)); break;
    case 0x28:
        // (differences in eighths of VTX_10's unit: 0.9 fixed point, << 3 to 4.12)
        add_vertex(sVx + (((s32)(p[0] << 22) >> 22) << 3), sVy + (((s32)(p[0] << 12) >> 22) << 3), sVz + (((s32)(p[0] << 2) >> 22) << 3));
        break;
    case 0x29: sPolyAttrNext = p[0]; break;
    case 0x2A: sTexImage = p[0]; sTs = sS; sTt = sT; break;
    case 0x2B: sPltt = p[0] & 0x1FFF; break;
    case 0x30:
        sDifAmb = p[0];
        if (p[0] & 0x8000) sColor = p[0] & 0x7FFF;
        break;
    case 0x31: sSpeEmi = p[0]; sUseShininess = (p[0] >> 15) & 1; break;
    case 0x32: {  // light vector, through the direction matrix
        int i = p[0] >> 30;
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
    sFrames[0].viewport[2] = sFrames[1].viewport[2] = 255;
    sFrames[0].viewport[3] = sFrames[1].viewport[3] = 191;
    // (written straight by the game's assembly: a value no game writes marks "nothing new")
    *IO(0x540) = 0xFFFFFFFFu;
    *IO(0x580) = 0xFFFFFFFFu;
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

void port_gx_fifo_write(u32 word) {
    init_once();
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

void port_gx_fifo_send(const void *src, void *dest, u32 bytes) {
    (void)dest;
    const u32 *s = src;
    for (u32 i = 0; i < bytes / 4; i++) port_gx_fifo_write(s[i]);
}

u32 port_gx_fifo_words(void) { return sFifoWords; }

// Every write to a DS 3D register (the rewritten reg_G3_* / reg_G3X_* assignments).
void port_g3_write(unsigned long addr, unsigned long value) {
    init_once();
    u32 off = (u32)addr - (0x04000000 + PORT_DS_DELTA);
    if (off >= 0x400 && off < 0x440) { port_gx_fifo_write((u32)value); return; }
    if (off >= 0x440 && off < 0x600) {
        u8 cmd = (u8)((off - 0x400) >> 2);
        if (cmd != sPortCmd) { sPortCmd = cmd; sPortN = 0; }
        if (kParams[cmd] == 0) { run(cmd, NULL); return; }
        sPortParams[sPortN++] = (u32)value;
        if (sPortN >= kParams[cmd]) { run(cmd, sPortParams); sPortN = 0; }
        return;
    }
    *IO(off) = (u32)value;  // (rendering registers: just stored, the renderer reads them)
}

// ---------------------------------------------------------------- frames
const G3Frame *port_g3_frame(void) { return sHaveShown ? &sFrames[!sBuild] : NULL; }

int port_g3_vblank(void) {
    init_once();
    extern uint32_t port_trace3d;
    if (port_trace3d && port_frame_count == port_trace3d) {
        port_log("3D trace, frame %lu: command counts:", (unsigned long)port_frame_count);
        for (int i = 0; i < 128; i++) if (sHist[i]) port_log(" %02X:%lu", i, (unsigned long)sHist[i]);
        port_log("\nprojection:");
        for (int i = 0; i < 16; i++) port_log(" %ld", (long)sProj[i]);
        port_log("\n");
        // the frame being shown: triangles with a vertex far off screen, behind or near the camera
        const G3Frame *f = &sFrames[!sBuild];
        int shown = 0;
        port_log("frame: %lu polygons, %lu vertices\n", (unsigned long)f->np, (unsigned long)f->nv);
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
    memset(sHist, 0, sizeof(sHist));
    // (the game's assembly also writes SWAP_BUFFERS straight into its register: seen here)
    volatile u32 *swapReg = IO(0x540);
    if (*swapReg != 0xFFFFFFFFu) {
        sFrames[sBuild].swap = *swapReg;
        *swapReg = 0xFFFFFFFFu;
        sSwapPending = 1;
    }
    volatile u32 *vpReg = IO(0x580);
    if (*vpReg != 0xFFFFFFFFu) {
        memcpy(sFrames[sBuild].viewport, (const void *)vpReg, 4);
        *vpReg = 0xFFFFFFFFu;
    }
    if (!sSwapPending) return 0;
    sSwapPending = 0;
    sBuild ^= 1;
    sHaveShown = 1;
    G3Frame *f = &sFrames[sBuild];
    f->nv = 0;
    f->np = 0;
    memcpy(f->viewport, sFrames[!sBuild].viewport, 4);
    return 1;
}
