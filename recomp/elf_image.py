#!/usr/bin/env python3
"""ELF32/ARM image loader and relocation verifier for the Fast Five HD port.

This is the reference model for the runtime's C image loader. It maps both PT_LOAD
segments at their link addresses (the library is non-PIE), parses the dynamic section,
applies every relocation, and reports exactly what a static port must satisfy.

Relocation handling:
  R_ARM_NONE (0)       ignored
  R_ARM_RELATIVE (23)  *(addr) += load_bias        (0 for link-address mapping)
  R_ARM_ABS32 (2)      *(addr) = S + A
  R_ARM_GLOB_DAT (21)  *(addr) = S
  R_ARM_JUMP_SLOT (22) *(addr) = S
  R_ARM_COPY (20)      *(addr) = S                 (not expected in a .so)

Undefined symbols are reported; they are the runtime's responsibility.

Usage:
    python3 recomp/elf_image.py analysis/apk/lib/armeabi-v7a/libFast5.so
"""
import struct
import sys
from collections import Counter
from pathlib import Path

PT_LOAD = 1
PT_DYNAMIC = 2

DT_HASH = 4
DT_NEEDED = 1
DT_STRTAB = 5
DT_SYMTAB = 6
DT_STRSZ = 10
DT_SONAME = 14
DT_REL = 17
DT_RELSZ = 18
DT_RELENT = 19
DT_PLTREL = 20
DT_JMPREL = 23
DT_PLTRELSZ = 2
DT_INIT_ARRAY = 25
DT_FINI_ARRAY = 26
DT_INIT_ARRAYSZ = 27
DT_FINI_ARRAYSZ = 28

R_ARM_NONE = 0
R_ARM_ABS32 = 2
R_ARM_COPY = 20
R_ARM_GLOB_DAT = 21
R_ARM_JUMP_SLOT = 22
R_ARM_RELATIVE = 23

RELOC_NAMES = {
    R_ARM_NONE: "R_ARM_NONE",
    R_ARM_ABS32: "R_ARM_ABS32",
    R_ARM_COPY: "R_ARM_COPY",
    R_ARM_GLOB_DAT: "R_ARM_GLOB_DAT",
    R_ARM_JUMP_SLOT: "R_ARM_JUMP_SLOT",
    R_ARM_RELATIVE: "R_ARM_RELATIVE",
}


