#!/usr/bin/env python3
"""M5Burner cover for Meshtastic ADV — 8-bit pixel art, 240x160 logical grid, x5 nearest upscale."""
import math
import random
from PIL import Image

W, H = 240, 160
SCALE = 5

# --- palette ---------------------------------------------------------------
SKY = [(9, 12, 38), (13, 17, 52), (18, 23, 66), (24, 30, 82)]
STAR = (222, 228, 255)
STAR_DIM = (120, 130, 180)
TEAL = (44, 216, 202)
TEAL_DIM = (26, 130, 126)
TEAL_FAINT = (18, 74, 82)
ORANGE = (255, 138, 61)
ORANGE_DIM = (170, 84, 38)
GREEN = (57, 255, 110)
AMBER = (255, 196, 80)
WHITE = (238, 240, 248)
GREY = (200, 204, 214)
GREY_SH = (140, 146, 162)
GREY_DK = (74, 78, 92)
KEY = (32, 34, 42)
BEZEL = (24, 26, 36)
SCREEN_BG = (5, 8, 14)
GROUND = (11, 14, 34)
GROUND_FAR = (16, 20, 48)
FUR = (232, 216, 176)
FUR_DK = (106, 74, 47)

px = {}

def put(x, y, c):
    if 0 <= x < W and 0 <= y < H:
        px[(x, y)] = c

def rect(x0, y0, x1, y1, c):
    for y in range(y0, y1 + 1):
        for x in range(x0, x1 + 1):
            put(x, y, c)

def line(x0, y0, x1, y1, c, skip=0):
    dx, dy = abs(x1 - x0), -abs(y1 - y0)
    sx, sy = (1 if x0 < x1 else -1), (1 if y0 < y1 else -1)
    err, i = dx + dy, 0
    while True:
        if skip == 0 or (i % (skip + 1)) != skip:
            put(x0, y0, c)
        i += 1
        if x0 == x1 and y0 == y1:
            break
        e2 = 2 * err
        if e2 >= dy:
            err += dy; x0 += sx
        if e2 <= dx:
            err += dx; y0 += sy

def arc_up(cx, cy, r, c, dash=3):
    pts = set()
    for a in range(181):
        x = cx + round(r * math.cos(math.radians(a)))
        y = cy - round(r * math.sin(math.radians(a)))
        pts.add((x, y))
    for i, (x, y) in enumerate(sorted(pts)):
        if i % dash != dash - 1:
            put(x, y, c)

# --- 5x7 font ----------------------------------------------------------------
F = {
 'A': ["01110","10001","10001","11111","10001","10001","10001"],
 'B': ["11110","10001","11110","10001","10001","10001","11110"],
 'C': ["01110","10001","10000","10000","10000","10001","01110"],
 'D': ["11110","10001","10001","10001","10001","10001","11110"],
 'E': ["11111","10000","11110","10000","10000","10000","11111"],
 'F': ["11111","10000","11110","10000","10000","10000","10000"],
 'G': ["01110","10001","10000","10111","10001","10001","01111"],
 'H': ["10001","10001","11111","10001","10001","10001","10001"],
 'I': ["11111","00100","00100","00100","00100","00100","11111"],
 'K': ["10001","10010","10100","11000","10100","10010","10001"],
 'L': ["10000","10000","10000","10000","10000","10000","11111"],
 'M': ["10001","11011","10101","10101","10001","10001","10001"],
 'N': ["10001","11001","10101","10011","10001","10001","10001"],
 'O': ["01110","10001","10001","10001","10001","10001","01110"],
 'P': ["11110","10001","10001","11110","10000","10000","10000"],
 'R': ["11110","10001","10001","11110","10100","10010","10001"],
 'S': ["01111","10000","10000","01110","00001","00001","11110"],
 'T': ["11111","00100","00100","00100","00100","00100","00100"],
 'U': ["10001","10001","10001","10001","10001","10001","01110"],
 'V': ["10001","10001","10001","10001","10001","01010","00100"],
 'Y': ["10001","10001","01010","00100","00100","00100","00100"],
 '-': ["00000","00000","00000","11111","00000","00000","00000"],
 ' ': ["00000","00000","00000","00000","00000","00000","00000"],
}

def text(s, x, y, c, scale=1, shadow=None):
    cx = x
    for ch in s:
        g = F[ch]
        for r, row in enumerate(g):
            for k, bit in enumerate(row):
                if bit == '1':
                    for dy in range(scale):
                        for dx in range(scale):
                            if shadow:
                                put(cx + k * scale + dx + scale, y + r * scale + dy + scale, shadow)
    # second pass so shadow never overdraws glyphs
        cx += 6 * scale
    cx = x
    for ch in s:
        g = F[ch]
        for r, row in enumerate(g):
            for k, bit in enumerate(row):
                if bit == '1':
                    for dy in range(scale):
                        for dx in range(scale):
                            put(cx + k * scale + dx, y + r * scale + dy, c)
        cx += 6 * scale

