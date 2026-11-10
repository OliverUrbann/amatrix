#!/usr/bin/env python3
"""Writes the Workbench tool icon for Amatrix: 8 colours in the MagicWB
palette (3 bitplanes), with a second image for the selected state.

    make-icon.py OUT.info [STACK] [new]

"new": the AppIcon for unread messages (a pink badge on the bubble).
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


def planes(art):
    w, h = len(art[0]), len(art)
    words = (w + 15) // 16
    out = b""
    for bit in range(DEPTH):
        for row in art:
            data = bytearray(words * 2)
            for x, pen in enumerate(row):
                if pen >> bit & 1:
                    data[x // 8] |= 0x80 >> (x % 8)
            out += bytes(data)
    return w, h, out


def image(art):
    w, h, data = planes(art)
    head = struct.pack(">hhhhhIBBI", 0, 0, w, h, DEPTH, 1, (1 << DEPTH) - 1, 0, 0)
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


def main():
    out = sys.argv[1]
    stack = int(sys.argv[2]) if len(sys.argv) > 2 else 65536
    badge = len(sys.argv) > 3 and sys.argv[3] == "new"
    art1, art2 = icon_art.draw(badge=badge), icon_art.selected(badge=badge)
    w, h, img1 = image(art1)
    _, _, img2 = image(art2)
    # struct Gadget (44 bytes)
    gadget = struct.pack(">IhhhhHHHIIIIIHI",
                         0, 0, 0, w, h + 1,
                         0x0006,          # GADGIMAGE | GADGHIMAGE: second image when selected
                         0x0003,          # RELVERIFY | GADGIMMEDIATE
                         0x0001,          # BOOLGADGET
                         1, 1, 0, 0, 0,   # GadgetRender, SelectRender (non-zero = images follow)
                         0,
                         1)               # UserData: revision 1 (OS 2 icon)
    assert len(gadget) == 44
    disk = struct.pack(">HH", 0xE310, 1) + gadget + struct.pack(
        ">BBIIiiIII", 3, 0,               # WBTOOL
        0, 0,                             # DefaultTool, ToolTypes
        -0x80000000, -0x80000000,         # NO_ICON_POSITION
        0, 0, stack)
    assert len(disk) == 78
    with open(out, "wb") as f:
        f.write(disk + img1 + img2 + colour_icon(art1, art2))


if __name__ == "__main__":
    main()
