#!/usr/bin/env python3
"""Writes the Workbench tool icon for Amatrix: 8 colours in the MagicWB
palette (3 bitplanes), with a second image for the selected state.

    make-icon.py OUT.info [STACK] [new]

"new": the AppIcon for unread messages (a pink badge on the bubble);
"drawer": a drawer icon; "text": a document icon for ReadMe/guide files -
both in the classic 4-colour style of the Workbench default icons.
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


def main():
    out = sys.argv[1]
    stack = int(sys.argv[2]) if len(sys.argv) > 2 else 65536
    kind = sys.argv[3] if len(sys.argv) > 3 else ""
    if kind == "drawer":
        data = build(icon_art.std_drawer(False), icon_art.std_drawer(True), 2, 4096, drawer=True,
                     classic=True)
    elif kind == "text":
        data = build(icon_art.std_document(), None, 4, 4096, tool="SYS:Utilities/MultiView",
                     classic=True)
    else:
        badge = kind == "new"
        data = build(icon_art.draw(badge=badge), icon_art.selected(badge=badge), 3, stack)
    with open(out, "wb") as f:
        f.write(data)


if __name__ == "__main__":
    main()
