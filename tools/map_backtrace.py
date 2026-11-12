#!/usr/bin/env python3
"""Resolve the game-thread backtrace printed by guest_fault to translated functions."""
import re
import subprocess
import sys
from pathlib import Path

root = Path(__file__).resolve().parent.parent
log = Path(sys.argv[1] if len(sys.argv) > 1 else "build/win_fault2.err")
text = log.read_text(errors="replace")
m = re.search(r"exe_base=0x([0-9a-fA-F]+)", text)
if not m:
    raise SystemExit("exe_base not found in log")
base = int(m.group(1), 16)
addrs = [int(x, 16) for x in re.findall(r"^bt ([0-9a-fA-F]+)", text, re.M)]
preferred = 0x140000000
vas = [hex(preferred + (a - base)) for a in addrs]
out = subprocess.run(
    ["addr2line", "-e", str(root / "build/fast5_win.exe"), "-f", "-C", *vas],
    capture_output=True, text=True,
).stdout.splitlines()
for i, addr in enumerate(addrs):
    fn = out[2 * i] if 2 * i < len(out) else "?"
    loc = out[2 * i + 1] if 2 * i + 1 < len(out) else "?"
    print(f"{i:2d}: bt=0x{addr:x} off=0x{addr - base:x} -> {fn} | {loc}")
