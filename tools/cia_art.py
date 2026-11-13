"""HOME Menu art for the CIA, from the player's own ROM (nothing of the game is kept in the repo):
the DS banner's 32x32 icon and English title.

    python3 tools/cia_art.py <rom.nds> <out dir> [--keyart image]

With --keyart (your own 2:1 artwork, kept out of the repo): the banner is layered, as the 3DS's
own games' are: the title logo and the mascot cut out of the art (flood fill from the edges through
its light background, the outline sealed first), on transparency, the logo in front
(banner_front.png, banner_back.png: 256x128; tools/cgfx_banner.py makes them a stereoscopic
banner). --logo-box / --mascot-box x0,y0,x1,y1 choose the regions (defaults suit the SoulSilver
key art); the icon is a square crop (--icon-box x0,y0,size; default: Lugia) scaled to 48x48.
Boxes are in the image's pixels, given as if it were 1600x800 (scaled to the real size).

Writes icon.png (48x48: the icon at 1.5x, as 3x nearest then halved, so its pixels stay even),
banner.png (256x128: the icon at 3x beside the title) and titles.txt (short title, long title,
publisher, one per line) for bannertool.
"""
import struct
import sys
from collections import deque

from PIL import Image, ImageDraw, ImageFilter, ImageFont

FONT = "/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf"
FONT_REG = "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf"


def ds_banner(rom):
    with open(rom, "rb") as f:
        head = f.read(0x200)
        off, = struct.unpack_from("<I", head, 0x68)
        f.seek(off)
        b = f.read(0x840)
    pal = struct.unpack_from("<16H", b, 0x220)
    icon = Image.new("RGBA", (32, 32))
    px = icon.load()
    for t in range(16):  # 4x4 tiles of 8x8, 4 bits per pixel, low nibble first
        tx, ty = t % 4, t // 4
        for i in range(32):
            v = b[0x20 + t * 32 + i]
            for k in range(2):
                idx = (v >> (4 * k)) & 15
                x, y = tx * 8 + (i % 4) * 2 + k, ty * 8 + i // 4
                c = pal[idx]
                r, g, bl = (c & 31) << 3, ((c >> 5) & 31) << 3, ((c >> 10) & 31) << 3
                px[x, y] = (r | r >> 5, g | g >> 5, bl | bl >> 5, 0 if idx == 0 else 255)
    title = b[0x340:0x440].decode("utf-16-le").split("\0")[0]
    return icon, title.split("\n")


def scaled(icon, factor3):
    """The icon at 1.5x (factor3 = 1) or 3x (factor3 = 2): 3x nearest, then halved if asked."""
    big = icon.resize((96, 96), Image.NEAREST)
    return big.resize((48, 48), Image.BOX) if factor3 == 1 else big


def gradient(w, h, top, bottom):
    img = Image.new("RGBA", (w, h))
    d = ImageDraw.Draw(img)
    for y in range(h):
        t = y / max(h - 1, 1)
        d.line([(0, y), (w, y)], fill=tuple(int(top[i] + (bottom[i] - top[i]) * t) for i in range(3)) + (255,))
    return img


def cutout(im, box, seal):
    """What's inside the box but not its light background: a flood fill from the box's edges
    through light, unsaturated pixels, first with the outline thickened by `seal` px (closing its
    gaps), then grown back up to the real outline. Returns RGBA cropped to the subject."""
    c = im.crop(box)
    w, h = c.size
    px = c.load()
    bar = Image.new("L", (w, h))
    bp = bar.load()
    for y in range(h):
        for x in range(w):
            r, g, b = px[x, y]
            if not (max(r, g, b) - min(r, g, b) < 48 and r + g + b > 3 * 95):
                bp[x, y] = 255
    tk = (bar.filter(ImageFilter.MaxFilter(2 * seal + 1)) if seal else bar).load()
    seen = bytearray(w * h)
    q = deque([(x, 0) for x in range(w)] + [(x, h - 1) for x in range(w)] + [(0, y) for y in range(h)] + [(w - 1, y) for y in range(h)])
    while q:
        x, y = q.popleft()
        i = y * w + x
        if seen[i] or tk[x, y]:
            continue
        seen[i] = 1
        for nx, ny in ((x - 1, y), (x + 1, y), (x, y - 1), (x, y + 1)):
            if 0 <= nx < w and 0 <= ny < h:
                q.append((nx, ny))
    q = deque((x, y, 0) for y in range(h) for x in range(w) if seen[y * w + x])
    while q:
        x, y, d = q.popleft()
        if d > seal:
            continue
        for nx, ny in ((x - 1, y), (x + 1, y), (x, y - 1), (x, y + 1)):
            if 0 <= nx < w and 0 <= ny < h and not seen[ny * w + nx] and not bp[nx, ny]:
                seen[ny * w + nx] = 1
                q.append((nx, ny, d + 1))
    m = Image.frombytes("L", (w, h), bytes(0 if v else 255 for v in seen))
    m = m.filter(ImageFilter.MinFilter(3)).filter(ImageFilter.GaussianBlur(1.0))  # (no light fringe; a soft edge)
    r = c.convert("RGBA")
    r.putalpha(m)
    return r.crop(m.point(lambda v: 255 if v > 8 else 0).getbbox())


