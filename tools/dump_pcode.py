#!/usr/bin/env python3
"""Dump Ghidra P-code for the game's native library - the one heavy step of the build.

The static recompiler does not read machine code directly: it reads Ghidra's P-code, which
is the decoder's own output, so the project never has to contain an ARM decoder.

Three things happen here, in this order, because the third needs the project the first
one created:

  1. import the library into a Ghidra project (the slow step - it is the analysis);
  2. dump the P-code of every function to a TSV;
  3. recover the functions Ghidra did not define (they are reachable only through tables)
     into a second TSV that the emitter merges.

Ghidra is found automatically, and a Ghidra that only exists inside WSL is used through
`wsl.exe`.  Everything else is plain Python, so this runs on Windows, Linux and macOS.

    python tools/dump_pcode.py                 # import, dump, recover
    python tools/dump_pcode.py --reuse         # reuse the project, skip the analysis
    python tools/dump_pcode.py --ghidra /c/ghidra_12.1.4
"""

import argparse
import hashlib
import os
import shutil
import subprocess
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import prereq  # noqa: E402
from pyrun import NO_WINDOW  # noqa: E402

ROOT = Path(__file__).resolve().parent.parent

# The functions Ghidra's analysis does not define: they are only reachable through data
# tables (a PVRTC decoder entry point and a virtual method), so the analysis never sees
# them.  The emitter merges them back in; without them the port cannot decode textures.
RECOVERED = [
    "0x553b70", "0x552e68", "0x6c8660", "0x6f65f8",
    "0x6f725c", "0x6f728c", "0x553208", "0x552e8c",
]

# Places a Ghidra install is usually found.
GHIDRA_GUESSES = [
    "/opt/tools/ghidra",
    "/usr/local/ghidra",
    "/opt/ghidra",
    "C:/ghidra",
    "C:/Program Files/ghidra",
    "C:/Program Files/ghidra_12.1.4_PUBLIC",
]


def log(msg: str) -> None:
    print(msg, flush=True)


def find_ghidra(explicit: str | None, allow_download: bool = False):
    """Return (command_prefix, kind) for the headless analyser, or (None, None)."""
    candidates = []
    if not explicit:
        fetched = prereq.find_fetched("ghidra")
        if fetched:
            candidates.append(str(fetched))
    if explicit:
        candidates.append(explicit)
    env = os.environ.get("GHIDRA_INSTALL_DIR")
    if env:
        candidates.append(env)
    candidates.extend(GHIDRA_GUESSES)

    for cand in candidates:
        cand = cand.rstrip("/\\")
        # The launcher is either directly under the install directory or inside the
        # versioned subdirectory the package unpacks into.
        home = prereq.find_home(Path(cand), prereq.PREREQS["ghidra"]["marker"])
        if home is not None:
            return [str(home / "support" / prereq.headless_name())], "native"

    # On PATH?
    for name in ("analyzeHeadless", "analyzeHeadless.bat"):
        found = shutil.which(name)
        if found:
            return [found], "native"

    if not explicit and allow_download:
        fetched = prereq.fetch("ghidra", True)
        if fetched:
            launcher = fetched / "support" / prereq.headless_name()
            if launcher.exists():
                return [str(launcher)], "native"

    # A Ghidra that only exists inside WSL.
    if shutil.which("wsl"):
        try:
            probe = subprocess.run(
                ["wsl", "-e", "sh", "-c", "test -x /opt/tools/ghidra/support/analyzeHeadless && echo yes"],
                capture_output=True, text=True, timeout=60)
            if probe.stdout.strip() == "yes":
                return ["wsl", "-e", "/opt/tools/ghidra/support/analyzeHeadless"], "wsl"
        except (OSError, subprocess.SubprocessError):
            pass

    return None, None


def to_wsl_path(path: Path) -> str:
    """Translate a Windows path for WSL (/mnt/c/...)."""
    text = str(path.resolve())
    if len(text) > 2 and text[1] == ":":
        drive = text[0].lower()
        rest = text[2:].replace("\\", "/")
        return f"/mnt/{drive}{rest}"
    return text


