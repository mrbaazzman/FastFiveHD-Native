#!/usr/bin/env python3
"""Report whether a captured frame is vertically flipped.

The Fast Five logo sits in the top-left corner of a correctly oriented frame, so the
brightness of the top-left patch is compared with the bottom-left patch.
"""
import sys

from PIL import Image


def mean_luma(im, box):
    patch = im.crop(box).convert("L")
    data = list(patch.getdata())
    return sum(data) / float(len(data)) if data else 0.0


def main(paths):
    for path in paths:
        im = Image.open(path)
        w, h = im.size
        top = mean_luma(im, (0, 0, 260, 140))
        bottom = mean_luma(im, (0, h - 140, 260, h))
        verdict = "flipped" if bottom > top + 8 else "upright"
        print("%-34s %dx%d  top-left=%6.1f bottom-left=%6.1f  %s"
              % (path, w, h, top, bottom, verdict))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:] or ["build/game_frame_1320.png"]))
