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
