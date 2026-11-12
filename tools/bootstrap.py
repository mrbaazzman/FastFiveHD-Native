#!/usr/bin/env python3
"""Build the native Windows port from nothing but the game.

This is the end-user entry point.  Give it the APK and the game data and it produces a
runnable `build/fast5_win.exe`:

    python tools/bootstrap.py --apk "00742-Fast-5-HD-1.0.9-PowerVR.apk"
    python tools/bootstrap.py --apk "..." --data "D:/FastFiveHD/GloftF5HP"

What it does, in order:

  1. checks the toolchain (Python, a C compiler, Ghidra);
  2. extracts the native library from the APK and verifies it;
  3. points the port at the game data (a link, not a 775 MB copy);
  4. builds the relocated guest image the port runs;
  5. dumps the P-code with Ghidra (the slow step - see tools/dump_pcode.py);
  6. translates every function to C;
  7. compiles and links the native Windows binary.

Steps 5 and 6 are cached: re-running the script after a source change only redoes the last
ones.  Everything it produces lives under `build/`, which is disposable.
"""

import argparse
import hashlib
import os
import shutil
import subprocess
import sys
import time
import zipfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import prereq  # noqa: E402
import pyrun  # noqa: E402
from pyrun import NO_WINDOW  # noqa: E402

ROOT = Path(__file__).resolve().parent.parent

# The port is tied to this exact library: the recompilation works on the shipped
# instructions at their shipped addresses, so a different build is a different port.
SO_SHA256 = "1c8d16f572c3b504edb2084b1a7887bfe8c7659a5d67f0690e813c1e483ccb12"
SO_IN_APK = "lib/armeabi-v7a/libFast5.so"
SO_PATH = ROOT / "analysis/apk/lib/armeabi-v7a/libFast5.so"
PCODE = ROOT / "analysis/reports/pcode_all.tsv"
# The functions that only the data tables reach (a PVRTC decoder entry point and a virtual
# method).  Without them the port faults on a virtual call during the boot, so the P-code
# dump is only reusable when they are there too.
PCODE_EXTRA = ROOT / "analysis/reports/pcode_extra.tsv"
PCODE_STAMP = ROOT / "analysis/reports/pcode_all.stamp"
CHUNKS = ROOT / "build/link"
EXE = ROOT / "build/fast5_win.exe"

# The data tree the game reads at run time.
DATA_LINK = ROOT / "GloftF5HP"


def log(msg: str = "") -> None:
    print(msg, flush=True)


def step(n: int, total: int, title: str) -> None:
    log(f"\n[{n}/{total}] {title}")
    log("-" * 66)


def run(script, args, label: str) -> int:
    """Run one of the build steps (as a script, so it works from a frozen executable too)."""
    log(f"  $ {Path(script).name} {' '.join(str(a) for a in args)}")
    t0 = time.time()
    proc = pyrun.run(script, args, cwd=ROOT)
    if proc.stdout:
        for line in proc.stdout.splitlines():
            log("    " + line)
    if proc.returncode != 0 and proc.stderr:
        for line in proc.stderr.splitlines()[-20:]:
            log("    " + line)
    log(f"  ... {label} finished in {time.time() - t0:.0f}s (exit {proc.returncode})")
    return proc.returncode


def find_angle() -> str | None:
    """Look for a GLES2 backend (libEGL.dll) where the port looks for it."""
    bases = [
        ROOT / "third_party/angle",
        Path("C:/Program Files/Google/Chrome/Application"),
        Path("C:/Program Files (x86)/Google/Chrome/Application"),
        Path("C:/Program Files/Microsoft/Edge/Application"),
        Path("C:/Program Files (x86)/Microsoft/Edge/Application"),
        Path("C:/Program Files/BraveSoftware/Brave-Browser/Application"),
        Path("C:/Program Files (x86)/BraveSoftware/Brave-Browser/Application"),
        Path("C:/Program Files/Microsoft/EdgeDev"),
    ]
    env = os.environ.get("FAST5_ANGLE_DIR")
    if env:
        bases.insert(1, Path(env))
    for base in bases:
        flat = base / "libEGL.dll"
        if flat.exists():
            return str(flat)
        try:                       # Chromium keeps it in a versioned subdirectory
            for sub in sorted(base.iterdir()):
                candidate = sub / "libEGL.dll"
                if sub.is_dir() and candidate.exists():
                    return str(candidate)
        except OSError:
            continue
    return None


def free_gb(path: Path) -> float:
    try:
        return shutil.disk_usage(path).free / 1073741824.0
    except OSError:
        return 0.0


# --------------------------------------------------------------------------- tools

