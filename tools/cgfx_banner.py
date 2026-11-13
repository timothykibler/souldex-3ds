"""A layered, stereoscopic HOME Menu banner: two transparent layers at different depths, full colour.

bannertool's own banner (makebanner -i) is one flat quad with a 256x128 RGBA4 texture: 16 levels
per channel, so gradients band, and flat. This takes that banner's CGFX as a template and changes
only what has to change:
  - the texture becomes a 256x256 RGBA8 atlas: the front layer above, the back layer below
    (the data is the file's last block, so it grows in place);
  - the mesh draws two quads instead of one, the front nearer the camera than the back: new vertex
    and index buffers go after the texture, and the mesh's offsets and sizes point at them;
  - the shape's bounding box gets the depth the layers span (no culling).
bannertool then packages the CGFX (makebanner -ci), compression and all.

    python3 tools/cgfx_banner.py <template.bnr> <front.png> <back.png> <out.cgfx>

Layers are 256x128 RGBA images in banner space. Layout facts (bannertool 1.2.2's template,
checked against its own output): texture rows top-down in 8x8 tiles, Z-order inside a tile;
v = 1 is the quad's top; the quad spans x -13..13, y -7.5..5.5 at z 0.
"""
import struct
import sys

from PIL import Image

sys.path.insert(0, __import__("os").path.dirname(__file__))

Z_FRONT, Z_BACK = 1.5, -1.5  # (the banner is 26 units wide)

# offsets in bannertool 1.2.2's banner CGFX (checked below before patching)
TXOB = 0x1010     # the image texture
SHAPE = 0xD4C     # the quad's SOBJ shape
IDX_SIZE, IDX_OFS = 0xE10, 0xE14     # index stream: size in bytes (u8 indices), relative offset
VTX_SIZE, VTX_OFS = 0xE48, 0xE4C     # interleaved vertex stream: size, relative offset (stride 20)
OBB_DEPTH = 0xDD0                    # the shape's bounding box depth (float)
IMAG = 0x14F8


def lz11_decompress(d):
    assert d[0] == 0x11
    n = d[1] | d[2] << 8 | d[3] << 16
    i = 4
    out = bytearray()
    while len(out) < n:
        f = d[i]; i += 1
        for bit in range(8):
            if len(out) >= n:
                break
            if f & (0x80 >> bit):
                b0 = d[i]; ind = b0 >> 4
                if ind == 0:
                    ln = ((b0 & 0xF) << 4 | d[i + 1] >> 4) + 0x11; disp = ((d[i + 1] & 0xF) << 8 | d[i + 2]) + 1; i += 3
                elif ind == 1:
                    ln = ((b0 & 0xF) << 12 | d[i + 1] << 4 | d[i + 2] >> 4) + 0x111; disp = ((d[i + 2] & 0xF) << 8 | d[i + 3]) + 1; i += 4
                else:
                    ln = ind + 1; disp = ((b0 & 0xF) << 8 | d[i + 1]) + 1; i += 2
                for _ in range(ln):
                    out.append(out[-disp])
            else:
                out.append(d[i]); i += 1
    return bytes(out)


def u32(b, o):
    return struct.unpack_from("<I", b, o)[0]


def morton(x, y):
    m = 0
    for i in range(3):
        m |= ((x >> i) & 1) << (2 * i) | ((y >> i) & 1) << (2 * i + 1)
    return m


def rgba8_tiled(img):
    """PICA RGBA8: 8x8 tiles, rows top-down, Z-order in a tile, each texel A, B, G, R."""
    w, h = img.size
    px = img.load()
    out = bytearray(w * h * 4)
    for y in range(h):
        for x in range(w):
            r, g, b, a = px[x, y]
            if a == 0:
                r = g = b = 0
            i = (((y >> 3) * (w >> 3) + (x >> 3)) * 64 + morton(x & 7, y & 7)) * 4
            out[i:i + 4] = bytes((a, b, g, r))
    return bytes(out)


def build(template_bnr, front, back, out):
    bnr = open(template_bnr, "rb").read()
    g = bytearray(lz11_decompress(bnr[u32(bnr, 8):]))
    # the template is what this expects
    assert g[:4] == b"CGFX" and g[TXOB:TXOB + 4] == b"TXOB" and g[SHAPE:SHAPE + 4] == b"SOBJ" and g[IMAG:IMAG + 4] == b"IMAG", "unexpected bannertool template"
    assert u32(g, TXOB + 0x14) == 128 and u32(g, TXOB + 0x18) == 256 and u32(g, TXOB + 0x30) == 4 and u32(g, TXOB + 0x40) == 0x10000
    assert u32(g, IDX_SIZE) == 6 and u32(g, VTX_SIZE) == 80 and u32(g, 0xE58) == 20
    tex_at = TXOB + 0x44 + u32(g, TXOB + 0x44)
    assert tex_at + 0x10000 == len(g) == IMAG + u32(g, IMAG + 4)

    atlas = Image.new("RGBA", (256, 256))
    atlas.paste(Image.open(front).convert("RGBA").resize((256, 128)), (0, 0))
    atlas.paste(Image.open(back).convert("RGBA").resize((256, 128)), (0, 128))
    tex = rgba8_tiled(atlas)

    # vertices: (x, y, z, u, v); the front layer samples the atlas's top half (v 0.5..1)
    verts = b""
    for z, v0 in ((Z_BACK, 0.0), (Z_FRONT, 0.5)):
        for x, y, u, v in ((-13, -7.5, 0, 0), (13, -7.5, 1, 0), (-13, 5.5, 0, 1), (13, 5.5, 1, 1)):
            verts += struct.pack("<5f", x, y, z, u, v0 + 0.5 * v)
    idx = bytes((0, 1, 2, 1, 3, 2, 4, 5, 6, 5, 7, 6))  # (back first, then front over it)

    g = g[:tex_at] + tex
    vtx_at = len(g); g += verts
    idx_at = len(g); g += idx
    while len(g) % 16:
        g += b"\0"
    struct.pack_into("<I", g, TXOB + 0x14, 256)             # height
    struct.pack_into("<I", g, TXOB + 0x20, 0x1401)          # GL type: unsigned byte
    struct.pack_into("<I", g, TXOB + 0x30, 0)               # PICA format: RGBA8
    struct.pack_into("<I", g, TXOB + 0x38, 256)             # image: height
    struct.pack_into("<I", g, TXOB + 0x40, len(tex))        # image: size
    struct.pack_into("<I", g, TXOB + 0x4C, 32)              # image: bits per pixel
    struct.pack_into("<I", g, VTX_SIZE, len(verts))
    struct.pack_into("<I", g, VTX_OFS, vtx_at - VTX_OFS)
    struct.pack_into("<I", g, IDX_SIZE, len(idx))
    struct.pack_into("<I", g, IDX_OFS, idx_at - IDX_OFS)
    struct.pack_into("<f", g, OBB_DEPTH, Z_FRONT - Z_BACK)
    struct.pack_into("<I", g, IMAG + 4, len(g) - IMAG)
    struct.pack_into("<I", g, 0x0C, len(g))
    assert len(g) < 512 * 1024
    open(out, "wb").write(g)
    print(f"cgfx_banner: {len(g)} bytes, two layers at z {Z_FRONT} / {Z_BACK}")


if __name__ == "__main__":
    build(*sys.argv[1:5])
