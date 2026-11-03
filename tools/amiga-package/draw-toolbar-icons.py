from PIL import Image
PAL = {'.': None, 'k': (0,0,0), 'w': (255,255,255), 'b': (59,103,162), 'B': (36,64,112),
       'd': (110,110,110), 'l': (175,175,175), 'e': (210,130,110), 'f': (255,169,151)}
UP = [
"................",
".......kk.......",
"......kwbk......",
".....kwbbBk.....",
"....kwbbbbBk....",
"...kwbbbbbbBk...",
"..kwbbbbbbbbBk..",
".kkkkkwbbBkkkkkd",
"..dddkwbbBkddddd",
".....kwbbBkd....",
".....kwbbBkd....",
".....kwbbBkd....",
".....kwbbBkd....",
".....kwbbBkd....",
".....kBBBBkd....",
"......kkkkdd....",
]
PERSON = [
"....kkkkk.......",
"...kwwfffk......",
"..kwffffffk.....",
"..kwfffffek.....",
"..kffffffekd....",
"..kfffffeekd....",
"...kffeeekd.....",
"....kkkkkd......",
"..kkkkkkkkk.....",
".kwbbbbbbbBk....",
"kwbbbbbbbbbBk...",
"kbbbbbbbbbbBkd..",
"kbbbbbbbbbbBkd..",
"kbbbbbbbbbbBkd..",
"kBBBBBBBBBBBkd..",
"kkkkkkkkkkkkkd..",
]
PLUS = [  # drawn over the person at x=9, y=9
"..kkk..",
"..kwk..",
"kkkwkkk",
"kwwwwwk",
"kkkwkkk",
"..kwk..",
"..kkk..",
]
person = [list(r) for r in PERSON]
for y, row in enumerate(PLUS):
    for x, ch in enumerate(row):
        if ch != '.': person[9 + y - 2][9 + x] = ch      # rows 7..13
person = [''.join(r) for r in person]
def img(art):
    im = Image.new("RGB", (16, 16), (149,149,149))
    for y, r in enumerate(art):
        assert len(r) == 16, (y, r)
        for x, ch in enumerate(r):
            if PAL[ch]: im.putpixel((x, y), PAL[ch])
    return im
both = Image.new("RGB", (16 * 12 * 2 + 24, 16 * 12 + 48), (149,149,149))
both.paste(img(UP).resize((192, 192), Image.NEAREST), (0, 0))
both.paste(img(person).resize((192, 192), Image.NEAREST), (216, 0))
small = Image.new("RGB", (16 * 2 + 8, 16), (149,149,149)); small.paste(img(UP), (0, 0)); small.paste(img(person), (24, 0))
both.paste(small.resize((80, 32), Image.NEAREST), (0, 200))
both.save("/tmp/claude-0/icons/both.png")
open("/tmp/claude-0/icons/art.txt", "w").write("UP\n" + "\n".join(UP) + "\nINVITE\n" + "\n".join(person) + "\n")
