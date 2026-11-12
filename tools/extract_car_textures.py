#!/usr/bin/env python3
"""Extract car TGA payloads by basename into the runtime overlay.

carTextures.zip stores images under Data/#temp/CarTga/, while the car material
requests /Highline_... names. The VFS overlay makes the basenames visible to
the engine's normal folder search without changing the archive.
"""
from pathlib import Path
import struct
import zlib

ROOT = Path(__file__).resolve().parent.parent
SRC = ROOT / "GloftF5HP" / "res" / "file000591.dat"
OUT = ROOT / "build" / "overlay"


def records(path):
    data = path.read_bytes()
    pos = data.find(b"PK\x03\x04")
    while pos >= 0 and data[pos:pos + 4] == b"PK\x03\x04":
        _ver, _flags, method = struct.unpack_from("<HHH", data, pos + 4)
        _crc, csize, usize = struct.unpack_from("<III", data, pos + 14)
        nlen, elen = struct.unpack_from("<HH", data, pos + 26)
        name = data[pos + 30:pos + 30 + nlen].decode("latin-1")
        start = pos + 30 + nlen + elen
        payload = data[start:start + csize]
        if method == 8:
            payload = zlib.decompress(payload, -15)
        elif method != 0:
            raise SystemExit(f"unsupported method {method}")
        if len(payload) != usize:
            raise SystemExit(f"size mismatch {name}")
        yield name, payload
        pos = start + csize


def main() -> int:
    written = 0
    for name, payload in records(SRC):
        low = name.lower()
        if not (low.endswith(".tga") or low.endswith(".pvr")):
            continue
        base = name.rsplit("/", 1)[-1]
        if not base:
            continue
        dst = OUT / base
        dst.parent.mkdir(parents=True, exist_ok=True)
        dst.write_bytes(payload)
        written += 1
    print(f"wrote {written} car texture basenames to {OUT}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
