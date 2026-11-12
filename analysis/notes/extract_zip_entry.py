import struct
import sys
from pathlib import Path

p = Path(sys.argv[1] if len(sys.argv) > 1 else "GloftF5HP/res/file000548.dat")
want = sys.argv[2] if len(sys.argv) > 2 else "universal_logo"
out_path = sys.argv[3] if len(sys.argv) > 3 else ""
data = p.read_bytes()
eocd = data.rfind(b"PK\x05\x06")
print("file size", len(data), "eocd", eocd)
cd_size, cd_off = struct.unpack_from("<II", data, eocd + 12)
print("cd_size", cd_size, "cd_off", cd_off)
for delta in (0, 38):
    pos = cd_off + delta
    if pos + 46 > len(data):
        continue
    if data[pos:pos + 4] != b"PK\x01\x02":
        continue
    print("central directory at", pos, "delta", delta)
    while data[pos:pos + 4] == b"PK\x01\x02":
        (method,) = struct.unpack_from("<H", data, pos + 10)
        (csize,) = struct.unpack_from("<I", data, pos + 20)
        (usize,) = struct.unpack_from("<I", data, pos + 24)
        nlen, elen, clen = struct.unpack_from("<HHH", data, pos + 28)
        (loff,) = struct.unpack_from("<I", data, pos + 42)
        name = data[pos + 46:pos + 46 + nlen].decode("latin-1")
        if want in name:
            print("entry", name, "method", method, "csize", csize, "usize", usize, "loff", loff)
            for ldelta in (0, 38):
                lp = loff + ldelta
                if data[lp:lp + 4] == b"PK\x03\x04":
                    ln, le = struct.unpack_from("<HH", data, lp + 26)
                    start = lp + 30 + ln + le
                    blob = data[start:start + csize]
                    print("  local at", lp, "data", start, "bytes", len(blob))
                    if method == 0:
                        out = blob
                    else:
                        import zlib
                        out = zlib.decompress(blob, -15)
                    Path("/tmp/logo.bsprite").write_bytes(out)
                    if out_path:
                        Path(out_path).parent.mkdir(parents=True, exist_ok=True)
                        Path(out_path).write_bytes(out)
                        print("  wrote", out_path)
                    print("  extracted", len(out), "bytes")
                    print("  hex", out[:64].hex())
                    print("  strings", [s for s in out.split(b"\0") if len(s) > 3][:10])
                    raise SystemExit
        pos += 46 + nlen + elen + clen
