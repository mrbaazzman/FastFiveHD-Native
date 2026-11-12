#!/usr/bin/env python3
"""ELF32/ARM census for the Fast Five HD recompilation effort.

Reads an ELF shared object and reports the facts the ARM->C emitter design depends on:

  * section map (address, size, role)
  * dynamic symbol census: functions, objects, ARM vs Thumb entry points
  * function size distribution and namespace prefixes
  * exception index (.ARM.exidx) row count
  * imported function census grouped by library

No third-party modules. Usage:

    python3 tools/elf_census.py <file.so> > analysis/reports/elf-census.md
"""
import struct
import sys
from collections import Counter, defaultdict
from pathlib import Path

STT_FUNC = 2
STT_OBJECT = 1
SHN_UNDEF = 0
ROLE = {
    ".text": "code",
    ".rodata": "read-only data",
    ".ARM.extab": "exception unwind tables",
    ".ARM.exidx": "exception index",
    ".init_array": "global constructors",
    ".fini_array": "global destructors",
    ".data.rel.ro": "vtables / RTTI",
    ".got": "global offset table",
    ".data": "initialised globals",
    ".bss": "zero-initialised globals",
    ".dynsym": "dynamic symbols",
    ".dynstr": "dynamic strings",
    ".rel.dyn": "relocations",
    ".rel.plt": "PLT relocations",
    ".plt": "PLT stubs",
    ".hash": "symbol hash",
}


def demangle_prefix(name: str, depth: int = 3) -> str:
    """Best-effort namespace prefix from an Itanium mangled name, for grouping only."""
    if not name.startswith("_Z"):
        return "(c symbols)"
    body = name[2:]
    if body.startswith("N"):
        body = body[1:]
    parts, i = [], 0
    while i < len(body) and len(parts) < depth:
        if body[i].isdigit():
            j = i
            while j < len(body) and body[j].isdigit():
                j += 1
            n = int(body[i:j])
            parts.append(body[j:j + n])
            i = j + n
        elif body[i] in "St":
            parts.append("std" if body[i] == "S" else "std")
            i += 1
        else:
            break
    return "::".join(parts) if parts else "(mangled)"