class Image:
    def __init__(self, path: Path):
        self.path = path
        self.data = path.read_bytes()
        if self.data[:4] != b"\x7fELF" or self.data[4] != 1:
            raise SystemExit("not an ELF32 file")
        hdr = struct.unpack_from("<HHIIIIIHHHHHH", self.data, 16)
        (self.e_type, self.e_machine, self.e_version, self.e_entry, self.e_phoff,
         self.e_shoff, self.e_flags, self.e_ehsize, self.e_phentsize,
         self.e_phnum, self.e_shentsize, self.e_shnum, self.e_shstrndx) = hdr

        self.loads = []
        self.dynamic = None
        for i in range(self.e_phnum):
            off = self.e_phoff + i * self.e_phentsize
            (p_type, p_offset, p_vaddr, p_paddr, p_filesz, p_memsz,
             p_flags, p_align) = struct.unpack_from("<IIIIIIII", self.data, off)
            if p_type == PT_LOAD:
                self.loads.append((p_vaddr, p_offset, p_filesz, p_memsz))
            elif p_type == PT_DYNAMIC:
                self.dynamic = (p_offset, p_filesz)

        self.image_end = max(v + m for v, _o, _f, m in self.loads)
        self.mem = bytearray(self.image_end)
        for vaddr, offset, filesz, _memsz in self.loads:
            self.mem[vaddr:vaddr + filesz] = self.data[offset:offset + filesz]

        self.dyn = {}
        self.dyn_order = []
        if self.dynamic:
            off, size = self.dynamic
            for i in range(size // 8):
                tag, val = struct.unpack_from("<II", self.data, off + i * 8)
                if tag == 0:
                    break
                self.dyn.setdefault(tag, val)
                self.dyn_order.append((tag, val))

        self.syms = []
        self.strtab = b""
        self._parse_symbols()
        self.relocations = []

    def _parse_symbols(self):
        symtab = self.dyn.get(DT_SYMTAB)
        strtab = self.dyn.get(DT_STRTAB)
        strsz = self.dyn.get(DT_STRSZ, 0)
        if not symtab or not strtab:
            return
        self.strtab = bytes(self.mem[strtab:strtab + strsz])
        count = None
        hash_addr = self.dyn.get(DT_HASH)
        if hash_addr:
            _nbucket, nchain = struct.unpack_from("<II", self.mem, hash_addr)
            count = nchain
        if count is None:
            # Fall back to the section table if the hash table is absent.
            shstr = self.e_shoff + self.e_shstrndx * self.e_shentsize
            _n, _t, _f, _a, _o, _s, _l, _i, _al, _e = struct.unpack_from(
                "<IIIIIIIIII", self.data, shstr)
            names = self.data[_o:_o + _s]
            for i in range(self.e_shnum):
                sh = self.e_shoff + i * self.e_shentsize
                name_off, _typ, _flags, _addr, _off, size, _link, _info, _align, _ent = \
                    struct.unpack_from("<IIIIIIIIII", self.data, sh)
                end = names.find(b"\0", name_off)
                if names[name_off:end] == b".dynsym":
                    count = size // 16
                    break
        if count is None:
            count = (strtab - symtab) // 16
        for i in range(count):
            off = symtab + i * 16
            if off + 16 > len(self.mem):
                break
            name_off, value, size, info, other, shndx = struct.unpack_from(
                "<IIIBBH", self.mem, off)
            end = self.strtab.find(b"\0", name_off)
            name = self.strtab[name_off:end].decode("latin-1", "replace")
            self.syms.append(dict(name=name, value=value, size=size, info=info,
                                  other=other, shndx=shndx))

    def reloc_sections(self):
        out = []
        if DT_REL in self.dyn and DT_RELSZ in self.dyn:
            out.append((self.dyn[DT_REL], self.dyn[DT_RELSZ], "JMPREL" if False else "REL"))
        if DT_JMPREL in self.dyn and DT_PLTRELSZ in self.dyn:
            out.append((self.dyn[DT_JMPREL], self.dyn[DT_PLTRELSZ], "JMPREL"))
        return out

    def apply_relocations(self, base=0):
        counts = Counter()
        unresolved = Counter()
        bad = []
        self.undefined_relocs = []
        for addr, size, kind in self.reloc_sections():
            for i in range(size // 8):
                off = addr + i * 8
                r_offset, r_info = struct.unpack_from("<II", self.mem, off)
                sym_index = r_info >> 8
                r_type = r_info & 0xFF
                counts[r_type] += 1
                if r_type == R_ARM_RELATIVE:
                    cur = struct.unpack_from("<I", self.mem, r_offset)[0]
                    struct.pack_into("<I", self.mem, r_offset, (cur + base) & 0xFFFFFFFF)
                elif r_type in (R_ARM_ABS32, R_ARM_GLOB_DAT, R_ARM_JUMP_SLOT, R_ARM_COPY):
                    if sym_index == 0 or sym_index >= len(self.syms):
                        bad.append((kind, r_offset, r_type, sym_index))
                        continue
                    sym = self.syms[sym_index]
                    if sym["shndx"] == 0:
                        unresolved[sym["name"]] += 1
                        self.undefined_relocs.append((r_offset, sym["name"], r_type))
                        continue
                    value = (sym["value"] + base) & 0xFFFFFFFF
                    if r_type == R_ARM_ABS32:
                        addend = struct.unpack_from("<I", self.mem, r_offset)[0]
                        value = (value + addend) & 0xFFFFFFFF
                    struct.pack_into("<I", self.mem, r_offset, value)
                elif r_type != R_ARM_NONE:
                    bad.append((kind, r_offset, r_type, sym_index))
        return counts, unresolved, bad

    def init_array(self):
        addr = self.dyn.get(DT_INIT_ARRAY)
        size = self.dyn.get(DT_INIT_ARRAYSZ, 0)
        if not addr:
            return []
        return list(struct.unpack_from("<%dI" % (size // 4), self.mem, addr))


def main() -> int:
    path = Path(sys.argv[1] if len(sys.argv) > 1 else
                "analysis/apk/lib/armeabi-v7a/libFast5.so")
    img = Image(path)
    defined = [s for s in img.syms if s["shndx"] != 0 and s["name"]]
    undefined = [s for s in img.syms if s["shndx"] == 0 and s["name"]]
    funcs = [s for s in defined if (s["info"] & 0xF) == 2]

    print(f"file            : {path}")
    print(f"ELF type/machine: {img.e_type}/{img.e_machine}  entry=0x{img.e_entry:x}")
    print(f"LOAD segments   : {len(img.loads)}  image_end=0x{img.image_end:x} "
          f"({img.image_end:,} bytes)")
    for v, o, f, m in img.loads:
        print(f"  vaddr=0x{v:08x} offset=0x{o:08x} filesz={f:>9,} memsz={m:>9,}")
    print(f"dynamic entries : {len(img.dyn_order)}")
    print(f"symbols         : {len(img.syms):,} total, {len(defined):,} defined, "
          f"{len(undefined):,} undefined, {len(funcs):,} functions")

    counts, unresolved, bad = img.apply_relocations()
    total = sum(counts.values())
    print(f"relocations     : {total:,}")
    for t, c in sorted(counts.items()):
        print(f"  {RELOC_NAMES.get(t, t):16} {c:>6,}")
    print(f"unresolved syms : {len(unresolved)} names, {sum(unresolved.values()):,} slots")
    for name, c in unresolved.most_common(15):
        print(f"  {name:<40} {c}")
    if bad:
        print(f"UNHANDLED/BAD   : {len(bad)}")
        for row in bad[:10]:
            print(f"  {row}")

    init = img.init_array()
    print(f"init_array      : {len(init)} constructors")
    print(f"  first: {[hex(x) for x in init[:5]]}")
    print(f"  last : {[hex(x) for x in init[-3:]]}")

    ok = not bad and len(init) == 433
    print(f"\nloader status   : {'OK' if ok else 'REVIEW'}")
    return 0 if ok else 1


if __name__ == "__main__":
    raise SystemExit(main())
