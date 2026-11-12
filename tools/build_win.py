#!/usr/bin/env python3
"""Incremental native Windows build: compile only changed chunks/runtime files with MinGW."""
import argparse
import os
import shutil
import subprocess
import sys
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import hostinfo  # noqa: E402
import prereq  # noqa: E402
from pyrun import NO_WINDOW  # noqa: E402

ROOT = Path(__file__).resolve().parent.parent
OUT = ROOT / "build" / "win"
OUT.mkdir(parents=True, exist_ok=True)

CHUNKS = sorted(ROOT.glob("build/link/chunk_*.c"))
RUNTIME = [
    ROOT / "runtime/main_win32.c",
    ROOT / "runtime/imports.c",
    ROOT / "runtime/vfs.c",
    ROOT / "runtime/guest_data.c",
    ROOT / "runtime/java_bridge.c",
    ROOT / "runtime/platform_win32.c",
    ROOT / "runtime/gl_win32.c",
    ROOT / "runtime/overrides.c",
    ROOT / "runtime/thread_win32.c",
    ROOT / "runtime/resolution.c",
]
TABLE = ROOT / "build/link/table.c"
SOURCES = CHUNKS + [TABLE] + RUNTIME
EXE = ROOT / "build" / "fast5_win.exe"

# Conservative dependency set: if one of these changes, affected translation units rebuild.
HEADERS = [
    ROOT / "runtime/cpu.h",
    ROOT / "runtime/mem.h",
    ROOT / "runtime/imports_table.h",
    ROOT / "runtime/resolution.h",
    ROOT / "runtime/platform.h",
]


# Debug info makes the executable about eight times larger; a release build leaves it out.
DEBUG = False


def object_for(src: Path) -> Path:
    return OUT / (src.stem + ".o")


def stale(src: Path, obj: Path) -> bool:
    if not obj.exists():
        return True
    try:
        obj_time = obj.stat().st_mtime
    except OSError:
        return True
    if src.stat().st_mtime > obj_time:
        return True
    for header in HEADERS:
        try:
            if header.exists() and header.stat().st_mtime > obj_time:
                return True
        except OSError:
            return True
    return False


def compiler() -> str:
    """The C compiler: the one on PATH, or the one the builder fetched."""
    if os.environ.get("CC"):
        return os.environ["CC"]
    found = shutil.which("gcc")
    if found:
        return found
    fetched = prereq.find_fetched("mingw")
    if fetched:
        for name in ("gcc.exe", "gcc"):
            candidate = fetched / "bin" / name
            if candidate.exists():
                return str(candidate)
    return "gcc"


def compile_one(src: Path):
    obj = object_for(src)
    # Generated chunks are the hot path (scene build, per-frame update): optimise them fully.
    opt = "-O2"
    cmd = [compiler(), "-std=c11", opt, "-Wall", "-Wextra",
           "-I", str(ROOT / "runtime"), "-c", str(src), "-o", str(obj)]
    if DEBUG:
        cmd.insert(4, "-g")
    p = subprocess.run(cmd, capture_output=True, text=True,
                       creationflags=NO_WINDOW)
    return src, p.returncode, p.stderr, p.stdout


def main() -> int:
    global DEBUG
    ap = argparse.ArgumentParser()
    ap.add_argument("--force", action="store_true", help="recompile and relink everything")
    ap.add_argument("--jobs", type=int, default=0,
                    help="parallel compile jobs (default: as many as the free memory allows)")
    ap.add_argument("--debug", action="store_true",
                    help="build with debug info (the binary is ~8x larger)")
    args = ap.parse_args()
    DEBUG = args.debug
    if not args.jobs:
        args.jobs = hostinfo.jobs()
        print(f"host: {hostinfo.describe()}")

    dirty = [src for src in SOURCES if args.force or stale(src, object_for(src))]
    if dirty:
        print(f"compiling {len(dirty)} of {len(SOURCES)} source(s) with {args.jobs} workers")
        failures = []
        with ThreadPoolExecutor(max_workers=max(1, min(args.jobs, len(dirty)))) as pool:
            for src, rc, err, out in pool.map(compile_one, dirty):
                if rc != 0:
                    failures.append((src, err, out))
        if failures:
            for src, err, out in failures[:5]:
                print(f"FAIL {src}\n{err[:3000]}\n{out[:1000]}")
            return 1
        print(f"compiled {len(dirty)} source(s)")
    else:
        print(f"all {len(SOURCES)} objects are up to date")

    objects = [object_for(src) for src in SOURCES]
    missing = [str(obj) for obj in objects if not obj.exists()]
    if missing:
        print(f"missing object(s): {', '.join(missing[:5])}")
        return 1

    exe_time = EXE.stat().st_mtime if EXE.exists() else -1.0
    need_link = args.force or not EXE.exists() or any(
        obj.stat().st_mtime > exe_time for obj in objects)
    if not need_link:
        print(f"up to date: {EXE}")
        return 0

    print(f"linking {EXE.name}")
    link_cmd = [compiler(), *(str(obj) for obj in objects),
                "-o", str(EXE), "-lm", "-lgdi32", "-luser32", "-lwinmm"]
    if DEBUG:
        link_cmd.insert(1, "-g")
    link = subprocess.run(link_cmd, capture_output=True, text=True,
                          creationflags=NO_WINDOW)
    if link.returncode != 0:
        print("LINK FAILED")
        if "Permission denied" in link.stderr or "cannot open output file" in link.stderr:
            print("The executable is probably running; stop fast5_win.exe and retry.")
        print(link.stderr[:5000])
        return 1
    print(f"linked {EXE}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
