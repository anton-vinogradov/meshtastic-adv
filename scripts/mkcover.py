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
W, H = 1200, 800
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
    img = img.resize((img.width * scale, img.height * scale), Image.NEAREST)
    d.rectangle([x + 12, y + 12, x + img.width + 12, y + img.height + 12], fill=DROP)
    d.rectangle([x - 4, y - 4, x + img.width + 3, y + img.height + 3], fill=border)
    im.paste(img, (x, y))

# титул: MESHTASTIC (teal) + ADV (orange), затем подзаголовок
t1 = "MESHTASTIC"
px_text(t1, 48, 42, TEAL, 5, shadow=DROP)
px_text("ADV", 48 + text_w(t1, 5) + 34, 26, ORANGE, 8, shadow=DROP)
px_text("KEYBOARD-FIRST MESH CLIENT FOR CARDPUTER", 50, 108, WHITE, 2)

# экраны: чат — герой, нодлист целиком картинкой-в-картинке поверх угла
place("chat", 28, 195, 4, TEAL)         # 960x540 -> до (988, 735)
place("nodes", 712, 510, 2, ORANGE)     # 480x270 -> до (1170, 765), поверх угла героя

out = os.path.join(S, "m5burner-cover.png")
im.save(out)
print("saved", out)
