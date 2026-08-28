#!/usr/bin/env python3
"""Build the square M5Burner cover from verified synthetic demo media."""

from __future__ import annotations

import argparse
from pathlib import Path

from PIL import Image, ImageDraw, ImageFont


def pixel_text(canvas: Image.Image, xy: tuple[int, int], text: str, color: tuple[int, int, int], scale: int) -> None:
    font = ImageFont.load_default()
    left, top, right, bottom = font.getbbox(text)
    mask = Image.new("L", (right - left + 2, bottom - top + 2), 0)
    ImageDraw.Draw(mask).text((1 - left, 1 - top), text, fill=255, font=font)
    mask = mask.resize((mask.width * scale, mask.height * scale), Image.Resampling.NEAREST)
    ink = Image.new("RGB", mask.size, color)
    canvas.paste(ink, xy, mask)


def build(source: Path, output: Path) -> None:
    if source.is_symlink() or not source.is_file():
        raise ValueError(f"safe demo source is missing: {source}")
    with Image.open(source) as image:
        image.seek(getattr(image, "n_frames", 1) - 1)
        screen = image.convert("RGB")
    if screen.size != (960, 540):
        raise ValueError(f"safe demo source must be 960x540, got {screen.size}")

    canvas = Image.new("RGB", (1000, 1000), (5, 6, 10))
    draw = ImageDraw.Draw(canvas)
    for y in range(1000):
        shade = 6 + y * 8 // 1000
        draw.line((0, y, 1000, y), fill=(shade, shade + 2, shade + 8))

    pixel_text(canvas, (70, 55), "MESHTASTIC ADV", (55, 225, 255), 6)
    pixel_text(canvas, (70, 120), "KEYBOARD-FIRST MESH MESSENGER", (238, 242, 247), 3)

    preview = screen.resize((864, 486), Image.Resampling.NEAREST)
    draw.rectangle((55, 177, 945, 689), fill=(0, 0, 0))
    draw.rectangle((62, 184, 938, 682), outline=(55, 225, 255), width=6)
    canvas.paste(preview, (68, 190))

    pixel_text(canvas, (70, 742), "TYPE. SEND. REACT. REPLY.", (255, 207, 63), 4)
    pixel_text(canvas, (70, 800), "ONBOARD LORA / BLE COMPANION", (238, 242, 247), 3)
    pixel_text(canvas, (70, 850), "NO PHONE REQUIRED", (66, 224, 106), 4)
    pixel_text(canvas, (70, 930), "REAL FIRMWARE / SYNTHETIC DEMO DATA", (164, 174, 192), 2)

    output.parent.mkdir(parents=True, exist_ok=True)
    canvas.save(output, format="PNG", optimize=True)


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source", required=True, type=Path, help="verified 960x540 demo GIF")
    parser.add_argument("--output", required=True, type=Path)
    args = parser.parse_args()
    build(args.source, args.output)
    print(f"M5Burner cover: {args.output}")


if __name__ == "__main__":
    main()
