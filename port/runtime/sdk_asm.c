// The NitroSDK functions that were Metrowerks inline assembly (cut out of their files by
// port/sdkfix.py), in C: matrix setup (NitroSDK's row-vector convention, fx32 = 20.12 fixed
// point), the quicksort, and the DS BIOS calls the game makes through SVC_*.
#include <stdlib.h>
#include <string.h>
#include <nitro.h>
#include "port.h"

// ---------------------------------------------------------------- 2x2
void MTX_Identity22_(MtxFx22 *d) { d->_00 = FX32_ONE; d->_01 = 0; d->_10 = 0; d->_11 = FX32_ONE; }
void MTX_Transpose22_(const MtxFx22 *s, MtxFx22 *d) { fx32 t = s->_01; d->_00 = s->_00; d->_01 = s->_10; d->_10 = t; d->_11 = s->_11; }
void MTX_Scale22_(MtxFx22 *d, fx32 x, fx32 y) { d->_00 = x; d->_01 = 0; d->_10 = 0; d->_11 = y; }
void MTX_Rot22_(MtxFx22 *d, fx32 s, fx32 c) { d->_00 = c; d->_01 = s; d->_10 = -s; d->_11 = c; }

// ---------------------------------------------------------------- 3x3
void MTX_Identity33_(MtxFx33 *d) {
    memset(d, 0, sizeof(*d));
    d->_00 = d->_11 = d->_22 = FX32_ONE;
}
void MTX_Copy33To43_(const MtxFx33 *s, MtxFx43 *d) {
    for (int r = 0; r < 3; r++) for (int c = 0; c < 3; c++) d->m[r][c] = s->m[r][c];
    d->m[3][0] = d->m[3][1] = d->m[3][2] = 0;
}
void MTX_Copy33To44_(const MtxFx33 *s, MtxFx44 *d) {
    memset(d, 0, sizeof(*d));
    for (int r = 0; r < 3; r++) for (int c = 0; c < 3; c++) d->m[r][c] = s->m[r][c];
    d->_33 = FX32_ONE;
}
void MTX_Transpose33_(const MtxFx33 *s, MtxFx33 *d) {
    MtxFx33 t;
    for (int r = 0; r < 3; r++) for (int c = 0; c < 3; c++) t.m[c][r] = s->m[r][c];
    *d = t;
}
void MTX_Scale33_(MtxFx33 *d, fx32 x, fx32 y, fx32 z) {
    memset(d, 0, sizeof(*d));
    d->_00 = x; d->_11 = y; d->_22 = z;
}
void MTX_RotX33_(MtxFx33 *d, fx32 s, fx32 c) {
    d->_00 = FX32_ONE; d->_01 = 0; d->_02 = 0;
    d->_10 = 0; d->_11 = c; d->_12 = s;
    d->_20 = 0; d->_21 = -s; d->_22 = c;
}
void MTX_RotY33_(MtxFx33 *d, fx32 s, fx32 c) {
    d->_00 = c; d->_01 = 0; d->_02 = -s;
    d->_10 = 0; d->_11 = FX32_ONE; d->_12 = 0;
    d->_20 = s; d->_21 = 0; d->_22 = c;
}
void MTX_RotZ33_(MtxFx33 *d, fx32 s, fx32 c) {
    d->_00 = c; d->_01 = s; d->_02 = 0;
    d->_10 = -s; d->_11 = c; d->_12 = 0;
    d->_20 = 0; d->_21 = 0; d->_22 = FX32_ONE;
}

// ---------------------------------------------------------------- 4x3
void MTX_Identity43_(MtxFx43 *d) {
    memset(d, 0, sizeof(*d));
    d->_00 = d->_11 = d->_22 = FX32_ONE;
}
void MTX_Copy43To44_(const MtxFx43 *s, MtxFx44 *d) {
    for (int r = 0; r < 4; r++) {
        for (int c = 0; c < 3; c++) d->m[r][c] = s->m[r][c];
        d->m[r][3] = (r == 3) ? FX32_ONE : 0;
    }
}
void MTX_Transpose43_(const MtxFx43 *s, MtxFx43 *d) {
    MtxFx43 t;
    for (int r = 0; r < 3; r++) for (int c = 0; c < 3; c++) t.m[c][r] = s->m[r][c];
    t.m[3][0] = t.m[3][1] = t.m[3][2] = 0;
    *d = t;
}
void MTX_Scale43_(MtxFx43 *d, fx32 x, fx32 y, fx32 z) {
    memset(d, 0, sizeof(*d));
    d->_00 = x; d->_11 = y; d->_22 = z;
}
void MTX_RotX43_(MtxFx43 *d, fx32 s, fx32 c) { MTX_RotX33_((MtxFx33 *)d, s, c); d->_30 = d->_31 = d->_32 = 0; }
void MTX_RotY43_(MtxFx43 *d, fx32 s, fx32 c) { MTX_RotY33_((MtxFx33 *)d, s, c); d->_30 = d->_31 = d->_32 = 0; }
void MTX_RotZ43_(MtxFx43 *d, fx32 s, fx32 c) { MTX_RotZ33_((MtxFx33 *)d, s, c); d->_30 = d->_31 = d->_32 = 0; }

