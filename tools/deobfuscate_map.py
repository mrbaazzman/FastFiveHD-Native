#!/usr/bin/env python3
"""Decode the Glitch engine obfuscation map (file000000.dat).

Transcribed instruction-by-instruction from
glitch::io::CFileSystem::addObfuscationFileMap (ELF 0x6297dc). The loop is signed:
the byte is sign-extended, the key is subtracted, and 0x100 is added only once, so the
intermediate value can still be negative. The byte stored/written is the low 8 bits, but
the division/multiplication below uses the signed value.

    x    = sext(src[i]) - key; if x < 0: x += 0x100
    q2   = trunc(x / 2)
    bit  = (x & 1) + (1 if x < 0 else 0)
    rem  = c_mod(q2 * bit, key)
    dst[i] = x & 0xff
    hist[i % 3] = dst[i]
    sum  = sext(hist[0]) + sext(hist[1]) + sext(hist[2])
    key  = c_mod(key + trunc(sum / 3) + ((i % 3) - 1) * rem, 0x100)
           (negative -> + 0x100)

A valid map ends with the marker "c|-|3k"; the engine rejects it otherwise.

Usage:
    python3 tools/deobfuscate_map.py <file000000.dat> [--key 0x4f] [--out ...]
"""
import argparse
import sys
from pathlib import Path


def sext(b: int) -> int:
    return b - 256 if b >= 128 else b


def trunc_div(a: int, b: int) -> int:
    q = abs(a) // abs(b)
    return -q if (a < 0) != (b < 0) else q


def c_mod(a: int, b: int) -> int:
    return a - trunc_div(a, b) * b


def decode(data: bytes, key: int) -> bytes:
    out = bytearray(data)
    hist = [0, 0, 0]
    k = key
    divisor = key          # [sp+4] reloaded each iteration: always the original param_2
    for i in range(len(out)):
        x = sext(out[i]) - k
        if x < 0:
            x += 0x100
        adj = x + (1 if x < 0 else 0)          # add r0, r8, r8, lsr #31
        q2 = trunc_div(adj, 2)                  # mov r0, r0, asr #1
        bit = (adj & 1) - (1 if x < 0 else 0)   # and r2,r0,#1 ; rsb r2,r3,r2
        rem = c_mod(q2 * bit, divisor) if divisor else 0    # __aeabi_idivmod(q2*bit, param_2) -> r1
        stored = x & 0xFF
        out[i] = stored
        hist[i % 3] = stored
        total = sext(hist[0]) + sext(hist[1]) + sext(hist[2])
        nk = k + trunc_div(total, 3) + ((i % 3) - 1) * rem
        nk = c_mod(nk, 0x100)
        if nk < 0:
            nk += 0x100
        k = nk
    return bytes(out)


def parse_map(decoded: bytes):
    text = decoded.split(b"c|-|3k", 1)[0]
    entries = []
    for line in text.replace(b"\r", b"").split(b"\n"):
        if b":" not in line:
            continue
        name, _, value = line.partition(b":")
        if name:
            entries.append((name.decode("latin-1"), value.decode("latin-1")))
    return entries


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("path")
    ap.add_argument("--key", default="0x4f")
    ap.add_argument("--out")
    ap.add_argument("--limit", type=int, default=30)
    ap.add_argument("--all-keys", action="store_true", help="search every key for the marker")
    args = ap.parse_args()

    data = Path(args.path).read_bytes()

    if args.all_keys:
        hits = []
        for k in range(256):
            d = decode(data, k)
            if d.rstrip(b"\x00")[-6:] == b"c|-|3k" or b"c|-|3k" in d:
                hits.append(k)
        print(f"keys producing the marker: {[hex(k) for k in hits]}")
        if not hits:
            return 1
        key = hits[0]
    else:
        key = int(args.key, 0)

    decoded = decode(data, key)
    stripped = decoded.rstrip(b"\x00")
    marker = stripped[-6:]
    ok = marker == b"c|-|3k"
    print(f"file       : {args.path}")
    print(f"size       : {len(data):,} bytes")
    print(f"key        : 0x{key:02x}")
    print(f"trailer    : {marker!r}")
    print(f"marker ok  : {ok}")
    if not ok:
        return 1

    entries = parse_map(decoded)
    print(f"entries    : {len(entries):,}")
    for name, value in entries[: args.limit]:
        print(f"  {name}  ->  {value}")
    if args.out:
        out = Path(args.out)
        out.parent.mkdir(parents=True, exist_ok=True)
        out.write_text("\n".join(f"{n}\t{v}" for n, v in entries) + "\n", encoding="utf-8")
        print(f"wrote {out}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
