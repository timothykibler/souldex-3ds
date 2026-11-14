// NitroSDK's MI module (memory copies, DMA, decompression) in plain C. On the DS these were
// hand-written ARM loops or DMA channels; here a DMA "transfer" is a copy that has finished when
// the call returns. Writes to the 3D geometry FIFO go to the port's command buffer.
#include <string.h>
#include <nitro.h>
#include "port.h"

void port_gx_fifo_write(u32 word);
static BOOL is_gxfifo(volatile const void *p) {
    u32 a = (u32)p;
    return a >= REG_GXFIFO_ADDR && a < REG_GXFIFO_ADDR + 0x40;
}

// ---------------------------------------------------------------- CPU copies
void MIi_CpuClear16(u16 data, void *destp, u32 size) {
    u16 *d = destp;
    for (u32 i = 0; i < size / 2; i++) d[i] = data;
}
void MIi_CpuCopy16(const void *srcp, void *destp, u32 size) { memmove(destp, srcp, size & ~1u); }
void MIi_CpuSend16(const void *srcp, volatile void *destp, u32 size) {
    const u16 *s = srcp;
    for (u32 i = 0; i < size / 2; i++) *(vu16 *)destp = s[i];
}
void MIi_CpuClear32(u32 data, void *destp, u32 size) {
    u32 *d = destp;
    for (u32 i = 0; i < size / 4; i++) d[i] = data;
}
void MIi_CpuCopy32(const void *srcp, void *destp, u32 size) { memmove(destp, srcp, size & ~3u); }
void MIi_CpuSend32(const void *srcp, volatile void *destp, u32 size) {
    const u32 *s = srcp;
    if (is_gxfifo(destp)) {  // (display lists: decoded in place, see port_gx_fifo_send; it times itself)
        void port_gx_fifo_send(const void *src, void *dest, u32 bytes);
        port_gx_fifo_send(s, (void *)destp, size & ~3u);
        return;
    }
    for (u32 i = 0; i < size / 4; i++) *(vu32 *)destp = s[i];
}
void MIi_CpuClearFast(u32 data, void *destp, u32 size) { MIi_CpuClear32(data, destp, size); }
void MIi_CpuCopyFast(const void *srcp, void *destp, u32 size) { memmove(destp, srcp, size & ~3u); }
void MI_CpuFill8(void *dest, u8 data, u32 size) { memset(dest, data, size); }
void MI_CpuCopy8(const void *src, void *dest, u32 size) { memmove(dest, src, size); }
void MI_Copy16B(const void *s, void *d) { memcpy(d, s, 16); }
void MI_Copy32B(const void *s, void *d) { memcpy(d, s, 32); }
void MI_Copy36B(const void *s, void *d) { memcpy(d, s, 36); }
void MI_Copy48B(const void *s, void *d) { memcpy(d, s, 48); }
void MI_Copy64B(const void *s, void *d) { memcpy(d, s, 64); }
void MI_Copy128B(const void *s, void *d) { memcpy(d, s, 128); }
void MI_Zero32B(void *d) { memset(d, 0, 32); }
void MI_Zero36B(void *d) { memset(d, 0, 36); }
void MI_Zero48B(void *d) { memset(d, 0, 48); }
void MI_Zero64B(void *d) { memset(d, 0, 64); }
u32 MI_SwapWord(u32 setData, volatile u32 *destp) { u32 o = *destp; *destp = setData; return o; }
u8 MI_SwapByte(u32 setData, volatile u8 *destp) { u8 o = *destp; *destp = (u8)setData; return o; }

// ---------------------------------------------------------------- DMA (immediate)
void MI_Init(void) {}
BOOL MI_IsDmaBusy(u32 dmaNo) { (void)dmaNo; return FALSE; }
void MI_WaitDma(u32 dmaNo) { (void)dmaNo; }
void MI_StopDma(u32 dmaNo) { (void)dmaNo; }

void MI_DmaFill32(u32 dmaNo, void *dest, u32 data, u32 size) { (void)dmaNo; MIi_CpuClear32(data, dest, size); }
void MI_DmaCopy32(u32 dmaNo, const void *src, void *dest, u32 size) { (void)dmaNo; MIi_CpuCopy32(src, dest, size); }
void MI_DmaSend32(u32 dmaNo, const void *src, volatile void *dest, u32 size) { (void)dmaNo; MIi_CpuSend32(src, dest, size); }
void MI_DmaFill16(u32 dmaNo, void *dest, u16 data, u32 size) { (void)dmaNo; MIi_CpuClear16(data, dest, size); }
void MI_DmaCopy16(u32 dmaNo, const void *src, void *dest, u32 size) { (void)dmaNo; MIi_CpuCopy16(src, dest, size); }
void MI_DmaSend16(u32 dmaNo, const void *src, volatile void *dest, u32 size) { (void)dmaNo; MIi_CpuSend16(src, dest, size); }

