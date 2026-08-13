#!/usr/bin/env python3
"""One-off generator for a white-background, black-ink version of the LED
index map -- printable / usable without a dark viewer background, unlike
the transparent yellow/white dot overlays. Reads led_map.json, writes
led_map_dots_bw.png in the repo root.
"""
import json
from pathlib import Path

from PIL import Image, ImageDraw, ImageFont

ROOT = Path(__file__).resolve().parent.parent
CANVAS_SIZE = (1920, 1080)
PADDING = 20

def main():
    mapping = json.loads((ROOT / "led_map.json").read_text())

    img = Image.new("RGB", CANVAS_SIZE, "white")
    draw = ImageDraw.Draw(img)
    try:
        font = ImageFont.truetype("DejaVuSans.ttf", 13)
    except OSError:
        font = ImageFont.load_default()

    for idx_str, points in mapping.items():
        idx = int(idx_str)
        for x, y in points:
            draw.ellipse((x - 6, y - 6, x + 6, y + 6), outline="black", width=1)
            draw.text((x + 8, y - 8), str(idx), fill="black", font=font)

    bbox = img.getbbox()
    # getbbox() on an RGB image with a white background won't find a
    # tight box (no alpha), so compute it manually against white.
    import numpy as np
    arr = np.array(img)
    nonwhite = np.any(arr != 255, axis=2)
    ys, xs = np.where(nonwhite)
    x0, x1 = max(xs.min() - PADDING, 0), min(xs.max() + PADDING, CANVAS_SIZE[0])
    y0, y1 = max(ys.min() - PADDING, 0), min(ys.max() + PADDING, CANVAS_SIZE[1])
    img = img.crop((x0, y0, x1, y1))

    out_path = ROOT / "led_map_dots_bw.png"
    img.save(out_path)
    print(f"wrote {out_path} ({img.size[0]}x{img.size[1]})")

if __name__ == "__main__":
    main()
