// The DS 3D engine in the port: the geometry engine (gx3d.c, in the DS's own fixed-point maths)
// turns the game's 3D commands into polygons; the renderer (render3d.c) draws them on the 3DS GPU.
#ifndef PORT_GX3D_H
#define PORT_GX3D_H

#include <stdint.h>

typedef struct {
    float x, y, z, w;     // clip space, as the DS computes it (x/w, y/w, z/w in -1..1, y up)
    uint8_t r, g, b, a;   // vertex colour (lit), alpha from the polygon
    float s, t;           // texture coordinates, in texels
} G3Vtx;

typedef struct {
    uint32_t polyattr;    // POLYGON_ATTR as latched at BEGIN_VTXS
    uint32_t teximage;    // TEXIMAGE_PARAM
    uint32_t pltt;        // PLTT_BASE
    uint32_t first, count;  // triangles' vertices in the frame's vertex array (count = 3 per triangle)
    uint8_t translucent;
    float s0, s1, t0, t1;   // texture coordinates' bounds (texels)
} G3Poly;

typedef struct {
    G3Vtx *v;
    G3Poly *p;
    uint32_t nv, np;
    uint32_t swap;        // SWAP_BUFFERS parameter
    uint8_t viewport[4];  // x1, y1, x2, y2 (y from the bottom)
    float zn0, zd0, zn1, zd1;  // the opaque vertices' z/w range, as fractions (min zn0/zd0, max zn1/zd1)
} G3Frame;

// The last frame the game swapped in (what the DS would now be showing), or NULL before the first.
const G3Frame *port_g3_frame(void);
// Called at the VBlank: a swap requested during the frame takes effect now.
int port_g3_vblank(void);

#endif
