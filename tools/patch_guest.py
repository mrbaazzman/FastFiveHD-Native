#!/usr/bin/env python3
"""Build build/guest.bin and resolve the Android libc data symbols the library imports.

Undefined data symbols (GLOB_DAT/ABS32) cannot be resolved against the host libc, so each one is
pointed at a runtime-provided cell. Kept separate from link_all.py so the image can be rebuilt
without recompiling the translation units.
"""
import struct
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "recomp"))
from elf_image import Image  # noqa: E402

SYMBOLS = {
    "_ctype_": 0x9A1000,
    "_tolower_tab_": 0x9A1200,
    "_toupper_tab_": 0x9A1280,
}


def main() -> int:
    img = Image(ROOT / "analysis/apk/lib/armeabi-v7a/libFast5.so")
    img.apply_relocations()
    patched = 0
    for off, name, _rtype in getattr(img, "undefined_relocs", []):
        if name in SYMBOLS:
            struct.pack_into("<I", img.mem, off, SYMBOLS[name])
            patched += 1
    out = ROOT / "build/guest.bin"
    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_bytes(bytes(img.mem))
    print(f"patched {patched} data symbol slot(s)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
