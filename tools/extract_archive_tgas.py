#!/usr/bin/env python3
"""Extract selected TGA records from a shipped custom ZIP container.

The .dat archives carry a small container prefix followed by normal ZIP local
records. Their central directory is not suitable for stock readers, so this
tool walks the local records and writes only the requested paths into the
derived runtime overlay.
"""
from pathlib import Path
import struct
import sys
import zlib

ROOT = Path(__file__).resolve().parent.parent
SRC = ROOT / "GloftF5HP" / "res" / "file000145.dat"
OUT = ROOT / "build" / "overlay"


def records(path):
    data = path.read_bytes()
    pos = data.find(b"PK\x03\x04")
    if pos < 0:
        raise SystemExit(f"{path}: no local records")
    while data[pos:pos + 4] == b"PK\x03\x04":
        _ver, _flags, method = struct.unpack_from("<HHH", data, pos + 4)
        _crc, csize, usize = struct.unpack_from("<III", data, pos + 14)
        nlen, elen = struct.unpack_from("<HH", data, pos + 26)
        name = data[pos + 30:pos + 30 + nlen].decode("latin-1")
        start = pos + 30 + nlen + elen
        payload = data[start:start + csize]
        if method == 8:
            payload = zlib.decompress(payload, -15)
        elif method != 0:
            raise SystemExit(f"{path}: unsupported ZIP method {method}")
        if len(payload) != usize:
            raise SystemExit(f"{path}: size mismatch for {name}")
        yield name, payload
        pos = start + csize


def main() -> int:
    written = 0
    pvr = 0
    for name, payload in records(SRC):
        if not name.lower().endswith(".tga"):
            continue
        dst = OUT / name
        dst.parent.mkdir(parents=True, exist_ok=True)
        dst.write_bytes(payload)
        written += 1
        alias = OUT / (name[:-4] + ".pvr")
        alias.write_bytes(payload)
        pvr += 1
    print(f"wrote {written} TGA files and {pvr} PVR payloads under {OUT}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
