import importlib.util
import struct
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent.parent
spec = importlib.util.spec_from_file_location("elf_image", ROOT / "recomp/elf_image.py")
e = importlib.util.module_from_spec(spec)
spec.loader.exec_module(e)

img = e.Image(ROOT / "analysis/apk/lib/armeabi-v7a/libFast5.so")

for line in (ROOT / "runtime/imports_table.h").read_text().splitlines():
    if "0x298454" in line:
        print("PLT 0x298454 ->", line.strip())

for addr, size, kind in img.reloc_sections():
    for i in range(size // 8):
        off = addr + i * 8
        r_offset, r_info = struct.unpack_from("<II", img.mem, off)
        if r_offset == 0x7E65CC:
            sym = img.syms[r_info >> 8]
            print("reloc at 0x7e65cc", kind, "type", r_info & 0xFF,
                  "sym", repr(sym["name"]), "shndx", sym["shndx"])

img.apply_relocations()
print("value at 0x7e65cc:", hex(struct.unpack_from("<I", img.mem, 0x7E65CC)[0]))
print("bytes at 0x70373c:", img.mem[0x70373C:0x70375C])
