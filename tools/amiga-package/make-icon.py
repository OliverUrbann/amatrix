#!/usr/bin/env python3
"""Writes the Workbench tool icon for Amatrix: 8 colours in the MagicWB
palette (3 bitplanes), with a second image for the selected state.

    make-icon.py OUT.info [STACK] [new]

"new": the AppIcon for unread messages (a pink badge on the bubble).
The picture is drawn in icon_art.py.
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


def main():
    out = sys.argv[1]
    stack = int(sys.argv[2]) if len(sys.argv) > 2 else 65536
    badge = len(sys.argv) > 3 and sys.argv[3] == "new"
    w, h, img1 = image(icon_art.draw(badge=badge))
    _, _, img2 = image(icon_art.selected(badge=badge))
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
        f.write(disk + img1 + img2)


if __name__ == "__main__":
    main()