def check_toolchain(skip_ghidra: bool, allow_download: bool = True) -> bool:
    ok = True
    if sys.version_info < (3, 9):
        log("  ERROR: Python 3.9 or newer is required.")
        ok = False

    cc = None
    for name in ("gcc", "cc", "clang"):
        if shutil.which(name):
            cc = name
            break
    if cc:
        ver = subprocess.run([cc, "--version"], capture_output=True, text=True,
                             creationflags=NO_WINDOW)
        log(f"  compiler: {ver.stdout.splitlines()[0] if ver.stdout else cc}")
        if cc in ("cc", "clang") and sys.platform == "win32":
            log("  WARNING: 'cc'/'clang' on Windows may not target x86-64 MinGW; "
                "install MinGW-w64 GCC if the link fails.")
    else:
        fetched = prereq.fetch("mingw", allow_download)
        if fetched:
            log(f"  compiler: {fetched} (fetched)")
        else:
            log("  ERROR: no C compiler found and it could not be downloaded.")
            ok = False

    if not skip_ghidra:
        sys.path.insert(0, str(ROOT / "tools"))
        import dump_pcode  # noqa: E402
        prefix, kind = dump_pcode.find_ghidra(None, allow_download=allow_download)
        if prefix:
            log(f"  ghidra: {' '.join(prefix)}")
        else:
            log("  ERROR: Ghidra not found and it could not be downloaded.")
            ok = False
        jdk = prereq.fetch("jdk", allow_download)
        if jdk:
            log(f"  jdk: {jdk}")

    # A GLES2 backend.  The port talks to a real EGL/GLES2 implementation; ANGLE (Google's
    # GL ES 2.0 over Direct3D 11) is the one this project was developed against, and every
    # current Chromium build ships it.  This is the same search the port itself does.
    angle = find_angle()
    if angle:
        log(f"  GLES2 backend: {angle}")
    else:
        log("  WARNING: no GLES2 backend (libEGL.dll) found in the usual places.")
        log("           The port needs one: install ANGLE, or drop libEGL.dll and")
        log("           libGLESv2.dll into third_party/angle/.")
        log("           Any Chromium-based browser ships it - see third_party/angle/README.md.")

    need = 3.0
    have = free_gb(ROOT)
    log(f"  disk space: {have:.1f} GiB free (about {need:.0f} GiB needed)")
    if have < need:
        log("  WARNING: not enough free disk space; the build may fail.")
    return ok


# ---------------------------------------------------------------------------- apk

def extract_library(apk: Path) -> bool:
    log(f"  APK: {apk} ({apk.stat().st_size / 1048576:.1f} MiB)")
    if not zipfile.is_zipfile(apk):
        log("  ERROR: that file is not a ZIP archive - is it really the APK?")
        return False
    with zipfile.ZipFile(apk) as z:
        names = z.namelist()
        if SO_IN_APK not in names:
            log(f"  ERROR: the APK has no {SO_IN_APK}.")
            log("         This port needs the armeabi-v7a build of Fast Five HD 1.0.9.")
            return False
        data = z.read(SO_IN_APK)
    digest = hashlib.sha256(data).hexdigest()
    log(f"  {SO_IN_APK}: {len(data)} bytes")
    log(f"  sha256: {digest}")
    if digest != SO_SHA256:
        log("  ERROR: this is not the library the port was built for.")
        log(f"         expected {SO_SHA256}")
        log("         The recompilation works on the shipped instructions at their shipped")
        log("         addresses, so a different build is a different port.")
        return False
    SO_PATH.parent.mkdir(parents=True, exist_ok=True)
    SO_PATH.write_bytes(data)
    log(f"  extracted to {SO_PATH.relative_to(ROOT)}")
    return True


# --------------------------------------------------------------------------- data

def remove_path(path: Path) -> None:
    """Remove a file, a symlink or a directory junction (Windows) or directory."""
    if not path.exists() and not path.is_symlink():
        return
    if path.is_symlink() or path.is_file():
        path.unlink()
        return
    try:
        path.rmdir()          # a Windows junction
    except OSError:
        shutil.rmtree(path, ignore_errors=True)


def link_data(data: Path) -> bool:
    res = data / "res"
    if not res.is_dir():
        log(f"  ERROR: {data} has no 'res' directory - that is not the game data tree.")
        log("         It looks like .../sdcard/gameloft/games/GloftF5HP on a device.")
        return False
    dats = list(res.glob("file*.dat"))
    total = sum(f.stat().st_size for f in dats)
    log(f"  data: {len(dats)} payloads, {total / 1073741824:.2f} GiB")
    if not dats:
        log("  ERROR: no 'file*.dat' payloads found in the data tree.")
        return False

    if DATA_LINK.exists() or DATA_LINK.is_symlink():
        try:
            if DATA_LINK.resolve() == data.resolve():
                log(f"  already linked: {DATA_LINK}")
                return True
        except OSError:
            pass
        log(f"  removing the old link {DATA_LINK}")
        remove_path(DATA_LINK)

    DATA_LINK.parent.mkdir(parents=True, exist_ok=True)
    try:
        if os.name == "nt":
            subprocess.run(["cmd", "/c", "mklink", "/J", str(DATA_LINK), str(data)],
                           check=True, capture_output=True,
                           creationflags=NO_WINDOW)
        else:
            DATA_LINK.symlink_to(data.resolve(), target_is_directory=True)
        log(f"  linked {DATA_LINK} -> {data}")
        return True
    except (OSError, subprocess.SubprocessError) as exc:
        log(f"  could not link ({exc}); copying instead (that is {total / 1073741824:.1f} GiB)")
        shutil.copytree(data, DATA_LINK)
        return True


