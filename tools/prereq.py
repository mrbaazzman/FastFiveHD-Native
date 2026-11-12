#!/usr/bin/env python3
"""The tools the build needs - found, or fetched.

The end user should not have to install Ghidra, a JDK and a C compiler by hand.  This
module looks for each of them:

  1. in the work directory (a previous run fetched them there);
  2. on the machine (the usual install places, PATH);
  3. otherwise it downloads the official package, verifies its SHA-256 and unpacks it.

Only what is missing is downloaded, so a machine that already has a compiler never
downloads one.  `FAST5_PREREQ_DIR` chooses where the fetched tools live.
"""

import hashlib
import os
import shutil
import subprocess
import sys
import tarfile
import urllib.request
import zipfile
from pathlib import Path

# A console program started from a windowed program opens a console window of its own
# unless it is told not to.
NO_WINDOW = getattr(subprocess, "CREATE_NO_WINDOW", 0x08000000)


def headless_name() -> str:
    """The headless analyser's launcher: a batch file on Windows, a script elsewhere."""
    return "analyzeHeadless.bat" if os.name == "nt" else "analyzeHeadless"


# The official packages.  The checksums are the publishers' own.
PREREQS = {
    "ghidra": {
        "name": "Ghidra 12.1.4 (the analysis)",
        "url": "https://github.com/NationalSecurityAgency/ghidra/releases/download/"
               "Ghidra_12.1.4_build/ghidra_12.1.4_PUBLIC_20260921.zip",
        "sha256": "ddac49f903da9d5bac833e5cc79395098b9c33cfd3279be5f31bd00387d2d4db",
        "size": 569649598,
        "marker": "support/" + headless_name(),
    },
    "jdk": {
        "name": "Eclipse Temurin JDK 21 (Ghidra needs it)",
        "url": "https://github.com/adoptium/temurin21-binaries/releases/download/"
               "jdk-21.0.13%2B9/OpenJDK21U-jdk_x64_windows_hotspot_21.0.13_9.zip",
        "sha256": "a4dbaa05fca4bd89d84584e58c459bd732efebb1722af80ab13c6516832004f9",
        "size": 205000000,
        "marker": "bin/java.exe",
    },
    "mingw": {
        "name": "MinGW-w64 GCC 16.2 (the compiler)",
        "url": "https://github.com/brechtsanders/winlibs_mingw/releases/download/"
               "16.2.0posix-23.1.3-14.0.0-ucrt-r3/"
               "winlibs-x86_64-posix-seh-gcc-16.2.0-llvm-23.1.3-mingw-w64ucrt-14.0.0-r3.zip",
        "sha256": "1f04e482797dc5f71fe2875cf2b743bfb9f5fd1de0955fdd3118438c0432ba5f",
        "size": 276000000,
        "marker": "bin/gcc.exe",
    },
}

# The file name each package has inside a bundled installer.
BUNDLED_NAMES = {
    "ghidra": "ghidra_12.1.4_PUBLIC_20260921.zip",
    "jdk": "OpenJDK21U-jdk_x64_windows_hotspot_21.0.13_9.zip",
    "mingw": "winlibs-x86_64-posix-seh-gcc-16.2.0-llvm-23.1.3-mingw-w64ucrt-14.0.0-r3.zip",
}
for _key, _name in BUNDLED_NAMES.items():
    PREREQS[_key]["bundled"] = [_name]

# Where the build looks for an installed copy.
SEARCH = {
    "ghidra": [
        "/opt/tools/ghidra", "/usr/local/ghidra", "/opt/ghidra",
        "C:/ghidra", "C:/Program Files/ghidra", "C:/Program Files/ghidra_12.1.4_PUBLIC",
    ],
    "jdk": [
        "C:/Program Files/Eclipse Adoptium/jdk-21",
        "C:/Program Files/Java/jdk-21",
        "C:/Program Files/Microsoft/jdk-21",
    ],
    "mingw": [
        "C:/mingw64", "C:/ProgramData/chocolatey/bin", "C:/Program Files/mingw64",
        "C:/msys64/mingw64", "C:/Program Files (x86)/mingw64",
    ],
}