def run(cmd, cwd, kind: str) -> int:
    """Run the analyser, streaming its output."""
    log("  $ " + " ".join(str(c) for c in cmd))
    start = time.time()
    proc = subprocess.Popen(cmd, cwd=str(cwd), stdout=subprocess.PIPE,
                            stderr=subprocess.STDOUT, text=True,
                            encoding="utf-8", errors="replace", bufsize=1,
                            creationflags=NO_WINDOW)
    assert proc.stdout is not None
    for line in proc.stdout:
        line = line.rstrip()
        if line:
            log("    " + line)
    rc = proc.wait()
    log(f"  ... finished in {time.time() - start:.0f}s (exit {rc})")
    return rc


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--ghidra", help="Ghidra install directory (or the analyseHeadless path)")
    ap.add_argument("--so", help="the native library (default: analysis/apk/lib/armeabi-v7a/libFast5.so)")
    ap.add_argument("--project", help="Ghidra project directory (default: analysis/ghidra-work)")
    ap.add_argument("--out", help="P-code TSV (default: analysis/reports/pcode_all.tsv)")
    ap.add_argument("--extra", help="recovered-functions TSV (default: analysis/reports/pcode_extra.tsv)")
    ap.add_argument("--max-function-size", type=int, default=0,
                    help="skip functions larger than this many bytes (0 = no limit, the default)")
    ap.add_argument("--reuse", action="store_true",
                    help="reuse an existing project instead of re-analysing (much faster)")
    ap.add_argument("--no-recover", action="store_true",
                    help="do not recover the table-only functions")
    ap.add_argument("--no-download", action="store_true",
                    help="never download anything; report what is missing instead")
    args = ap.parse_args()

    so = Path(args.so) if args.so else ROOT / "analysis/apk/lib/armeabi-v7a/libFast5.so"
    project = Path(args.project) if args.project else ROOT / "analysis/ghidra-work"
    out = Path(args.out) if args.out else ROOT / "analysis/reports/pcode_all.tsv"
    extra = Path(args.extra) if args.extra else ROOT / "analysis/reports/pcode_extra.tsv"
    scripts = ROOT / "recomp/ghidra_scripts"

    if not so.is_file():
        log(f"ERROR: the native library is missing: {so}")
        log("       Run tools/bootstrap.py, which extracts it from the APK.")
        return 2

    prefix, kind = find_ghidra(args.ghidra, allow_download=not args.no_download)
    if not prefix:
        log("ERROR: Ghidra was not found.")
        log("       Install it (https://ghidra-sre.org) and either put it on PATH, set")
        log("       GHIDRA_INSTALL_DIR, or pass --ghidra <install dir>.")
        log("       A Ghidra inside WSL is used automatically.")
        return 2
    log(f"ghidra: {' '.join(prefix)}")
    # Ghidra's launcher needs a JDK 21 or newer.  The fetched one wins over an older
    # JAVA_HOME that happens to be set on the machine.
    jdk = prereq.find_fetched("jdk")
    current = os.environ.get("JAVA_HOME")
    if jdk and (not current or prereq.jdk_version(Path(current)) < 21):
        os.environ["JAVA_HOME"] = str(jdk)
        log(f"jdk: {jdk}")

    out.parent.mkdir(parents=True, exist_ok=True)

    if args.reuse:
        if not (project / "fast5.gpr").exists() and not (project / "fast5.rep").exists():
            log(f"ERROR: no project to reuse in {project}")
            return 2
        log("reusing the existing Ghidra project (no re-analysis)")
        mode = ["-process", "-noanalysis"]
    else:
        if project.exists():
            log(f"removing the old project {project}")
            shutil.rmtree(project, ignore_errors=True)
        project.mkdir(parents=True, exist_ok=True)
        log("importing the library into Ghidra (this is the slow step)")
        mode = ["-import", to_wsl_path(so) if kind == "wsl" else str(so)]

    # Ghidra's launcher already caps the JVM heap (MAXMEM in support/launch.sh, 2 GB by
    # default), so the analysis stays inside a couple of gigabytes.
    cmd = prefix + [to_wsl_path(project) if kind == "wsl" else str(project), "fast5",
                    *mode,
                    "-scriptPath", to_wsl_path(scripts) if kind == "wsl" else str(scripts),
                    "-postScript", "DumpPcode.java",
                    to_wsl_path(out) if kind == "wsl" else str(out),
                    str(args.max_function_size)]
    if run(cmd, ROOT, kind) != 0:
        log("ERROR: Ghidra failed.")
        return 1

    if not out.exists() or out.stat().st_size == 0:
        log(f"ERROR: Ghidra produced no P-code at {out}")
        return 1
    log(f"p-code: {out} ({out.stat().st_size / 1048576:.0f} MiB)")

    if not args.no_recover:
        log("recovering the functions that only the data tables reach")
        cmd = prefix + [to_wsl_path(project) if kind == "wsl" else str(project), "fast5",
                        "-process", "-noanalysis",
                        "-scriptPath", to_wsl_path(scripts) if kind == "wsl" else str(scripts),
                        "-postScript", "AddAndDumpFunctions.java",
                        to_wsl_path(extra) if kind == "wsl" else str(extra),
                        *RECOVERED]
        if run(cmd, ROOT, kind) != 0:
            log("ERROR: recovering the table-only functions failed.")
            return 1
        if extra.exists():
            log(f"recovered: {extra} ({extra.stat().st_size / 1024:.0f} KiB)")
        else:
            log("WARNING: no recovered-function file was written; the port will fault on a "
                "virtual call and will miss some textures.")

    # Stamp the dump with the library it came from, so a later run can tell whether it is
    # still current without trusting file timestamps.  The stamp is written last: it means
    # "the dump AND the recovered functions both match this library".
    try:
        digest = hashlib.sha256(so.read_bytes()).hexdigest()
        (out.parent / (out.stem + ".stamp")).write_text(digest + "\n")
        log(f"stamped: {digest}")
    except OSError as exc:
        log(f"could not write the stamp ({exc})")

    return 0


if __name__ == "__main__":
    sys.exit(main())
