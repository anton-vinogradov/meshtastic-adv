#!/usr/bin/env python3
"""M5Burner cover: the real UI, face-on.

Composites screenshots captured from the device (docs/img/shots/, produced by
the ADVUI_SCREENSHOT build's demo dump) at 4x nearest-neighbour, so the listing
shows actual pixels rather than a drawing. Writes docs/img/m5burner-cover.png.
"""
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


from PIL import Image, ImageDraw
import random

import os
ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
S = os.path.join(ROOT, "docs", "img")
W, H = 1000, 1000  # square: the store card centre-crops, so leave it nothing to cut
BG = (10, 13, 34)
BG2 = (16, 20, 52)
TEAL = (44, 216, 202)
TEAL_DIM = (26, 120, 116)
ORANGE = (255, 138, 61)
DROP = (5, 7, 22)
WHITE = (238, 240, 248)
STAR = (200, 208, 245)
STAR_DIM = (95, 105, 155)

im = Image.new("RGB", (W, H), BG)
d = ImageDraw.Draw(im)

for y in range(H):
    t = y / H
    c = tuple(int(a + (b - a) * t) for a, b in zip(BG, BG2))
    d.line([(0, y), (W, y)], fill=c)
random.seed(9)
for _ in range(80):
    x, y = random.randrange(8, W - 8), random.randrange(8, H - 8)
    im.putpixel((x, y), STAR if random.random() < 0.25 else STAR_DIM)

def px_text(s, x, y, color, scale, shadow=None):
    cx = x
    if shadow:
        for ch in s:
            g = F.get(ch, F[' '])
            for r, row in enumerate(g):
                for k, bit in enumerate(row):
                    if bit == '1':
                        d.rectangle([cx + k*scale + scale, y + r*scale + scale,
                                     cx + (k+1)*scale + scale - 1, y + (r+1)*scale + scale - 1], fill=shadow)
            cx += 6 * scale
    cx = x
    for ch in s:
        g = F.get(ch, F[' '])
        for r, row in enumerate(g):
            for k, bit in enumerate(row):
                if bit == '1':
                    d.rectangle([cx + k*scale, y + r*scale,
                                 cx + (k+1)*scale - 1, y + (r+1)*scale - 1], fill=color)
        cx += 6 * scale

def text_w(s, scale):
    return len(s) * 6 * scale - scale

def place(shot, x, y, scale, border):
    img = Image.open(os.path.join(S, "shots", shot + ".png"))
    img = img.resize((int(img.width * scale), int(img.height * scale)), Image.NEAREST)
    d.rectangle([x + 12, y + 12, x + img.width + 12, y + img.height + 12], fill=DROP)
    d.rectangle([x - 4, y - 4, x + img.width + 3, y + img.height + 3], fill=border)
    im.paste(img, (x, y))

# Title centred: the store crops the card towards the middle, so nothing
# load-bearing may sit near an edge.
t1, t2 = "MESHTASTIC", "ADV"
tw = text_w(t1, 4) + 28 + text_w(t2, 7)
tx = (W - tw) // 2
px_text(t1, tx, 70, TEAL, 4, shadow=DROP)
px_text(t2, tx + text_w(t1, 4) + 28, 56, ORANGE, 7, shadow=DROP)
sub = "KEYBOARD-FIRST MESH CLIENT"
px_text(sub, (W - text_w(sub, 2)) // 2, 132, WHITE, 2)

# Chat is the hero, node list picture-in-picture over its corner; both kept
# well inside the crop-safe middle.
# Two screens stacked: chat on top, node list below — both fully inside a
# square canvas, so a centre-crop card can't cut either one.
place("chat", 80, 180, 3.5, TEAL)       # 840x472 -> (920, 652)
place("nodes", 330, 560, 2.5, ORANGE)   # 600x338 -> (930, 898)

out = os.path.join(S, "m5burner-cover.png")
im.save(out)
print("saved", out)
