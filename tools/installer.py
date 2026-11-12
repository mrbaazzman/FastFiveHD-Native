#!/usr/bin/env python3
"""Install the built port into a folder of the user's choosing.

The result is a self-contained game folder:

    <install>/
        fast5_win.exe      the native port
        play.bat           a launcher (starts the game in this folder)
        GloftF5HP/res/...  the game data
        build/userdata/    the game's writable data (created empty)

The game data is **copied**, never moved or modified: the port reads the shipped files in
place and writes only into `build/userdata`.

    python tools/installer.py --build <work dir> --data <GloftF5HP> --install <folder>
"""

import argparse
import os
import shutil
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent

LAUNCHER = """@echo off
rem Fast Five HD - native port launcher.
rem The game reads its data from this folder, so it has to start here.
cd /d "%~dp0"
fast5_win.exe %*
"""


def log(msg: str = "") -> None:
    print(msg, flush=True)


def human(n: float) -> str:
    for unit in ("B", "KiB", "MiB", "GiB"):
        if n < 1024 or unit == "GiB":
            return f"{n:.1f} {unit}"
        n /= 1024
    return f"{n:.1f} GiB"


def copy_tree(src: Path, dst: Path) -> tuple:
    """Copy a directory, reporting the progress."""
    files = [p for p in src.rglob("*") if p.is_file()]
    total = sum(p.stat().st_size for p in files)
    done = 0
    last = 0
    for path in files:
        rel = path.relative_to(src)
        out = dst / rel
        out.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(path, out)
        done += path.stat().st_size
        if total and done - last > (64 << 20):
            last = done
            log(f"    {done * 100 // total}%  ({human(done)} / {human(total)})")
    return len(files), total


def install(build_dir: Path, data: Path, target: Path) -> int:
    exe = build_dir / "fast5_win.exe"
    if not exe.exists():
        log(f"ERROR: there is no built executable at {exe}")
        return 2

    log(f"installing into {target}")
    target.mkdir(parents=True, exist_ok=True)

    # 1. the executable
    shutil.copy2(exe, target / "fast5_win.exe")
    log(f"  fast5_win.exe  ({human(exe.stat().st_size)})")

    # 2. the launcher
    (target / "play.bat").write_text(LAUNCHER, encoding="utf-8")
    log("  play.bat")

    # 3. the relocated guest image the port loads at start-up
    guest = build_dir / "guest.bin"
    if guest.exists():
        (target / "build").mkdir(parents=True, exist_ok=True)
        shutil.copy2(guest, target / "build" / "guest.bin")
        log(f"  build/guest.bin  ({human(guest.stat().st_size)})")
    else:
        log(f"  WARNING: there is no guest image at {guest}; the game will not start.")

    # 4. the game data
    src = data / "res"
    if not src.is_dir():
        log(f"ERROR: the game data has no 'res' directory: {data}")
        return 2
    dst = target / "GloftF5HP" / "res"
    if dst.exists():
        shutil.rmtree(dst, ignore_errors=True)
    t0 = time.time()
    count, size = copy_tree(src, dst)
    log(f"  GloftF5HP/res: {count} files, {human(size)}  ({time.time() - t0:.0f}s)")

    # 5. the game's writable area
    (target / "build" / "userdata").mkdir(parents=True, exist_ok=True)
    log("  build/userdata")

    log("")
    log("Done. The game is in:")
    log(f"  {target}")
    log("")
    log("Start it with play.bat, or run fast5_win.exe from that folder.")
    return 0


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--build", required=True, help="the folder holding the built executable")
    ap.add_argument("--data", required=True, help="the GloftF5HP game data tree")
    ap.add_argument("--install", required=True, help="where to install the game")
    args = ap.parse_args()
    return install(Path(args.build).resolve(), Path(args.data).resolve(),
                   Path(args.install).expanduser().resolve())


if __name__ == "__main__":
    sys.exit(main())