def prereq_dir() -> Path:
    base = os.environ.get("FAST5_PREREQ_DIR")
    if base:
        return Path(base)
    local = os.environ.get("LOCALAPPDATA") or str(Path.home())
    return Path(local) / "FastFivePortBuilder" / "tools"


def log(msg: str) -> None:
    print(msg, flush=True)


def sha256_of(path: Path) -> str:
    h = hashlib.sha256()
    with path.open("rb") as fh:
        for block in iter(lambda: fh.read(1 << 20), b""):
            h.update(block)
    return h.hexdigest()


def find_installed(key: str) -> Path | None:
    """A copy that is already on the machine."""
    # (the installed copies are found below; the fetched ones come first)
    if key == "jdk":
        java = shutil.which("java")
        if java:
            home = Path(java).resolve().parent.parent
            if (home / "bin" / "javac.exe").exists() or (home / "bin" / "javac").exists():
                if jdk_ok(home):
                    return home
        # the usual install places, newest first
        best = None
        for base in SEARCH.get("jdk", []):
            root = Path(base)
            if not root.exists():
                continue
            if jdk_ok(root):
                return root
            try:
                for sub in sorted(root.iterdir()):
                    if sub.is_dir() and jdk_ok(sub):
                        best = best or sub
            except OSError:
                continue
        return best
    if key == "mingw":
        gcc = shutil.which("gcc")
        if gcc:
            return Path(gcc).resolve().parent.parent
        return None
    for base in SEARCH.get(key, []):
        root = Path(base)
        if not root.exists():
            continue
        marker = PREREQS[key]["marker"]
        if (root / marker).exists():
            return root
        # the marker may be one level down (a versioned directory)
        try:
            for sub in sorted(root.iterdir()):
                if sub.is_dir() and (sub / marker).exists():
                    return sub
        except OSError:
            continue
    return None


def jdk_version(home: Path) -> int:
    """The major version of a JDK, or 0 if it cannot be read."""
    java = home / "bin" / ("java.exe" if os.name == "nt" else "java")
    if not java.exists():
        return 0
    try:
        out = subprocess.run([str(java), "-version"], capture_output=True,
                             text=True, timeout=30, creationflags=NO_WINDOW)
        text = (out.stderr or "") + (out.stdout or "")
    except (OSError, subprocess.SubprocessError):
        return 0
    import re
    m = re.search(r'version "(\d+)', text)
    return int(m.group(1)) if m else 0


def jdk_ok(home: Path) -> bool:
    """Ghidra 12 needs a JDK 21 or newer."""
    return jdk_version(home) >= 21


def find_home(root: Path, marker: str) -> Path | None:
    """The tool's home directory inside an unpacked package.

    Packages unpack either directly into the tool's home or into a versioned
    subdirectory, so both are tried.
    """
    if (root / marker).exists():
        return root
    try:
        for sub in sorted(root.iterdir()):
            if sub.is_dir() and (sub / marker).exists():
                return sub
    except OSError:
        pass
    return None


def find_fetched(key: str) -> Path | None:
    root = prereq_dir() / key
    return find_home(root, PREREQS[key]["marker"])


def bundled_package(key: str) -> Path | None:
    """The package carried inside the executable, if there is one.

    A bundled installer needs no internet and no installed tools: the packages travel
    inside it and are unpacked on the first run.
    """
    meipass = getattr(sys, "_MEIPASS", None)
    if not meipass:
        return None
    for name in PREREQS[key].get("bundled", []):
        path = Path(meipass) / "prereq" / name
        if path.exists():
            return path
    return None


def unpack(archive: Path, dest: Path) -> None:
    dest.mkdir(parents=True, exist_ok=True)
    log(f"  unpacking {archive.name} ...")
    if archive.suffix == ".zip":
        with zipfile.ZipFile(archive) as z:
            z.extractall(dest)
    else:
        with tarfile.open(archive) as t:
            t.extractall(dest)


