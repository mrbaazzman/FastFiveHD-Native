import struct
import sys
from pathlib import Path

root = Path(sys.argv[1] if len(sys.argv) > 1 else "GloftF5HP/res")
needle = (sys.argv[2] if len(sys.argv) > 2 else "universal_logo").encode()
for p in sorted(root.glob("*.dat")):
    try:
        with p.open("rb") as f:
            f.seek(0, 2)
            size = f.tell()
            f.seek(max(0, size - 131072))
            tail = f.read()
    except OSError:
        continue
    eocd = tail.rfind(b"PK\x05\x06")
    if eocd < 0 or eocd + 22 > len(tail):
        continue
    cd_size, cd_off = struct.unpack_from("<II", tail, eocd + 12)
    base = size - len(tail)
    for delta in (0, 38):
        pos = cd_off + delta
        if pos + 46 > size:
            continue
        f = p.open("rb")
        f.seek(pos)
        head = f.read(46)
        if len(head) < 46 or head[:4] != b"PK\x01\x02":
            f.close()
            continue
        names = []
        while head[:4] == b"PK\x01\x02":
            nlen, elen, clen = struct.unpack_from("<HHH", head, 28)
            name = f.read(nlen).decode("latin-1")
            f.seek(elen + clen, 1)
            if needle.lower() in name.encode().lower():
                names.append(name)
            head = f.read(46)
        f.close()
        if names:
            print(p.name, "delta", delta, names)
        break
