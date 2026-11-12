#!/usr/bin/env python3
"""Parse the decoded obfuscation map produced by tools/emulate_map.py."""
import sys
from collections import Counter
from pathlib import Path


def main() -> int:
    raw = Path(sys.argv[1] if len(sys.argv) > 1 else "analysis/reports/dat-map.raw").read_bytes()
    marker = b"c|-|3k"
    body, _, _ = raw.partition(marker)
    text = body.replace(b"\r", b"")

    entries = []
    for line in text.split(b"\n"):
        if not line.strip(b"\x00"):
            continue
        parts = line.split(b":")
        if len(parts) < 2:
            entries.append((line.decode("latin-1"), ""))
            continue
        entries.append((parts[0].decode("latin-1"), ":".join(p.decode("latin-1") for p in parts[1:])))

    print(f"raw bytes        : {len(raw):,}")
    print(f"text bytes       : {len(text):,}")
    print(f"lines            : {len(entries):,}")

    exts = Counter()
    for name, _ in entries:
        if "." in name:
            exts[name.rsplit(".", 1)[1].lower()] += 1
    print("\ntop extensions:")
    for ext, count in exts.most_common(25):
        print(f"  {ext:<12} {count}")

    print("\nfirst 40 entries:")
    for name, value in entries[:40]:
        print(f"  {name}  ->  {value}")

    print("\nentries containing 'file':")
    n = 0
    for name, value in entries:
        if "file" in value.lower() or "file" in name.lower():
            print(f"  {name}  ->  {value}")
            n += 1
            if n >= 20:
                break

    out = Path("analysis/reports/dat-map.tsv")
    out.write_text("\n".join(f"{a}\t{b}" for a, b in entries) + "\n", encoding="utf-8")
    print(f"\nwrote {out}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