// ---------------------------------------------------------------- 4x4
void MTX_Identity44_(MtxFx44 *d) {
    memset(d, 0, sizeof(*d));
    d->_00 = d->_11 = d->_22 = d->_33 = FX32_ONE;
}
void MTX_Copy44To33_(const MtxFx44 *s, MtxFx33 *d) {
    for (int r = 0; r < 3; r++) for (int c = 0; c < 3; c++) d->m[r][c] = s->m[r][c];
}
void MTX_Copy44To43_(const MtxFx44 *s, MtxFx43 *d) {
    for (int r = 0; r < 4; r++) for (int c = 0; c < 3; c++) d->m[r][c] = s->m[r][c];
}
void MTX_Transpose44_(const MtxFx44 *s, MtxFx44 *d) {
    MtxFx44 t;
    for (int r = 0; r < 4; r++) for (int c = 0; c < 4; c++) t.m[c][r] = s->m[r][c];
    *d = t;
}
void MTX_Scale44_(MtxFx44 *d, fx32 x, fx32 y, fx32 z) {
    memset(d, 0, sizeof(*d));
    d->_00 = x; d->_11 = y; d->_22 = z; d->_33 = FX32_ONE;
}
static void rot44(MtxFx44 *d, void (*f)(MtxFx33 *, fx32, fx32), fx32 s, fx32 c) {
    MtxFx33 m;
    f(&m, s, c);
    MTX_Copy33To44_(&m, d);
}
void MTX_RotX44_(MtxFx44 *d, fx32 s, fx32 c) { rot44(d, MTX_RotX33_, s, c); }
void MTX_RotY44_(MtxFx44 *d, fx32 s, fx32 c) { rot44(d, MTX_RotY33_, s, c); }
void MTX_RotZ44_(MtxFx44 *d, fx32 s, fx32 c) { rot44(d, MTX_RotZ33_, s, c); }

// ---------------------------------------------------------------- MATH_QSort
static MATHCompareFunc sQsortCompare;
static int qsort_adapter(const void *a, const void *b) { return sQsortCompare((void *)a, (void *)b); }
void MATH_QSort(void *head, u32 num, u32 width, MATHCompareFunc comp, void *stackBuf) {
    (void)stackBuf;
    MATHCompareFunc saved = sQsortCompare;
    sQsortCompare = comp;
    qsort(head, num, width, qsort_adapter);
    sQsortCompare = saved;
}

// ---------------------------------------------------------------- the DS BIOS calls the game makes
void SVC_WaitByLoop(s32 count) { (void)count; }
// CpuSet: bits 0-20 the count (halfwords, or words with bit 26), bit 24 fill from one source value.
void SVC_CpuSet(const void *srcp, void *destp, u32 cnt) {
    u32 n = cnt & 0x1FFFFF;
    BOOL fill = (cnt >> 24) & 1, words = (cnt >> 26) & 1;
    if (words) {
        const u32 *s = srcp; u32 *d = destp;
        for (u32 i = 0; i < n; i++) d[i] = fill ? s[0] : s[i];
    } else {
        const u16 *s = srcp; u16 *d = destp;
        for (u32 i = 0; i < n; i++) d[i] = fill ? s[0] : s[i];
    }
}
void SVC_CpuSetFast(const void *srcp, void *destp, u32 cnt) { SVC_CpuSet(srcp, destp, cnt | (1u << 26)); }
void SVC_UncompressLZ8(const void *srcp, void *destp) { MI_UncompressLZ8(srcp, destp); }
void SVC_UncompressRL8(const void *srcp, void *destp) { MI_UncompressRL8(srcp, destp); }
s32 SVC_Div(s32 number, s32 denom) { return denom ? number / denom : 0; }
s32 SVC_DivRem(s32 number, s32 denom) { return denom ? number % denom : number; }
u16 SVC_Sqrt(u32 src) {
    u32 r = 0, bit = 1u << 30;
    while (bit > src) bit >>= 2;
    while (bit) { if (src >= r + bit) { src -= r + bit; r = (r >> 1) + bit; } else r >>= 1; bit >>= 2; }
    return (u16)r;
}
void SVC_Halt(void) { port_frame(); }
void SVC_UnpackBits(const void *srcp, void *destp, const MIUnpackBitsParam *paramp) {
    MI_UnpackBits(srcp, destp, (MIUnpackBitsParam *)paramp);
}
u16 SVC_GetCRC16(u32 start, const void *datap, u32 size) {
    static const u16 table[16] = { 0x0000, 0xCC01, 0xD801, 0x1400, 0xF001, 0x3C00, 0x2800, 0xE401,
                                   0xA001, 0x6C00, 0x7800, 0xB401, 0x5000, 0x9C01, 0x8801, 0x4400 };
    u16 crc = (u16)start;
    const u16 *p = datap;
    for (u32 i = 0; i < size / 2; i++) {
        u16 v = p[i];
        for (int k = 0; k < 4; k++) {
            u16 t = table[crc & 0xF];
            crc = (crc >> 4) ^ t ^ table[(v >> (k * 4)) & 0xF];
        }
    }
    return crc;
}
