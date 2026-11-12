#!/usr/bin/env python3
"""One command for the whole job.

    python build.py              build the installer and test it end to end
    python build.py installer    just build dist/FastFiveSetup.exe
    python build.py test         just run the end-to-end install test
    python build.py release      commit, tag and publish the installer

The product is `FastFiveSetup.exe`: a C# WinForms installer that carries the whole build
pipeline, so the end user needs only the game.  This script produces it and proves it
works.
"""

import argparse
import os
import shutil
import subprocess
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parent
PUB = ROOT / "public"
WORK = ROOT / "build"
SETUP = WORK / "FastFiveSetup.exe"


def say(msg: str) -> None:
    print(msg, flush=True)


def run(cmd, label: str) -> int:
    say(f"  {label} ...")
    t0 = time.time()
    log = WORK / (label.replace(" ", "_").replace("/", "_") + ".log")
    log.parent.mkdir(parents=True, exist_ok=True)
    with log.open("w", encoding="utf-8", errors="replace") as fh:
        rc = subprocess.run(cmd, cwd=str(ROOT), stdout=fh,
                            stderr=subprocess.STDOUT).returncode
    say(f"  {label}: {'ok' if rc == 0 else 'FAILED'} ({time.time() - t0:.0f}s, log: {log.name})")
    return rc


def ps(script: str, *args: str) -> int:
    return run(["powershell", "-NoProfile", "-ExecutionPolicy", "Bypass",
                "-File", str(ROOT / "tools" / script), *args], script)


def build_installer() -> int:
    # A running instance locks the executable.
    for name in ("FastFiveSetup.exe", "FastFivePortBuilder.exe", "fast5_win.exe"):
        subprocess.run(["taskkill", "/F", "/IM", name], capture_output=True)
    time.sleep(1)

    if run([sys.executable, "tools/build_builder.py"], "build builder") != 0:
        return 1
    shutil.copy2(ROOT / "dist/FastFivePortBuilder.exe", WORK / "fast5build.exe")
    if ps("build_toolchain.ps1") != 0:
        return 1
    if ps("make_builder.ps1") != 0:
        return 1
    if ps("build_installer.ps1") != 0:
        return 1
    if SETUP.exists():
        say(f"  the installer: {SETUP} ({SETUP.stat().st_size / 1048576:.0f} MiB)")
    return 0


def test_end_to_end() -> int:
    """Install the game with the installer and play it."""
    if not SETUP.exists():
        say("  the installer is not built yet")
        return 1
    apk = next(ROOT.glob("*.apk"), None)
    data = ROOT / "GloftF5HP"
    if not apk or not data.is_dir():
        say("  the APK or the game data is missing from the tree")
        return 1
    target = Path(os.environ.get("LOCALAPPDATA", str(Path.home()))) / "Programs" / "Fast Five HD"
    shutil.rmtree(target, ignore_errors=True)
    say(f"  installing into {target}")
    rc = run([str(SETUP), "--silent", "--apk", str(apk), "--data", str(data),
              "--dir", str(target), "--log", str(WORK / "install_test.log")], "install")
    if rc != 0:
        return rc
    exe = target / "fast5_win.exe"
    if not exe.exists():
        say("  the installer reported success but installed no game")
        return 1
    say(f"  the installed game: {exe} ({exe.stat().st_size / 1048576:.0f} MiB)")
    return 0


def publish() -> int:
    """Commit, tag and publish the installer as a release."""
    if not SETUP.exists():
        say("  the installer is not built yet")
        return 1
    tag = "v1.0.0"
    if run(["git", "add", "-A"], "git add") != 0:
        return 1
    if run(["git", "commit", "-m",
            "Installer: a single executable builds and installs the port from the game",
            "--author=Fast Five HD Port <noreply@example.com>"], "git commit") != 0:
        return 1
    run(["git", "tag", "-f", tag], "git tag")
    return run(["gh", "release", "upload", tag, str(SETUP),
                "--title", "Fast Five HD - Native Port Installer",
                "--notes", "One executable that builds the native port from your own copy of "
                           "the game. It carries the toolchain and the analysis of the "
                           "library, so nothing has to be installed."],
               "gh release")


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("what", nargs="?", default="all",
                    choices=["all", "installer", "test", "release"])
    args = ap.parse_args()

    say("Fast Five HD - native port")
    say("=" * 60)
    WORK.mkdir(exist_ok=True)

    if args.what in ("all", "installer"):
        if build_installer():
            return 1
    if args.what in ("all", "test"):
        if test_end_to_end():
            return 1
    if args.what == "release":
        if publish():
            return 1

    say("")
    say("done.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