def main() -> int:
    if len(sys.argv) != 2:
        print(__doc__, file=sys.stderr)
        return 2
    path = Path(sys.argv[1])
    data = path.read_bytes()

    if data[:4] != b"\x7fELF":
        print("not an ELF file", file=sys.stderr)
        return 1
    if data[4] != 1:
        print("only ELF32 is supported", file=sys.stderr)
        return 1
    if data[5] != 1:
        print("only little-endian is supported", file=sys.stderr)
        return 1
    (e_type, e_machine, _ver, e_entry, _phoff, e_shoff, e_flags, _ehsize,
     _phentsize, _phnum, e_shentsize, e_shnum, e_shstrndx) = struct.unpack_from(
        "<HHIIIIIHHHHHH", data, 16)

    sections = []
    for i in range(e_shnum):
        off = e_shoff + i * e_shentsize
        (sh_name, sh_type, sh_flags, sh_addr, sh_offset, sh_size,
         sh_link, sh_info, sh_addralign, sh_entsize) = struct.unpack_from("<IIIIIIIIII", data, off)
        sections.append(dict(name_off=sh_name, type=sh_type, flags=sh_flags, addr=sh_addr,
                             offset=sh_offset, size=sh_size, link=sh_link, info=sh_info,
                             align=sh_addralign, entsize=sh_entsize))

    shstr = sections[e_shstrndx]
    shstr_data = data[shstr["offset"]:shstr["offset"] + shstr["size"]]
    for s in sections:
        end = shstr_data.find(b"\0", s["name_off"])
        s["name"] = shstr_data[s["name_off"]:end].decode("latin-1")

    by_name = {s["name"]: s for s in sections}
    dynsym = by_name[".dynsym"]
    dynstr = sections[dynsym["link"]]
    dynstr_data = data[dynstr["offset"]:dynstr["offset"] + dynstr["size"]]

    def sym_name(off: int) -> str:
        end = dynstr_data.find(b"\0", off)
        return dynstr_data[off:end].decode("latin-1", "replace")

    funcs, objects, undefined = [], [], []
    for i in range(dynsym["size"] // 16):
        (st_name, st_value, st_size, st_info, st_other, st_shndx) = struct.unpack_from(
            "<IIIBBH", data, dynsym["offset"] + i * 16)
        name = sym_name(st_name)
        typ = st_info & 0xF
        if st_shndx == SHN_UNDEF:
            undefined.append((name, typ))
            continue
        if typ == STT_FUNC:
            funcs.append((name, st_value, st_size))
        elif typ == STT_OBJECT:
            objects.append((name, st_value, st_size))

    thumb = [f for f in funcs if f[1] & 1]
    arm = [f for f in funcs if not (f[1] & 1)]
    sized = [f for f in funcs if f[2] > 0]
    zero = [f for f in funcs if f[2] == 0]
    sizes = sorted((f[2] for f in sized), reverse=True)
    total = sum(sizes) or 1

    buckets = Counter()
    for sz in sizes:
        if sz <= 16:
            buckets["<=16"] += 1
        elif sz <= 64:
            buckets["17-64"] += 1
        elif sz <= 256:
            buckets["65-256"] += 1
        elif sz <= 1024:
            buckets["257-1024"] += 1
        elif sz <= 4096:
            buckets["1025-4096"] += 1
        else:
            buckets[">4096"] += 1

    ns = Counter(demangle_prefix(f[0]) for f in funcs)

    print(f"# ELF census — `{path.name}`\n")
    print(f"- SHA-256 reference: see `docs/01-TARGET.md`")
    print(f"- ELF type `{e_type}`, machine `{e_machine}`, entry `0x{e_entry:x}`, flags `0x{e_flags:x}`\n")

    print("## Sections\n")
    print("| Section | Address | Size | Role |")
    print("|---|---|---|---|")
    for s in sections:
        if s["size"] == 0:
            continue
        print(f"| `{s['name']}` | `0x{s['addr']:x}` | {s['size']:,} | {ROLE.get(s['name'], '')} |")

    print("\n## Dynamic symbols\n")
    print("| Kind | Count |")
    print("|---|---|")
    print(f"| Functions (defined) | {len(funcs):,} |")
    print(f"| — ARM entry points (even) | {len(arm):,} |")
    print(f"| — Thumb entry points (odd) | {len(thumb):,} |")
    print(f"| — with non-zero size | {len(sized):,} |")
    print(f"| — zero-size (aliases/thunks) | {len(zero):,} |")
    print(f"| Objects (defined) | {len(objects):,} |")
    print(f"| Undefined symbols | {len(undefined):,} |")

    if sized:
        print("\n## Function sizes (non-zero)\n")
        print(f"- total {total:,} bytes, mean {total // len(sized):,} B, median {sizes[len(sizes)//2]:,} B")
        print(f"- largest {sizes[0]:,} B\n")
        print("| Size bucket (bytes) | Functions | Bytes |")
        print("|---|---|---|")
        for key in ("<=16", "17-64", "65-256", "257-1024", "1025-4096", ">4096"):
            rows = [s for s in sizes if (s <= 16 if key == "<=16" else
                                         17 <= s <= 64 if key == "17-64" else
                                         65 <= s <= 256 if key == "65-256" else
                                         257 <= s <= 1024 if key == "257-1024" else
                                         1025 <= s <= 4096 if key == "1025-4096" else s > 4096)]
            print(f"| {key} | {len(rows):,} | {sum(rows):,} |")

    print("\n## Namespace groups (top 25)\n")
    print("| Namespace | Functions |")
    print("|---|---|")
    for name, count in ns.most_common(25):
        print(f"| `{name}` | {count:,} |")

    exidx = by_name.get(".ARM.exidx")
    extab = by_name.get(".ARM.extab")
    init = by_name.get(".init_array")
    print("\n## Runtime structures\n")
    print("| Structure | Count / size |")
    print("|---|---|")
    if exidx:
        print(f"| `.ARM.exidx` rows | {exidx['size'] // 8:,} |")
    if extab:
        print(f"| `.ARM.extab` size | {extab['size']:,} B |")
    if init:
        print(f"| `.init_array` constructors | {init['size'] // 4:,} |")

    print("\n## Largest functions\n")
    print("| Address | Size | Symbol |")
    print("|---|---|---|")
    for name, value, size in sorted(sized, key=lambda f: f[2], reverse=True)[:20]:
        print(f"| `0x{value & ~1:x}` | {size:,} | `{name}` |")

    print("\n## Undefined symbols by prefix\n")
    pref = Counter()
    for name, _t in undefined:
        key = name.split("@")[0]
        pref[key[:3] if key.startswith("__") else key.split("_")[0] or key] += 1
    print("| Prefix | Count |")
    print("|---|---|")
    for name, count in pref.most_common(15):
        print(f"| `{name}` | {count:,} |")

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
