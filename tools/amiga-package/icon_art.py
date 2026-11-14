"""The Amatrix program icon in the MagicWB style: two speech bubbles, "[m]".

draw() returns rows of pen numbers (MagicWB palette, 8 colours); badge=True is
the AppIcon for unread messages.  preview.py-like use: see make-icon.py.
"""
# MagicWB palette: 0 grey, 1 black, 2 white, 3 blue, 4 dark grey, 5 light grey, 6 beige, 7 pink
PAL=[(149,149,149),(0,0,0),(255,255,255),(59,103,162),(123,123,123),(175,175,175),(170,144,124),(255,169,151)]
W,H=46,40
def canvas(): return [[0]*W for _ in range(H)]
def put(c,x,y,v):
    if 0<=x<W and 0<=y<H: c[y][x]=v
def bubble(c,x0,y0,x1,y1,fill,hi,lo,r=3):
    def inside(x,y):
        for cx,cy in ((x0+r,y0+r),(x1-r,y0+r),(x0+r,y1-r),(x1-r,y1-r)):
            pass
        dx=max(x0+r-x,0,x-(x1-r)); dy=max(y0+r-y,0,y-(y1-r))
        return x0<=x<=x1 and y0<=y<=y1 and dx*dx+dy*dy<=r*r+1
    for y in range(y0,y1+1):
        for x in range(x0,x1+1):
            if not inside(x,y): continue
            edge=not all(inside(x+a,y+b) for a,b in((1,0),(-1,0),(0,1),(0,-1)))
            if edge: put(c,x,y,1); continue
            tl=not (inside(x-2,y) and inside(x,y-2) and inside(x-1,y-1)) or not inside(x-1,y-1) 
            near_tl = not inside(x-2,y) or not inside(x,y-2)
            near_br = not inside(x+2,y) or not inside(x,y+2)
            put(c,x,y, hi if near_tl and not near_br else lo if near_br and not near_tl else fill)
def shadow(c,x0,y0,x1,y1,r=3):
    for y in range(y0,y1+1):
        for x in range(x0,x1+1):
            dx=max(x0+r-x,0,x-(x1-r)); dy=max(y0+r-y,0,y-(y1-r))
            if dx*dx+dy*dy<=r*r+1 and c[y][x]==0: put(c,x,y,4)
def tail(c,x,y,fill,lo,left=True):
    # triangle hanging from the bubble's bottom edge at (x,y)
    for i in range(7):
        w=7-i
        for k in range(w):
            xx=x+k if left else x-k
            put(c,xx,y+i,1 if k in(0,w-1) else fill)
    for k in range(1,6): put(c,x+k if left else x-k,y,fill)   # open the bubble's edge
GL={'[':["##","#.","#.","#.","#.","#.","#.","#.","##"],
    ']':["##",".#",".#",".#",".#",".#",".#",".#","##"],
    'm':[".........",".........","#.##.##..","##..#..#.","#...#..#.","#...#..#.","#...#..#.","#...#..#.","........."]}
def glyphs(c,x,y,col,brk):
    for ch,cc in (('[',brk),('m',col),(']',brk)):
        g=GL[ch]
        for j,row in enumerate(g):
            for i,p in enumerate(row):
                if p=='#': put(c,x+i,y+j,cc)
        x+=len(g[0])+ (2 if ch=='[' else 0)
def draw(front_fill=2,front_hi=2,front_lo=5,gcol=3,brk=1,badge=False):
    c=canvas()
    # back bubble (beige), tail to the right
    shadow(c,16,2,45,21)
    bubble(c,14,0,43,19,6,7,4)
    tail(c,39,19,6,4,left=False)
    # front bubble
    shadow(c,2,13,35,33)
    bubble(c,0,11,33,31,front_fill,front_hi,front_lo)
    tail(c,6,31,front_fill,front_lo,left=True)
    glyphs(c,8,17,gcol,brk)
    if badge:
        cx,cy=33,11
        for y in range(-5,6):
            for x in range(-5,6):
                d=x*x+y*y
                if d<=27: put(c,cx+x,cy+y,1 if d>17 else (2 if (x<-1 and y<-1 and d>5) else 7))
        for y in range(-3,2): put(c,cx,cy+y,1)
        put(c,cx,cy+3,1)
    return c


