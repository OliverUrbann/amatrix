from PIL import Image
# MagicWB palette (8 colours) + the darker blue we already use
PAL = {'.': None, 'k': (0,0,0), 'w': (255,255,255), 'b': (59,103,162), 'B': (36,64,112),
       'd': (110,110,110), 'l': (175,175,175), 'g': (149,149,149), 'e': (170,144,124),
       'f': (255,169,151)}
# lower-case letters with ':' suffix-free; dithering via pairs:
#  'x' = checkerboard of w/b (light blue), 'y' = b/B (mid shadow), 'z' = f/e (skin shade), 'q' = w/l (plus shading)
DITHER = {'x': ('w', 'b'), 'y': ('b', 'B'), 'z': ('f', 'e'), 'q': ('w', 'l'), 'u': ('l', 'g')}
UP = [
"................",
".......kk.......",
"......kwxk......",
".....kwxbyk.....",
"....kwxbbbyk....",
"...kwxbbbbbyk...",
"..kwxbbbbbbbyk..",
".kkkkkwxbykkkkkd",
"..ddd.kwxbyk.ddd",
".....kwxbyk.....",
".....kwxbykd....",
".....kwxbykd....",
".....kwxbykd....",
".....kwxbykd....",
".....kyyyykd....",
"......kkkkdd....",
]
PERSON = [
"....kkkkk.......",
"...kwwfffk......",
"..kwffffffk.....",
"..kwfffffzk.....",
"..kfffffzzkd....",
"..kffffzzekd....",
"...kfzzeekd.....",
"....kkkkkd......",
"..kkkkkkkkk.....",
".kwxxbbbbbyk....",
"kwxbbbbbbbbyk...",
"kxbbbbbbbbbykd..",
"kbbbbbbbbbbykd..",
"kbbbbbbbbbbykd..",
"kyyyyyyyyyyykd..",
"kkkkkkkkkkkkkd..",
]
PLUS = [
"..kkk..",
"..kwk..",
"kkkwkkk",
"kwwwwqk",
"kkkqkkk",
"..kqk..",
"..kkk..",
]
def fix(rows):
    return rows
UP[8] = "..dddkwxbyk.ddd."
person = [list(r) for r in PERSON]
for y, row in enumerate(PLUS):
    for x, ch in enumerate(row):
        if ch != '.': person[7 + y][9 + x] = ch
person = [''.join(r) for r in person]
def resolve(art):
    out = []
    for y, r in enumerate(art):
        row = ''
        for x, ch in enumerate(r):
            if ch in DITHER: ch = DITHER[ch][(x + y) & 1]
            row += ch
        out.append(row)
    return out
up, inv = resolve(UP), resolve(person)
def img(art):
    im = Image.new("RGB", (16, 16), (149,149,149))
    for y, r in enumerate(art):
        assert len(r) == 16, (y, r)
        for x, ch in enumerate(r):
            if PAL[ch]: im.putpixel((x, y), PAL[ch])
    return im
both = Image.new("RGB", (16 * 12 * 2 + 24, 16 * 12 + 48), (149,149,149))
both.paste(img(up).resize((192, 192), Image.NEAREST), (0, 0))
both.paste(img(inv).resize((192, 192), Image.NEAREST), (216, 0))
small = Image.new("RGB", (40, 16), (149,149,149)); small.paste(img(up), (0, 0)); small.paste(img(inv), (24, 0))
both.paste(small.resize((80, 32), Image.NEAREST), (0, 200))
both.save("/tmp/claude-0/icons/both2.png")
open("/tmp/claude-0/icons/art2.txt", "w").write("UP\n" + "\n".join(up) + "\nINVITE\n" + "\n".join(inv) + "\n")
