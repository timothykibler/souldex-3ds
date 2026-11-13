/* NitroSDK's gx/include/gxasm.h for the 3DS port: the fast copies to the geometry FIFO become
   writes to the port's 3D command buffer (port/runtime/gx3d.c). */
#ifndef PORT_GXASM_H
#define PORT_GXASM_H

#include <nitro/types.h>

void port_gx_fifo_write(u32 word);
void port_gx_fifo_send(const void *src, void *dest, u32 bytes);

static inline void GX_SendFifo36B(const void *pSrc, void *pDest) { port_gx_fifo_send(pSrc, pDest, 36); }
static inline void GX_SendFifo48B(const void *pSrc, void *pDest) { port_gx_fifo_send(pSrc, pDest, 48); }
static inline void GX_SendFifo64B(const void *pSrc, void *pDest) { port_gx_fifo_send(pSrc, pDest, 64); }
static inline void GX_SendFifo128B(const void *pSrc, void *pDest) { port_gx_fifo_send(pSrc, pDest, 128); }

#endif
