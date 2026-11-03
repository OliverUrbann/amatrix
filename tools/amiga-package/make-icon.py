#!/usr/bin/env python3
"""Writes a classic (OS 2/3, 4 colour) Workbench tool icon for Amatrix.

    make-icon.py OUT.info [STACK]

The picture is a speech bubble with an "m" (Matrix), drawn below as text:
'.' = colour 0 (background), '#' = 1 (black), 'o' = 2 (white), '+' = 3 (blue).
"""
import struct, sys

ART = [
    "..........................................",
    "...######################################.",
    "..#oooooooooooooooooooooooooooooooooooooo#",
    "..#o++++++++++++++++++++++++++++++++++++o#",
    "..#o+oooooooooooooooooooooooooooooooooo+o#",
    "..#o+o##.......................##ooooo+o#",
    "..#o+o#.........................#ooooo+o#",
    "..#o+o#...####...####..####.....#ooooo+o#",
    "..#o+o#...#...#.#....##....#....#ooooo+o#",
    "..#o+o#...#...#.#....##....#....#ooooo+o#",
    "..#o+o#...#...#.#....##....#....#ooooo+o#",
    "..#o+o#...#...#.#....##....#....#ooooo+o#",
    "..#o+o#...#...#.#....##....#....#ooooo+o#",
    "..#o+o#.........................#ooooo+o#",
    "..#o+o##.......................##ooooo+o#",
    "..#o+oooooooooooooooooooooooooooooooooo+o#",
    "..#o++++++++++++++++++++++++++++++++++++o#",
    "..#oooooooooooooooo+++oooooooooooooooooo#",
    "...################+++##################.",
    "..................++#....................",
    ".................++#.....................",
    "................+#.......................",
]
COL = {".": 0, "#": 1, "o": 2, "+": 3}


def planes(art):
    w = max(len(r) for r in art)
    art = [r.ljust(w, ".") for r in art]
    h = len(art)
    words = (w + 15) // 16
    out = b""
    for bit in (0, 1):
        for row in art:
            data = bytearray(words * 2)
            for x, ch in enumerate(row):
                if COL[ch] >> bit & 1:
                    data[x // 8] |= 0x80 >> (x % 8)
            out += bytes(data)
    return w, h, out


def main():
    out = sys.argv[1]
    stack = int(sys.argv[2]) if len(sys.argv) > 2 else 65536
    w, h, data = planes(ART)
    # struct Gadget (44 bytes)
    gadget = struct.pack(">IhhhhHHHIIIIIHI",
                         0, 0, 0, w, h + 1,
                         0x0004,          # GADGIMAGE, GADGHCOMP
                         0x0003,          # RELVERIFY | GADGIMMEDIATE
                         0x0001,          # BOOLGADGET
                         1, 0, 0, 0, 0,   # GadgetRender (non-zero = image follows), ...
                         0,
                         1)               # UserData: revision 1 (OS 2 icon)
    assert len(gadget) == 44
    disk = struct.pack(">HH", 0xE310, 1) + gadget + struct.pack(
        ">BBIIiiIII", 3, 0,               # WBTOOL
        0, 0,                             # DefaultTool, ToolTypes
        -0x80000000, -0x80000000,         # NO_ICON_POSITION
        0, 0, stack)
    assert len(disk) == 78
    image = struct.pack(">hhhhhIBBI", 0, 0, w, h, 2, 1, 3, 0, 0)
    assert len(image) == 20
    with open(out, "wb") as f:
        f.write(disk + image + data)


if __name__ == "__main__":
    main()