#define DONE(cb, arg) do { if (cb) (cb)(arg); } while (0)
void MI_DmaFill32Async(u32 n, void *d, u32 v, u32 s, MIDmaCallback cb, void *arg) { MI_DmaFill32(n, d, v, s); DONE(cb, arg); }
void MI_DmaCopy32Async(u32 n, const void *src, void *d, u32 s, MIDmaCallback cb, void *arg) { MI_DmaCopy32(n, src, d, s); DONE(cb, arg); }
void MI_DmaSend32Async(u32 n, const void *src, volatile void *d, u32 s, MIDmaCallback cb, void *arg) { MI_DmaSend32(n, src, d, s); DONE(cb, arg); }
void MI_DmaFill16Async(u32 n, void *d, u16 v, u32 s, MIDmaCallback cb, void *arg) { MI_DmaFill16(n, d, v, s); DONE(cb, arg); }
void MI_DmaCopy16Async(u32 n, const void *src, void *d, u32 s, MIDmaCallback cb, void *arg) { MI_DmaCopy16(n, src, d, s); DONE(cb, arg); }
void MI_DmaSend16Async(u32 n, const void *src, volatile void *d, u32 s, MIDmaCallback cb, void *arg) { MI_DmaSend16(n, src, d, s); DONE(cb, arg); }

// H-blank DMA repeats a copy every scanline (raster effects): not in phase 0.
void MI_HBlankDmaCopy32(u32 n, const void *s, void *d, u32 z) { (void)n; (void)s; (void)d; (void)z; }
void MI_HBlankDmaCopy16(u32 n, const void *s, void *d, u32 z) { (void)n; (void)s; (void)d; (void)z; }
void MI_HBlankDmaCopy32If(u32 n, const void *s, void *d, u32 z) { (void)n; (void)s; (void)d; (void)z; }
void MI_HBlankDmaCopy16If(u32 n, const void *s, void *d, u32 z) { (void)n; (void)s; (void)d; (void)z; }
void MI_VBlankDmaCopy32(u32 n, const void *s, void *d, u32 z) { MI_DmaCopy32(n, s, d, z); }
void MI_VBlankDmaCopy16(u32 n, const void *s, void *d, u32 z) { MI_DmaCopy16(n, s, d, z); }
void MI_VBlankDmaCopy32Async(u32 n, const void *s, void *d, u32 z, MIDmaCallback cb, void *arg) { MI_DmaCopy32(n, s, d, z); DONE(cb, arg); }
void MI_VBlankDmaCopy16Async(u32 n, const void *s, void *d, u32 z, MIDmaCallback cb, void *arg) { MI_DmaCopy16(n, s, d, z); DONE(cb, arg); }
void MIi_CardDmaCopy32(u32 n, const void *s, void *d, u32 z) { MI_DmaCopy32(n, s, d, z); }
void MI_DispMemDmaCopy(u32 n, const void *s) { (void)n; (void)s; }

void MI_SendGXCommand(u32 n, const void *src, u32 len) { (void)n; MIi_CpuSend32(src, (volatile void *)REG_GXFIFO_ADDR, len); }
void MI_SendGXCommandAsync(u32 n, const void *src, u32 len, MIDmaCallback cb, void *arg) { MI_SendGXCommand(n, src, len); DONE(cb, arg); }
void MI_SendGXCommandFast(u32 n, const void *src, u32 len) { MI_SendGXCommand(n, src, len); }
void MI_SendGXCommandAsyncFast(u32 n, const void *src, u32 len, MIDmaCallback cb, void *arg) { MI_SendGXCommand(n, src, len); DONE(cb, arg); }