def selected(badge=False):
    return draw(front_fill=3, front_hi=5, front_lo=1, gcol=2, brk=2, badge=badge)


def _rect(c, x0, y0, x1, y1, v):
    for y in range(y0, y1 + 1):
        for x in range(x0, x1 + 1):
            put(c, x, y, v)


def drawer(selected=False):
    """a filing drawer: blue body, white front plate with a handle"""
    w, h = 44, 30
    c = [[0] * w for _ in range(h)]
    _rect(c, 3, 5, w - 2, h - 2, 4)                   # shadow
    body = 5 if selected else 3
    top = 1 if selected else 2
    _rect(c, 1, 3, w - 4, h - 4, 1)                   # outline
    _rect(c, 2, 4, w - 5, h - 5, body)
    _rect(c, 2, 4, w - 5, 4, top)                     # light top edge / dark opening
    _rect(c, 2, 4, 2, h - 5, 2)                       # left highlight
    _rect(c, 7, 10, w - 10, h - 9, 1)                 # label plate
    _rect(c, 8, 11, w - 11, h - 10, 2)
    _rect(c, 12, 15, w - 15, 16, 1)                   # handle
    return c


def document(selected=False):
    """a text page with a folded corner and lines of text"""
    w, h = 32, 38
    c = [[0] * w for _ in range(h)]
    _rect(c, 3, 3, w - 2, h - 2, 4)                   # shadow
    page = 5 if selected else 2
    _rect(c, 1, 1, w - 4, h - 4, 1)
    _rect(c, 2, 2, w - 5, h - 5, page)
    for i in range(8):                                # folded corner
        _rect(c, w - 12 + i, 1 + i, w - 4, 1 + i, 0 if i == 0 else 0)
    for i in range(9):
        put(c, w - 13 + i, 1 + i, 1)
    _rect(c, w - 12, 2, w - 5, 9, 5 if not selected else 2)
    for i in range(9):
        put(c, w - 13 + i, 1 + i, 1)
    for x in range(w - 13, w - 3):
        put(c, x, 10 if False else 9, 1) if False else None
    _rect(c, 5, 6, 14, 8, 3)                          # blue heading bar
    for k, y in enumerate(range(13, h - 7, 4)):       # lines of text
        _rect(c, 5, y, w - 9 - (6 if k % 3 == 2 else 0), y + 1, 4)
    return c


# ---- the small logo for the MagicWB template icons (templates/*.info) ----
_SG = {'[': ["##", "#.", "#.", "#.", "#.", "#.", "##"],
       ']': ["##", ".#", ".#", ".#", ".#", ".#", "##"],
       'm': [".....", ".....", "####.", "#.#.#", "#.#.#", "#.#.#", "....."]}


def mini_logo(both=True, pressed=False):
    """the two speech bubbles with "[m]", small (26x20); both=False: only
    the white bubble without its tail (19x11).  Pen 0 = transparent, -1 =
    grey of the background kept free around nothing (unused)."""
    c = canvas()
    oy = 6 if both else 0
    if both:
        bubble(c, 8, 0, 25, 10, 6, 7, 4, r=2)
        for i in range(4):                             # its tail, to the right
            for k in range(4 - i):
                put(c, 23 - k, 10 + i, 1 if k in (0, 3 - i) else 6)
        for k in range(1, 3): put(c, 23 - k, 10, 6)
    bubble(c, 0, oy, 18, oy + 10, 2, 2, 5, r=2)
    if both:
        for i in range(4):                             # tail of the front bubble
            for k in range(4 - i):
                put(c, 3 + k, oy + 10 + i, 1 if k in (0, 3 - i) else 2)
        for k in range(1, 3): put(c, 3 + k, oy + 10, 2)
    x = 4
    for ch, col in (('[', 1), ('m', 3), (']', 1)):
        for j, row in enumerate(_SG[ch]):
            for i, p in enumerate(row):
                if p == '#': put(c, x + i, oy + 2 + j, col)
        x += len(_SG[ch][0]) + 1
    w, h = (26, 20) if both else (19, 11)
    return [row[:w] for row in c[:h]]
