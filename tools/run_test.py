#!/usr/bin/env python3
"""Headless test run for the Fast Five HD port.

Starts build/fast5_win.exe with a given window size, lets it run for a fixed
number of seconds, then stops it. The in-engine capture hook writes
build/game_frame_<n>.bmp; this script converts the newest ones to PNG so they
can be inspected.

    python tools/run_test.py --width 1920 --height 1080 --seconds 90 --env FAST5_AUTO_MENU=1
"""
import argparse
import glob
import os
import subprocess
import sys
import time

from PIL import Image


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--width", type=int, default=800)
    ap.add_argument("--height", type=int, default=480)
    ap.add_argument("--seconds", type=float, default=60.0)
    ap.add_argument("--env", action="append", default=[],
                    help="NAME=VALUE (repeatable)")
    ap.add_argument("--png", action="store_true",
                    help="convert build/game_frame_*.bmp to png")
    ap.add_argument("--log", default="build/fast5.log")
    args = ap.parse_args()

    env = dict(os.environ)
    env["FAST5_FAKE_GPU"] = "1"
    env["FAST5_WIDTH"] = str(args.width)
    env["FAST5_HEIGHT"] = str(args.height)
    env["FAST5_LOG"] = args.log
    env.setdefault("FAST5_TEST_MINIMIZED", "1")
    for item in args.env:
        name, _, value = item.partition("=")
        env[name] = value

    before = set(glob.glob("build/game_frame_*.bmp"))
    proc = subprocess.Popen(["build/fast5_win.exe"], env=env)
    deadline = time.time() + args.seconds
    try:
        while time.time() < deadline and proc.poll() is None:
            time.sleep(0.5)
    finally:
        if proc.poll() is None:
            proc.terminate()
            try:
                proc.wait(timeout=10)
            except subprocess.TimeoutExpired:
                proc.kill()
                proc.wait(timeout=10)
    print("exit code:", proc.returncode)

    new = sorted(set(glob.glob("build/game_frame_*.bmp")) - before,
                 key=lambda p: os.path.getmtime(p))
    if args.png:
        for path in new:
            out = path[:-4] + ".png"
            try:
                Image.open(path).convert("RGB").save(out)
                print("png:", out)
            except Exception as exc:  # pragma: no cover
                print("convert failed:", path, exc)
    else:
        for path in new:
            print("bmp:", path)
    return 0


if __name__ == "__main__":
    sys.exit(main())