// ---------------------------------------------------------------- decompression
// (formats as the DS BIOS has them: a 32-bit header, type in the low byte, size above it)
void MI_UncompressLZ8(const void *srcp, void *destp) {
    const u8 *s = srcp;
    u8 *d = destp;
    u32 size = (s[1] | s[2] << 8 | s[3] << 16);
    s += 4;
    u32 out = 0;
    BOOL ext = (((const u8 *)srcp)[0] & 0x0F) != 0;  // LZ77 extended (types 0x11)
    while (out < size) {
        u8 flags = *s++;
        for (int i = 0; i < 8 && out < size; i++, flags <<= 1) {
            if (!(flags & 0x80)) { d[out++] = *s++; continue; }
            u32 len, disp;
            if (!ext) {
                len = (s[0] >> 4) + 3;
                disp = (((s[0] & 0xF) << 8) | s[1]) + 1;
                s += 2;
            } else {
                u32 top = s[0] >> 4;
                if (top == 0) {
                    len = (((s[0] & 0xF) << 4) | (s[1] >> 4)) + 0x11;
                    disp = (((s[1] & 0xF) << 8) | s[2]) + 1;
                    s += 3;
                } else if (top == 1) {
                    len = (((s[0] & 0xF) << 12) | (s[1] << 4) | (s[2] >> 4)) + 0x111;
                    disp = (((s[2] & 0xF) << 8) | s[3]) + 1;
                    s += 4;
                } else {
                    len = top + 1;
                    disp = (((s[0] & 0xF) << 8) | s[1]) + 1;
                    s += 2;
                }
            }
            for (u32 k = 0; k < len && out < size; k++, out++) d[out] = d[out - disp];
        }
    }
}
void MI_UncompressLZ16(const void *srcp, void *destp) { MI_UncompressLZ8(srcp, destp); }

void MI_UncompressRL8(const void *srcp, void *destp) {
    const u8 *s = srcp;
    u8 *d = destp;
    u32 size = (s[1] | s[2] << 8 | s[3] << 16), out = 0;
    s += 4;
    while (out < size) {
        u8 f = *s++;
        if (f & 0x80) {
            u32 n = (f & 0x7F) + 3;
            u8 v = *s++;
            while (n-- && out < size) d[out++] = v;
        } else {
            u32 n = (f & 0x7F) + 1;
            while (n-- && out < size) d[out++] = *s++;
        }
    }
}
void MI_UncompressRL16(const void *srcp, void *destp) { MI_UncompressRL8(srcp, destp); }

void MI_UncompressHuffman(const void *srcp, void *destp) {
    // Tree right after the header (its size byte at +4, the root node at +5); then the bit stream
    // in 32-bit words, most significant bit first. Symbols are packed into 32-bit words from the
    // low end, as the BIOS writes them.
    const u8 *s = srcp;
    u32 bits = s[0] & 0xF;
    u32 size = (s[1] | s[2] << 8 | s[3] << 16);
    const u8 *root = s + 5;
    const u32 *stream = (const u32 *)(s + 4 + (s[4] + 1) * 2);
    u32 *d = destp;
    u32 written = 0, word = 0, nbits = 0;
    const u8 *node = root;
    while (written < size) {
        u32 w = *stream++;
        for (int b = 31; b >= 0 && written < size; b--) {
            u32 bit = (w >> b) & 1;
            const u8 *child = (const u8 *)(((u32)node & ~1u) + (node[0] & 0x3F) * 2 + 2 + bit);
            if (node[0] & (bit ? 0x40 : 0x80)) {
                word |= (u32)child[0] << nbits;
                nbits += bits;
                node = root;
                if (nbits == 32) { *d++ = word; written += 4; word = 0; nbits = 0; }
            } else {
                node = child;
            }
        }
    }
}

void MI_UnfilterDiff8(const void *srcp, void *destp) {
    const u8 *s = srcp;
    u32 size = (s[1] | s[2] << 8 | s[3] << 16);
    s += 4;
    u8 *d = destp, v = 0;
    for (u32 i = 0; i < size; i++) { v += s[i]; d[i] = v; }
}
void MI_UnfilterDiff16(const void *srcp, void *destp) {
    const u8 *s8 = srcp;
    u32 size = (s8[1] | s8[2] << 8 | s8[3] << 16);
    const u16 *s = (const u16 *)(s8 + 4);
    u16 *d = destp, v = 0;
    for (u32 i = 0; i < size / 2; i++) { v += s[i]; d[i] = v; }
}

void MI_UnpackBits(const void *srcp, void *destp, MIUnpackBitsParam *p) {
    const u8 *s = srcp;
    u32 *d = destp;
    u32 out = 0, nout = 0;
    u32 mask = (1u << p->srcBitNum) - 1;
    for (u32 i = 0; i < p->srcNum; i++) {
        for (u32 b = 0; b < 8; b += p->srcBitNum) {
            u32 v = (s[i] >> b) & mask;
            if (v || p->destOffset0_on) v += p->destOffset;
            out |= v << nout;
            nout += p->destBitNum;
            if (nout >= 32) { *d++ = out; out = 0; nout = 0; }
        }
    }
}

void MIi_UncompressBackward(void *bottom) { (void)bottom; port_fatal("compressed overlay load"); }
