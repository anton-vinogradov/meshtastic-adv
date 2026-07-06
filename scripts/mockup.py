#!/usr/bin/env python3
"""Composite real UI screenshots into a photo of the Cardputer ADV.

Regenerates the docs "hero" shots from the same frames the demo-dump produces,
so the photo assets stay current with the UI:

    python3 scripts/mockup.py --config docs/img/mockup.json \
        --frame shots/chats.png --out docs/img/hero.png
    python3 scripts/mockup.py --config docs/img/mockup.json \
        --frames shots/ --gif docs/img/hero.gif --delay 1400

The config holds the photo path and the screen's corner quad (pixels in the
original photo, order: TL TR BR BL). Mark the corners once per photo; use
--debug to render the quad outline for eyeballing.
"""

import argparse
import json
import sys
from pathlib import Path

from PIL import Image, ImageDraw


def solve8(a, b):
    """Gaussian elimination for the 8x8 system of the perspective transform."""
    n = 8
    m = [row[:] + [b[i]] for i, row in enumerate(a)]
    for col in range(n):
        piv = max(range(col, n), key=lambda r: abs(m[r][col]))
        if abs(m[piv][col]) < 1e-12:
            raise ValueError("degenerate quad")
        m[col], m[piv] = m[piv], m[col]
        d = m[col][col]
        m[col] = [v / d for v in m[col]]
        for r in range(n):
            if r != col and m[r][col]:
                f = m[r][col]
                m[r] = [v - f * w for v, w in zip(m[r], m[col])]
    return [m[i][n] for i in range(n)]


def perspective_coeffs(quad, size):
    """Coefficients mapping output (photo) pixels back into the frame rect."""
    w, h = size
    src = [(0, 0), (w, 0), (w, h), (0, h)]
    rows, rhs = [], []
    for (X, Y), (x, y) in zip(quad, src):
        rows.append([X, Y, 1, 0, 0, 0, -x * X, -x * Y])
        rhs.append(x)
        rows.append([0, 0, 0, X, Y, 1, -y * X, -y * Y])
        rhs.append(y)
    return solve8(rows, rhs)


def fill_quad(photo, quad, color=(5, 5, 5)):
    """Paint the full glass area (the frame is 16:9, the glass is slightly taller)."""
    out = photo.convert("RGB").copy()
    ImageDraw.Draw(out).polygon(quad, fill=color)
    return out


def composite(photo, frame, quad, scale=3):
    """Warp the 240x135 frame into the photo's screen quad (nearest keeps pixels crisp)."""
    # Upscale first so the pixel-art edges survive the resampling into the quad.
    frame = frame.convert("RGBA").resize((frame.width * scale, frame.height * scale), Image.NEAREST)
    coeffs = perspective_coeffs(quad, frame.size)
    layer = frame.transform(photo.size, Image.PERSPECTIVE, coeffs, Image.BICUBIC)

    # Alpha: opaque inside the quad, feathered 1px at the border by the transform itself.
    mask = Image.new("L", frame.size, 255)
    mask = mask.transform(photo.size, Image.PERSPECTIVE, coeffs, Image.BICUBIC)

    out = photo.convert("RGB").copy()
    out.paste(layer, (0, 0), mask)
    return out


def load_config(path):
    cfg = json.loads(Path(path).read_text())
    photo = Image.open(Path(path).parent / cfg["photo"]) if not Path(cfg["photo"]).is_absolute() else Image.open(cfg["photo"])
    return cfg, photo


def scaled_quad(cfg, photo, out_w):
    k = out_w / photo.width if out_w else 1.0
    quad = [(x * k, y * k) for x, y in cfg["corners"]]
    if out_w:
        photo = photo.resize((out_w, round(photo.height * out_w / photo.width)), Image.LANCZOS)
    return photo, quad


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--config", required=True)
    ap.add_argument("--frame", help="single screenshot -> PNG")
    ap.add_argument("--frames", help="directory of screenshots (sorted) -> GIF")
    ap.add_argument("--out", help="output PNG (with --frame)")
    ap.add_argument("--gif", help="output GIF (with --frames)")
    ap.add_argument("--delay", type=int, default=1400, help="GIF frame delay, ms")
    ap.add_argument("--width", type=int, default=900, help="output width (photo downscale)")
    ap.add_argument("--debug", action="store_true", help="draw the quad outline instead")
    args = ap.parse_args()

    cfg, photo = load_config(args.config)
    photo, quad = scaled_quad(cfg, photo, args.width)
    if "glass" in cfg:
        k = args.width / Image.open(Path(args.config).parent / cfg["photo"]).width if args.width else 1.0
        photo = fill_quad(photo, [(x * k, y * k) for x, y in cfg["glass"]])

    if args.debug:
        dbg = photo.convert("RGB").copy()
        d = ImageDraw.Draw(dbg)
        d.polygon(quad, outline=(255, 0, 0), width=2)
        for i, (x, y) in enumerate(quad):
            d.ellipse([x - 4, y - 4, x + 4, y + 4], outline=(255, 255, 0), width=2)
            d.text((x + 6, y + 6), "TL TR BR BL".split()[i], fill=(255, 255, 0))
        dbg.save(args.out or "mockup-debug.png")
        print("debug quad ->", args.out or "mockup-debug.png")
        return

    if args.frame:
        out = composite(photo, Image.open(args.frame), quad)
        out.save(args.out, quality=92)
        print("->", args.out)
    elif args.frames:
        paths = sorted(Path(args.frames).glob("*.png"))
        if not paths:
            sys.exit("no frames in " + args.frames)
        imgs = [composite(photo, Image.open(p), quad) for p in paths]
        # Quantize on the first frame's palette so the static photo doesn't shimmer.
        base = imgs[0].quantize(colors=255, dither=Image.FLOYDSTEINBERG)
        rest = [im.quantize(palette=base, dither=Image.FLOYDSTEINBERG) for im in imgs[1:]]
        base.save(args.gif, save_all=True, append_images=rest, duration=args.delay, loop=0, optimize=True)
        print("->", args.gif, f"({len(imgs)} frames)")
    else:
        sys.exit("need --frame or --frames")


if __name__ == "__main__":
    main()