def download_package(key: str) -> Path | None:
    """The official package as an archive, downloaded whether or not it is installed.

    This is what a bundled installer carries: the package itself, so the installer works
    on a machine that has none of the tools and no internet connection.
    """
    spec = PREREQS[key]
    bundled = bundled_package(key)
    if bundled is not None:
        return bundled
    cache = prereq_dir()
    cache.mkdir(parents=True, exist_ok=True)
    archive = cache / spec["bundled"][0]
    if archive.exists() and sha256_of(archive) == spec["sha256"]:
        return archive
    log(f"  downloading {spec['name']} ({spec['size'] / 1048576:.0f} MB) ...")
    tmp = archive.with_suffix(archive.suffix + ".part")
    with urllib.request.urlopen(spec["url"], timeout=120) as response:
        total = int(response.headers.get("Content-Length") or 0)
        done = 0
        last = 0
        with tmp.open("wb") as out:
            while True:
                block = response.read(1 << 20)
                if not block:
                    break
                out.write(block)
                done += len(block)
                if total and done - last > (32 << 20):
                    last = done
                    log(f"    {done * 100 // total}%  "
                        f"({done / 1048576:.0f} / {total / 1048576:.0f} MB)")
    if sha256_of(tmp) != spec["sha256"]:
        tmp.unlink(missing_ok=True)
        log(f"  {spec['name']}: checksum mismatch, refusing to use it")
        return None
    tmp.replace(archive)
    log(f"  {spec['name']}: downloaded and verified")
    return archive


def fetch(key: str, allow_download: bool = True) -> Path | None:
    """Return the directory holding the tool, downloading it if need be."""
    spec = PREREQS[key]

    found = find_fetched(key)
    if found:
        log(f"  {spec['name']}: {found}")
        return found
    found = find_installed(key)
    if found:
        log(f"  {spec['name']}: {found} (already installed)")
        return found

    if not allow_download:
        log(f"  {spec['name']}: MISSING")
        return None

    dest = prereq_dir() / key
    archive = prereq_dir() / (spec["url"].rsplit("/", 1)[-1])
    archive.parent.mkdir(parents=True, exist_ok=True)

    bundled = bundled_package(key)
    if bundled is not None:
        log(f"  {spec['name']}: the bundled package")
        archive = bundled
    elif archive.exists() and sha256_of(archive) == spec["sha256"]:
        log(f"  {spec['name']}: using the downloaded package")
    else:
        log(f"  {spec['name']}: downloading "
            f"({spec['size'] / 1048576:.0f} MB) ...")
        tmp = archive.with_suffix(archive.suffix + ".part")
        with urllib.request.urlopen(spec["url"], timeout=120) as response:
            total = int(response.headers.get("Content-Length") or 0)
            done = 0
            last = 0
            with tmp.open("wb") as out:
                while True:
                    block = response.read(1 << 20)
                    if not block:
                        break
                    out.write(block)
                    done += len(block)
                    if total and done - last > (32 << 20):
                        last = done
                        log(f"    {done * 100 // total}%  "
                            f"({done / 1048576:.0f} / {total / 1048576:.0f} MB)")
        digest = sha256_of(tmp)
        if digest != spec["sha256"]:
            tmp.unlink(missing_ok=True)
            log(f"  {spec['name']}: checksum mismatch, refusing to use it")
            return None
        tmp.replace(archive)
        log(f"  {spec['name']}: downloaded and verified")
    if bundled is not None and sha256_of(archive) != spec["sha256"]:
        log(f"  {spec['name']}: the bundled package is damaged")
        return None

    unpack(archive, dest)
    home = find_home(dest, spec["marker"])
    if home is None:
        log(f"  {spec['name']}: the package did not contain {spec['marker']}")
        return None
    return home


def ensure_all(allow_download: bool = True) -> dict:
    """Make sure every tool is there; returns the ones that are missing."""
    out = {}
    for key in ("ghidra", "jdk", "mingw"):
        out[key] = fetch(key, allow_download)
    return out


if __name__ == "__main__":
    allow = "--no-download" not in sys.argv
    missing = [k for k, v in ensure_all(allow).items() if v is None]
    if missing:
        print("missing:", ", ".join(missing))
        sys.exit(1)
    print("all prerequisites are in place")
