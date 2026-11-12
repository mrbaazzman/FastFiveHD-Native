import sys
from pathlib import Path

path = Path(sys.argv[1] if len(sys.argv) > 1 else "build/boot.log")
needle = (sys.argv[2] if len(sys.argv) > 2 else "DBG tex loadTextureFromFile").encode()
before = int(sys.argv[3]) if len(sys.argv) > 3 else 35
after = int(sys.argv[4]) if len(sys.argv) > 4 else 15

lines = path.read_bytes().split(b"\n")
idx = [i for i, line in enumerate(lines) if needle in line]
if not idx:
    print("not found")
    raise SystemExit(1)
i = idx[-1]
print(f"line {i + 1} of {len(lines)}")
for j in range(max(0, i - before), min(len(lines), i + after)):
    print(f"{j + 1}: {lines[j][:240]!r}")