# ------------------------------------------------------------------------ pipeline

def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--apk", help="the game's APK")
    ap.add_argument("--data", help="the game data tree (the GloftF5HP directory)")
    ap.add_argument("--ghidra", help="Ghidra install directory")
    ap.add_argument("--jobs", type=int, default=os.cpu_count() or 8)
    ap.add_argument("--skip-pcode", action="store_true",
                    help="reuse the existing P-code dump (do not run Ghidra)")
    ap.add_argument("--force", action="store_true",
                    help="redo everything, including the P-code dump")
    ap.add_argument("--no-run", action="store_true", help="build only, do not smoke-test")
    ap.add_argument("--no-download", action="store_true",
                    help="never download a missing tool; report it instead")
    args = ap.parse_args()

    total = 7
    log("Fast Five HD - native port build")
    log("=" * 66)

    step(1, total, "checking the toolchain")
    if not check_toolchain(args.skip_pcode, allow_download=not args.no_download):
        log("\nThe toolchain is not ready. Fix the problems above and run it again.")
        return 2

    if args.apk:
        step(2, total, "extracting the native library from the APK")
        if not extract_library(Path(args.apk).expanduser().resolve()):
            return 2
    else:
        step(2, total, "using the native library already in the tree")
        if not SO_PATH.is_file():
            log(f"  ERROR: {SO_PATH} is missing. Pass --apk <the game's APK>.")
            return 2
        log(f"  {SO_PATH.relative_to(ROOT)}")

    step(3, total, "pointing the port at the game data")
    if args.data:
        if not link_data(Path(args.data).expanduser().resolve()):
            return 2
    else:
        log("  (skipped - pass --data <the GloftF5HP directory> to set it up)")

    step(4, total, "building the guest image")
    if run(ROOT / "tools/patch_guest.py", [], "guest image") != 0:
        return 1

    step(5, total, "dumping the P-code (Ghidra)")
    # The dump is stamped with the digest of the library it came from, so re-extracting the
    # library from the APK does not invalidate it.
    fresh = False
    if PCODE.exists() and PCODE_EXTRA.exists() and PCODE_STAMP.exists() and not args.force:
        try:
            digest = hashlib.sha256(SO_PATH.read_bytes()).hexdigest()
            fresh = PCODE_STAMP.read_text().strip() == digest
        except OSError:
            fresh = False
    if args.skip_pcode or fresh:
        if fresh:
            log(f"  reusing {PCODE.name} (it was dumped from this exact library)")
        else:
            log("  skipped (--skip-pcode)")
    else:
        dump_args = []
        if args.ghidra:
            dump_args += ["--ghidra", args.ghidra]
        if run(ROOT / "tools/dump_pcode.py", dump_args, "P-code dump") != 0:
            log("  ERROR: the P-code dump failed.")
            return 1

    if not PCODE_EXTRA.exists():
        log("  ERROR: the recovered functions are missing "
            f"({PCODE_EXTRA.relative_to(ROOT)}).")
        log("         They are the functions only the game's data tables reach, and without")
        log("         them the port faults on a virtual call during the boot.")
        log("         Run tools/dump_pcode.py to produce them.")
        return 2

    step(6, total, "translating every function to C")
    if CHUNKS.exists() and not args.force and any(CHUNKS.glob("chunk_*.c")):
        log("  (the generated chunks are present; re-run with --force to redo them)")
    else:
        link_args = [str(PCODE), "--emit-only", "--jobs", str(args.jobs)]
        if run(ROOT / "recomp/link_all.py", link_args, "translation") != 0:
            return 1

    step(7, total, "compiling and linking the native Windows binary")
    # The build picks its own parallelism: compiling the generated chunks is memory
    # hungry, so the job count follows the free memory rather than the core count.
    build_args = []
    if args.force:
        build_args.append("--force")
    if run(ROOT / "tools/build_win.py", build_args, "native build") != 0:
        return 1

    log("\n" + "=" * 66)
    log(f"built: {EXE.relative_to(ROOT)}  ({EXE.stat().st_size / 1048576:.0f} MiB)")
    if not args.no_run:
        log("\nsmoke test: booting the game for 20 seconds ...")
        log_path = ROOT / "build/bootstrap.log"
        pyrun.run(ROOT / "tools/run_test.py",
                  ["--width", "1280", "--height", "720", "--seconds", "20",
                   "--log", str(log_path)], cwd=ROOT)
        # The game is killed when the test ends, so its exit code means nothing; the log
        # is what says whether it survived the boot.
        faults = 0
        try:
            faults = log_path.read_text(errors="replace").count("GUEST FAULT")
        except OSError:
            pass
        log(f"  the log is in {log_path.relative_to(ROOT)}")
        if faults:
            log(f"  ERROR: the game faulted {faults} time(s) during the smoke test.")
            log("         The most common cause is a missing P-code dump or a build that")
            log("         does not match the library - see the log for the address.")
        else:
            log("  the game booted cleanly.")
    log("\nPlay it with:  play.bat        (or play.bat menu / play.bat race)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