def placed(sub, x, y, w=None, h=None):
    """A 256x128 transparent layer with the cut-out scaled (premultiplied: no fringes) and placed."""
    if w is None:
        w = round(sub.width * h / sub.height)
    if h is None:
        h = round(sub.height * w / sub.width)
    s = sub.convert("RGBa").resize((w, h), Image.LANCZOS).convert("RGBA")
    s = s.filter(ImageFilter.UnsharpMask(radius=1, percent=50, threshold=2))
    layer = Image.new("RGBA", (256, 128), (0, 0, 0, 0))
    layer.alpha_composite(s, (x, y))
    return layer


def keyart(path, out, box, logobox, mascotbox):
    """Banner layers and icon from the player's own artwork (any size, 2:1 best)."""
    im = Image.open(path).convert("RGB")
    w, h = im.size
    if w * 128 != h * 256:  # (centre-crop to 2:1)
        if w * 128 > h * 256:
            cw = h * 2
            im = im.crop(((w - cw) // 2, 0, (w - cw) // 2 + cw, h))
        else:
            ch = w // 2
            im = im.crop((0, (h - ch) // 2, w, (h - ch) // 2 + ch))
        w, h = im.size
    k = w / 1600.0
    sc = lambda b: tuple(int(v * k) for v in b)
    logo = cutout(im, sc(logobox or (40, 260, 840, 620)), max(2, int(3 * k)))      # (the title logo)
    mascot = cutout(im, sc(mascotbox or (860, 20, 1600, 800)), max(3, int(6 * k)))  # (Lugia)
    back = placed(mascot, 138, 2, h=124)
    front = placed(logo, 4, 22, w=180)
    back.save(f"{out}/banner_back.png")
    front.save(f"{out}/banner_front.png")
    flat = back.copy()
    flat.alpha_composite(front)
    flat.save(f"{out}/banner.png")  # (the flat banner: transparent too)
    sharp = ImageFilter.UnsharpMask(radius=1, percent=60, threshold=2)
    x0, y0, size = (int(v * k) for v in (box if box else (860, 40, 760)))  # (Lugia)
    size = min(size, w - x0, h - y0)
    im.crop((x0, y0, x0 + size, y0 + size)).resize((48, 48), Image.LANCZOS).filter(sharp).save(f"{out}/icon.png")


def main():
    args = sys.argv[1:]
    art = box = logobox = mascotbox = None
    if "--keyart" in args:
        i = args.index("--keyart"); art = args[i + 1]; del args[i:i + 2]
    if "--logo-box" in args:
        i = args.index("--logo-box"); logobox = tuple(int(v) for v in args[i + 1].split(",")); del args[i:i + 2]
    if "--mascot-box" in args:
        i = args.index("--mascot-box"); mascotbox = tuple(int(v) for v in args[i + 1].split(",")); del args[i:i + 2]
    if "--icon-box" in args:
        i = args.index("--icon-box"); box = tuple(int(v) for v in args[i + 1].split(",")); del args[i:i + 2]
    rom, out = args[0], args[1]
    icon, lines = ds_banner(rom)
    name = lines[0] if lines else "Pokemon SoulSilver"
    maker = lines[-1] if len(lines) > 1 else ""

    # icon: on a silver tile (the HOME Menu icon has no transparency)
    tile = gradient(48, 48, (250, 251, 253), (214, 220, 230))
    tile.alpha_composite(scaled(icon, 1))
    tile.convert("RGB").save(f"{out}/icon.png")

    # banner: silver, the icon large on the left, the title on the right
    ban = gradient(256, 128, (238, 241, 246), (168, 178, 196))
    d = ImageDraw.Draw(ban)
    shadow = Image.new("RGBA", (256, 128), (0, 0, 0, 0))  # (a soft shadow under the icon: it has its own frame)
    ImageDraw.Draw(shadow).rectangle([18, 22, 18 + 96, 22 + 96], fill=(30, 40, 70, 90))
    ban.alpha_composite(shadow.filter(ImageFilter.GaussianBlur(4)))
    ban.alpha_composite(scaled(icon, 2), (14, 16))
    words = name.split(" ", 1)  # ("Pokémon", "SoulSilver")
    big = ImageFont.truetype(FONT, 22)
    small = ImageFont.truetype(FONT_REG, 12)
    y = 30
    for w in words:
        d.text((124, y), w, font=big, fill=(28, 37, 65, 255))
        y += 28
    d.text((125, y + 6), maker, font=small, fill=(60, 70, 95, 255))
    ban.convert("RGB").save(f"{out}/banner.png")

    if art:
        keyart(art, out, box, logobox, mascotbox)  # (over the ROM-made banner and icon)
    with open(f"{out}/titles.txt", "w", encoding="utf-8") as f:
        f.write(f"{name}\n{name} (3DS port)\n{maker}\n" if maker else f"{name}\n{name} (3DS port)\npersonal build\n")
    print(f"cia_art: {name!r}")


if __name__ == "__main__":
    main()
