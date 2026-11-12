#!/usr/bin/env python3
"""Package the port builder into a single Windows executable.

    python tools/build_builder.py

The result is `dist/FastFivePortBuilder.exe`: one file, no Python installation needed by
the end user.  It contains the whole Python side of the build - the bootstrap pipeline,
the Ghidra dumper, the recompiler and the native build script.

Ghidra and a C compiler are *not* bundled (they are not ours, and they are large); the
builder finds them and says exactly what to install when they are missing.

Requires PyInstaller:  python -m pip install pyinstaller
"""

import argparse
import os
import shutil
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
NAME = "FastFivePortBuilder"
ENTRY = ROOT / "tools" / "builder_gui.py"
DIST = ROOT / "dist"
WORK = ROOT / "build" / "builder"


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--onefile", action="store_true", default=True,
                    help="one executable (the default)")
    ap.add_argument("--onedir", dest="onefile", action="store_false",
                    help="a folder instead of a single file")
    ap.add_argument("--clean", action="store_true", help="rebuild from scratch")
    ap.add_argument("--bundle", action="store_true",
                    help="embed the tools (Ghidra, a JDK, a compiler) in the executable, so "
                         "the installer needs no internet and no installed tools")
    args = ap.parse_args()

    try:
        import PyInstaller  # noqa: F401
    except ImportError:
        print("PyInstaller is not installed:")
        print("    python -m pip install pyinstaller")
        return 2

    # The bundled toolchain must not carry caches or generated output.
    for junk in ROOT.glob("tools/__pycache__"):
        shutil.rmtree(junk, ignore_errors=True)
    for junk in ROOT.glob("recomp/__pycache__"):
        shutil.rmtree(junk, ignore_errors=True)
    generated = ROOT / "recomp" / "generated"
    if generated.exists():
        shutil.rmtree(generated, ignore_errors=True)

    sep = ";" if os.name == "nt" else ":"

    # The packages the bundled installer carries.
    bundle_data = []
    link_dir = ROOT / "build" / "bundle" / "prereq"
    if args.bundle:
        sys.path.insert(0, str(ROOT / "tools"))
        import prereq
        cache = prereq.prereq_dir()
        cache.mkdir(parents=True, exist_ok=True)
        link_dir.mkdir(parents=True, exist_ok=True)
        for key, spec in prereq.PREREQS.items():
            name = spec["bundled"][0]
            src = cache / name
            if not src.exists():
                print(f"  fetching {spec['name']} to embed it ...")
                got = prereq.download_package(key)
                if not got:
                    print(f"  ERROR: could not fetch {spec['name']}")
                    return 1
                src = got
            # Hard-link the package into one folder: PyInstaller then takes the whole
            # folder as a single data entry.
            link = link_dir / name
            if not link.exists():
                try:
                    os.link(src, link)
                except OSError:
                    shutil.copy2(src, link)
            print(f"  embedding {spec['name']} ({src.stat().st_size / 1048576:.0f} MB)")
        bundle_data += ["--add-data", f"{link_dir}{sep}prereq"]
    # Absolute sources: --specpath makes PyInstaller resolve relative ones against the spec.
    cmd = [
        sys.executable, "-m", "PyInstaller",
        "--noconfirm",
        "--onefile" if args.onefile else "--onedir",
        "--name", NAME,
        "--windowed",                # no console window: the GUI must not flash one
        "--distpath", str(DIST),
        "--workpath", str(WORK),
        "--specpath", str(WORK),
        "--add-data", f"{ROOT / 'tools'}{sep}tools",
        "--add-data", f"{ROOT / 'recomp'}{sep}recomp",
        "--add-data", f"{ROOT / 'runtime'}{sep}runtime",
        "--add-data", f"{ROOT / 'play.bat'}{sep}.",
        *bundle_data,
        "--exclude-module", "PIL",
        "--exclude-module", "numpy",
        "--exclude-module", "tkinter.test",
        # The build scripts are bundled as *data*, so PyInstaller cannot see what they
        # import; the standard library pieces they use have to be asked for explicitly.
        "--hidden-import", "concurrent.futures",
        "--hidden-import", "concurrent.futures.thread",
        "--hidden-import", "concurrent.futures.process",
        "--hidden-import", "collections",
        "--hidden-import", "dataclasses",
        "--hidden-import", "typing",
        "--hidden-import", "runpy",
        "--hidden-import", "hostinfo",
        "--hidden-import", "importlib.util",
        "--hidden-import", "tkinter",
        "--hidden-import", "tkinter.filedialog",
        "--hidden-import", "tkinter.messagebox",
        "--hidden-import", "tkinter.ttk",
        str(ENTRY),
    ]
    print("$ " + " ".join(cmd))
    rc = subprocess.run(cmd, cwd=str(ROOT)).returncode
    if rc != 0:
        return rc

    exe = DIST / (f"{NAME}.exe" if args.onefile else NAME)
    if exe.exists():
        size = exe.stat().st_size / 1048576
        print(f"\nbuilt {exe}  ({size:.1f} MiB)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
