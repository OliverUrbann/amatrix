#!/usr/bin/env python3
"""Writes the Workbench tool icon for Amatrix: 8 colours in the MagicWB
palette (3 bitplanes), with a second image for the selected state.

    make-icon.py OUT.info [STACK] [new]

"new": the AppIcon for unread messages (a pink badge on the bubble);
"drawer", "text", "guide": the drawer, readme and AmigaGuide icons - MagicWB
template icons (templates/*.info) with the small Amatrix logo drawn in.
The picture is drawn in icon_art.py.

After the classic planar images an OS 3.5 colour icon follows (IFF FORM
ICON: FACE + two IMAG chunks) with colour 0 transparent and no frame, so
OS 3.5+ Workbench shows the icon without the grey box; older systems
ignore it and use the planar images.
"""
import os, struct, sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import icon_art

DEPTH = 3


def planes(art, depth=DEPTH):
    w, h = len(art[0]), len(art)
    words = (w + 15) // 16
    out = b""
    for bit in range(depth):
        for row in art:
            data = bytearray(words * 2)
            for x, pen in enumerate(row):
                if pen >> bit & 1:
                    data[x // 8] |= 0x80 >> (x % 8)
            out += bytes(data)
    return w, h, out


def image(art, depth=DEPTH):
    w, h, data = planes(art, depth)
    head = struct.pack(">hhhhhIBBI", 0, 0, w, h, depth, 1, (1 << depth) - 1, 0, 0)
    assert len(head) == 20
    return w, h, head + data


def chunk(cid, data):
    out = cid + struct.pack(">I", len(data)) + data
    return out + (b"\0" if len(data) & 1 else b"")


def imag(art):
    pixels = bytes(pen for row in art for pen in row)
    pal = bytes(c for rgb in icon_art.PAL for c in rgb)
    head = struct.pack(">BBBBBBHH",
                       0,                 # transparent colour
                       len(icon_art.PAL) - 1,
                       0x03,              # transparent + palette attached
                       0, 0,              # image and palette uncompressed
                       DEPTH,
                       len(pixels) - 1, len(pal) - 1)
    return chunk(b"IMAG", head + pixels + pal)


def colour_icon(art1, art2):
    w, h = len(art1[0]), len(art1)
    face = struct.pack(">BBBBH", w - 1, h - 1,
                       0x01,              # frameless
                       0x11,              # aspect 1:1
                       len(icon_art.PAL) * 3 - 1)
    body = b"ICON" + chunk(b"FACE", face) + imag(art1) + imag(art2)
    return b"FORM" + struct.pack(">I", len(body)) + body


def build(art1, art2, wbtype=3, stack=65536, tool=None, drawer=False, classic=False):
    """WBTOOL=3, WBDRAWER=2, WBPROJECT=4; tool = DefaultTool of a project.
    classic: 4 colours in the standard palette, no OS 3.5 colour image (looks
    the same on every Workbench); art2 None: selected = complemented"""
    depth = 2 if classic else DEPTH
    w, h, img1 = image(art1, depth)
    img2 = image(art2, depth)[2] if art2 else b""
    gadget = struct.pack(">IhhhhHHHIIIIIHI",
                         0, 0, 0, w, h + 1,
                         0x0006 if art2 else 0x0004,   # GADGIMAGE (+GADGHIMAGE: own selected image, else complement)
                         0x0003,          # RELVERIFY | GADGIMMEDIATE
                         0x0001,          # BOOLGADGET
                         1, 1 if art2 else 0, 0, 0, 0,   # GadgetRender, SelectRender (non-zero = images follow)
                         0,
                         1)               # UserData: revision 1 (OS 2 icon)
    assert len(gadget) == 44
    disk = struct.pack(">HH", 0xE310, 1) + gadget + struct.pack(
        ">BBIIiiIII", wbtype, 0,
        1 if tool else 0, 0,              # DefaultTool, ToolTypes
        -0x80000000, -0x80000000,         # NO_ICON_POSITION
        1 if drawer else 0, 0, stack)
    assert len(disk) == 78
    dd = b""
    if drawer:                            # DrawerData: NewWindow (48 bytes) + CurrentX/Y
        dd = struct.pack(">HHHHBBIIIIIIIHHHHH", 60, 40, 400, 200, 255, 255,
                         0, 0xF, 0, 0, 0, 0, 0, 90, 40, 65535, 65535, 1)
        dd += struct.pack(">ii", 0, 0)
        assert len(dd) == 56
    dt = b""
    if tool:
        s = tool.encode("latin-1") + b"\0"
        dt = struct.pack(">I", len(s)) + s
    return disk + dd + img1 + img2 + dt + (b"" if classic else colour_icon(art1, art2))


def parse_info(data):
    """a classic .info: header fields and the images as rows of pens"""
    g = struct.unpack(">IhhhhHHHIIIIIHI", data[4:48])
    typ, _, deftool, tooltypes, _, _, drawerdata, _, stack = struct.unpack(">BBIIiiIII", data[48:78])
    off = 78
    dd = b""
    if drawerdata:
        dd = data[off:off + 56]; off += 56
    images = []
    for has in (g[8], g[9]):
        if not has: continue
        _, _, w, h, d = struct.unpack(">hhhhh", data[off:off + 10])
        bpr = (w + 15) // 16 * 2
        raw = data[off + 20:off + 20 + bpr * h * d]
        pix = [[0] * w for _ in range(h)]
        for b in range(d):
            for y in range(h):
                row = raw[(b * h + y) * bpr:(b * h + y + 1) * bpr]
                for x in range(w):
                    if row[x // 8] & (0x80 >> (x % 8)): pix[y][x] |= 1 << b
        images.append(pix)
        off += 20 + bpr * h * d
    rest = data[off:]
    if deftool:                                       # skip the template's default tool
        n = struct.unpack(">I", rest[:4])[0]; rest = rest[4 + n:]
    return dict(dd=dd, images=images, tooltypes=rest if tooltypes else b"")


def blit(pix, art, x0, y0):
    for y, row in enumerate(art):
        for x, pen in enumerate(row):
            if pen and 0 <= y0 + y < len(pix) and 0 <= x0 + x < len(pix[0]): pix[y0 + y][x0 + x] = pen


def from_template(name, wbtype, tool, decorate, stack=4096):
    """a MagicWB template icon (templates/NAME.info) with our logo drawn in;
    its tool types (the artist's credit) are kept"""
    here = os.path.dirname(os.path.abspath(__file__))
    t = parse_info(open(os.path.join(here, "templates", name + ".info"), "rb").read())
    a1, a2 = t["images"]
    if t["dd"]:
        # a drawer: not the template's window and view settings, but a plain
        # window that shows only the files with icons, as icons
        t["dd"] = struct.pack(">HHHHBBIIIIIIIHHHHH", 60, 40, 400, 200, 255, 255,
                              0, 0xF, 0, 0, 0, 0, 0, 90, 40, 65535, 65535, 1) + struct.pack(">ii", 0, 0)
        t["tooltypes"] = t["tooltypes"][:-6] + struct.pack(">IH", 1, 1)   # dd_Flags: icons only, dd_ViewModes: as icons
    decorate(a1, False); decorate(a2, True)
    w, h, img1 = image(a1)
    _, _, img2 = image(a2)
    gadget = struct.pack(">IhhhhHHHIIIIIHI", 0, 0, 0, w, h + 1, 0x0006, 0x0003, 0x0001, 1, 1, 0, 0, 0, 0, 1)
    disk = struct.pack(">HH", 0xE310, 1) + gadget + struct.pack(
        ">BBIIiiIII", wbtype, 0, 1 if tool else 0, 1 if t["tooltypes"] else 0,
        -0x80000000, -0x80000000, 1 if t["dd"] else 0, 0, stack)
    dt = b""
    if tool:
        s = tool.encode("latin-1") + b"\0"
        dt = struct.pack(">I", len(s)) + s
    return disk + t["dd"] + img1 + img2 + dt + t["tooltypes"] + colour_icon(a1, a2)


def pink_shift(a1, a2):
    """how far the selected image of a template is moved (by its pink pixels)"""
    def first(a):
        for y, row in enumerate(a):
            for x, pen in enumerate(row):
                if pen == 7: return x, y
        return 0, 0
    (x1, y1), (x2, y2) = first(a1), first(a2)
    return x2 - x1, y2 - y1


def deco_drawer(pix, sel):
    blit(pix, icon_art.mini_logo(both=False), 30 + (-3 if sel else 0), 5 + (2 if sel else 0))


def deco_guide(pix, sel):
    blit(pix, icon_art.mini_logo(), 7 + sel, 5 + sel)


def deco_text(pix, sel):
    blit(pix, icon_art.mini_logo(), 5 + sel, 5 + sel)
    for k, y in enumerate(range(8, 24, 3)):           # lines of text beside it
        for x in range(34 + sel, 49 + sel - (5 if k % 3 == 2 else 0)):
            pix[y + sel][x] = 1
            pix[y + sel + 1][x] = 2


def deco_program(pix, sel, badge=False):
    blit(pix, icon_art.mini_logo(), 14 + sel, 5 + sel)
    if badge:                                         # unread: a pink disc with "!"
        cx, cy = 43 + sel, 9 + sel
        for y in range(-4, 5):
            for x in range(-4, 5):
                d = x * x + y * y
                if d <= 18: pix[cy + y][cx + x] = 1 if d > 11 else 7
        for y in (-2, -1, 0, 2): pix[cy + y][cx] = 1


def main():
    out = sys.argv[1]
    stack = int(sys.argv[2]) if len(sys.argv) > 2 else 65536
    kind = sys.argv[3] if len(sys.argv) > 3 else ""
    if kind == "drawer":
        data = from_template("Drawer", 2, None, deco_drawer)
    elif kind == "text":
        data = from_template("Plate", 4, "SYS:Utilities/MultiView", deco_text)
    elif kind == "guide":
        data = from_template("Guide", 4, "SYS:Utilities/MultiView", deco_guide)
    else:
        badge = kind == "new"
        data = from_template("Plate", 3, None, lambda pix, sel: deco_program(pix, sel, badge), stack)
    with open(out, "wb") as f:
        f.write(data)


if __name__ == "__main__":
    main()