def text_w(s, scale=1):
    return len(s) * 6 * scale - scale

# --- sky ---------------------------------------------------------------------
for y in range(H):
    band = min(3, y // 34)
    for x in range(W):
        put(x, y, SKY[band])
# dither seams
random.seed(7)
for b in range(1, 4):
    yb = b * 34
    for x in range(W):
        if random.random() < 0.5:
            put(x, yb - 1, SKY[b])
        if random.random() < 0.3:
            put(x, yb, SKY[b - 1])

# stars (keep clear of the title block)
random.seed(42)
for _ in range(90):
    x, y = random.randrange(4, W - 4), random.randrange(4, 100)
    if 52 <= x <= 188 and 8 <= y <= 66:
        continue
    put(x, y, STAR if random.random() < 0.3 else STAR_DIM)

# --- mesh constellation --------------------------------------------------------
nodes = [(22, 22), (46, 52), (16, 78), (206, 16), (228, 46), (192, 62), (222, 88)]
links = [(0, 1), (1, 2), (3, 4), (4, 5), (5, 6)]
for a, b in links:
    (x0, y0), (x1, y1) = nodes[a], nodes[b]
    line(x0, y0, x1, y1, TEAL_FAINT, skip=2)
for i, (x, y) in enumerate(nodes):
    c = ORANGE if i == 4 else TEAL
    put(x, y, WHITE)
    for dx, dy in ((1, 0), (-1, 0), (0, 1), (0, -1)):
        put(x + dx, y + dy, c)
    for dx, dy in ((2, 0), (-2, 0), (0, 2), (0, -2)):
        put(x + dx, y + dy, TEAL_DIM if i != 4 else ORANGE_DIM)

# --- title ---------------------------------------------------------------------
# shadows near-bg dark: a bright shadow floods the glyph counters (A's bowl,
# D's gap) and reads as corruption at block scale
DROP = (5, 7, 22)
t1 = "MESHTASTIC"
text(t1, (W - text_w(t1, 2)) // 2, 12, TEAL, 2, shadow=DROP)
t2 = "ADV"
text(t2, (W - text_w(t2, 4)) // 2, 30, ORANGE, 4, shadow=DROP)
t3 = "KEYBOARD-FIRST MESH CLIENT"
text(t3, (W - text_w(t3, 1)) // 2, 64, WHITE, 1)

# --- ground ----------------------------------------------------------------------
HOR = 118
for x in range(W):
    h1 = int(10 * math.sin((x + 30) / 38.0)) - 2
    for y in range(HOR - max(0, h1), H):
        put(x, y, GROUND_FAR)
for x in range(W):
    h2 = int(7 * math.sin((x + 130) / 30.0)) - 3
    for y in range(HOR + 4 - max(0, h2), H):
        put(x, y, GROUND)

# pines on the far ridge
random.seed(5)
for bx in (10, 150, 168, 232):
    base = HOR - max(0, int(10 * math.sin((bx + 30) / 38.0)) - 2)
    for i in range(4):
        rect(bx - i // 2, base - 4 + i, bx + i // 2, base - 4 + i, (8, 11, 30))

# --- radio mast on the right hill -------------------------------------------------
MX, MTOP, MBASE = 196, 82, HOR + 2
line(MX, MTOP, MX - 3, MBASE, GREY_DK)
line(MX, MTOP, MX + 3, MBASE, GREY_DK)
for i, yy in enumerate(range(MTOP + 4, MBASE, 5)):
    w = 1 + (yy - MTOP) // 8
    line(MX - w, yy, MX + w, yy, GREY_DK)
put(MX, MTOP - 1, ORANGE)
put(MX, MTOP - 2, ORANGE)
arc_up(MX, MTOP - 2, 4, ORANGE_DIM, dash=2)
arc_up(MX, MTOP - 2, 7, (100, 52, 26), dash=2)

# --- Cardputer ---------------------------------------------------------------------
BX0, BY0, BX1, BY1 = 30, 96, 118, 150
rect(BX0, BY0, BX1, BY1, GREY)
for x in range(BX0, BX1 + 1):  # top/bottom outline
    put(x, BY0 - 1, GREY_DK); put(x, BY1 + 1, GREY_DK)
for y in range(BY0, BY1 + 1):
    put(BX0 - 1, y, GREY_DK); put(BX1 + 1, y, GREY_DK)
for c in ((BX0 - 1, BY0 - 1), (BX1 + 1, BY0 - 1), (BX0 - 1, BY1 + 1), (BX1 + 1, BY1 + 1)):
    put(*c, SKY[3])  # knock out corners -> rounded
# side shading
for y in range(BY0, BY1 + 1):
    put(BX1, y, GREY_SH)
    put(BX1 - 1, y, GREY_SH) if y > BY1 - 2 else None

# screen
SX0, SY0, SX1, SY1 = 36, 100, 90, 121
rect(SX0 - 1, SY0 - 1, SX1 + 1, SY1 + 1, BEZEL)
rect(SX0, SY0, SX1, SY1, SCREEN_BG)
rect(SX0, SY0, SX1, SY0 + 2, (16, 60, 58))          # header bar
line(SX0 + 1, SY0 + 1, SX0 + 14, SY0 + 1, TEAL)     # channel name
put(SX1 - 2, SY0 + 1, GREEN)                        # signal dot
chat = [(GREEN, 26), (WHITE, 34), (AMBER, 18), (GREEN, 30), (WHITE, 22)]
yy = SY0 + 5
for c, wl in chat:
    put(SX0 + 2, yy, TEAL_DIM)                      # "> " prompt
    line(SX0 + 4, yy, SX0 + 4 + wl, yy, c)
    if c is GREEN:
        put(SX1 - 2, yy, TEAL)                      # ack check
    yy += 3
line(SX0 + 2, SY1 - 1, SX0 + 10, SY1 - 1, GREY_DK)  # input line
put(SX0 + 12, SY1 - 1, WHITE)                       # cursor

# M5 stamp sticker
rect(94, 100, 113, 121, (232, 226, 214))
rect(95, 101, 112, 106, ORANGE)
rect(95, 108, 103, 113, (60, 150, 220))
rect(105, 108, 112, 113, (40, 42, 50))
rect(95, 115, 112, 120, (120, 190, 90))

# keyboard: 4 rows x 13 keys
ky = 126
for r in range(4):
    kx = BX0 + 4 + (r % 2) * 2
    for k in range(13):
        if kx + 4 > BX1 - 3:
            break
        rect(kx, ky, kx + 4, ky + 3, KEY)
        put(kx + 1, ky + 1, (58, 60, 70))
        kx += 6
    ky += 6

# antenna + LoRa waves
AX, AY = 114, 88
rect(AX - 1, AY, AX, BY0 - 1, GREY_DK)
put(AX, AY - 1, TEAL)
put(AX - 1, AY - 1, TEAL)
for r, c in ((5, TEAL), (9, TEAL_DIM), (13, TEAL_FAINT)):
    arc_up(AX, AY - 2, r, c, dash=3)

# --- ferret -------------------------------------------------------------------------
fx, fy = 136, 146  # tail joins at the left, nose at the right
line(fx, fy, fx + 13, fy, FUR)          # body top
line(fx, fy + 1, fx + 13, fy + 1, FUR)
line(fx + 1, fy + 2, fx + 12, fy + 2, FUR_DK)     # belly/legs shadow
put(fx + 2, fy + 3, FUR_DK); put(fx + 6, fy + 3, FUR_DK); put(fx + 10, fy + 3, FUR_DK)
rect(fx + 12, fy - 3, fx + 15, fy - 1, FUR)       # head up
put(fx + 12, fy - 4, FUR_DK); put(fx + 15, fy - 4, FUR_DK)  # ears
put(fx + 14, fy - 2, FUR_DK)                      # mask
put(fx + 16, fy - 2, FUR_DK)                      # nose
line(fx - 1, fy, fx - 4, fy - 3, FUR_DK)          # tail
put(fx - 5, fy - 4, FUR_DK)

# --- frame ----------------------------------------------------------------------------
for x in range(W):
    put(x, 0, (4, 5, 16)); put(x, H - 1, (4, 5, 16))
    put(x, 2, TEAL_FAINT); put(x, H - 3, TEAL_FAINT)
for y in range(H):
    put(0, y, (4, 5, 16)); put(W - 1, y, (4, 5, 16))
    put(2, y, TEAL_FAINT); put(W - 3, y, TEAL_FAINT)
for cx, cy in ((2, 2), (W - 3, 2), (2, H - 3), (W - 3, H - 3)):
    for dx in range(-1, 2):
        for dy in range(-1, 2):
            put(cx + dx, cy + dy, TEAL)

# --- render -----------------------------------------------------------------------------
im = Image.new("RGB", (W, H))
for (x, y), c in px.items():
    im.putpixel((x, y), c)
im = im.resize((W * SCALE, H * SCALE), Image.NEAREST)
out = "/private/tmp/claude-501/-Users-randoom-claude/61ef78cd-ea4d-46cd-9e3f-224f805231ea/scratchpad/cover/m5burner-cover.png"
im.save(out)
print("saved", out, im.size)
