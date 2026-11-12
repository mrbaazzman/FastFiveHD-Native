#!/usr/bin/env python3
"""Compare a frame captured at two resolutions.

The 800x480 capture is scaled up to the 1920x1080 capture and the difference is reported
per tile, so elements that are missing (or displaced) at the higher resolution show up as
dark/bright tiles instead of being invisible in a screenshot.
"""
import sys

from PIL import Image, ImageChops


def tiles(a, b, cols=12, rows=7):
    """Mean absolute difference per tile of a vs b."""
    w, h = a.size
    tw, th = w // cols, h // rows
    out = []
    for ty in range(rows):
        for tx in range(cols):
            box = (tx * tw, ty * th, (tx + 1) * tw, (ty + 1) * th)
            pa = a.crop(box).convert("L")
            pb = b.crop(box).convert("L")
            diff = ImageChops.difference(pa, pb)
            hist = diff.histogram()
            total = sum(hist)
            mean = sum(i * c for i, c in enumerate(hist)) / float(total or 1)
            out.append((round(mean, 1), tx * tw, ty * th, tw, th))
    out.sort(reverse=True)
    return out


def main(argv):
    if len(argv) < 3:
        print("usage: compare_frames.py <small.bmp> <big.bmp> [out.png]")
        return 1
    small = Image.open(argv[1]).convert("RGB")
    big = Image.open(argv[2]).convert("RGB")
    s = small.resize(big.size, Image.LANCZOS)
    diff = ImageChops.difference(s, big).convert("L")
    diff = diff.point(lambda v: min(255, v * 4))
    if len(argv) > 3:
        diff.save(argv[3])
    print("biggest tiles (mean diff, x, y, w, h):")
    for mean, x, y, w, h in tiles(s, big)[:14]:
        print("  %6.1f  at %4d,%4d (%dx%d)" % (mean, x, y, w, h))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
