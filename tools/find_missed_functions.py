#!/usr/bin/env python3
"""Find code pointers in the image that Ghidra did not turn into functions.

Ghidra finds functions from symbols, direct calls and some pointer tables. Functions reached
only through a table it did not recognize are left as raw bytes inside no function, so the
translator has no body for them. This scans every aligned word in the loaded image for a value
inside .text that is not covered by a dumped function and not a PLT stub.

Usage:
    python3 tools/find_missed_functions.py [pcode.tsv]
"""
import importlib.util
import struct
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "recomp"))


def load(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


def main() -> int:
    tsv = Path(sys.argv[1]) if len(sys.argv) > 1 else ROOT / "analysis/reports/pcode.tsv"
    cov = load("coverage", ROOT / "recomp/coverage.py")
    elf = load("elf_image", ROOT / "recomp/elf_image.py")

    img = elf.Image(ROOT / "analysis/apk/lib/armeabi-v7a/libFast5.so")
    img.apply_relocations()

    ranges = []
    insn_addrs = set()
    for f in cov.iter_funcs(tsv):
        if f.insns:
            ranges.append((f.entry, f.entry + max(f.size, 4), f.name))
            insn_addrs.update(i.addr for i in f.insns)
    ranges.sort()
    starts = [r[0] for r in ranges]

    text_start = 0x298B60
    text_end = 0x7022D8            # .rodata start; .text ends before it
    plt = (0x297D4C, 0x298B5C)

    import bisect
    def covered(addr):
        if addr in insn_addrs:
            return True
        i = bisect.bisect_right(starts, addr) - 1
        return i >= 0 and ranges[i][0] <= addr < ranges[i][1]

    candidates = {}
    for off in range(0, len(img.mem) - 4, 4):
        val = struct.unpack_from("<I", img.mem, off)[0]
        if not (text_start <= val < text_end):
            continue
        if plt[0] <= val < plt[1]:
            continue
        if covered(val):
            continue
        candidates.setdefault(val, []).append(off)

    import capstone
    md = capstone.Cs(capstone.CS_ARCH_ARM, capstone.CS_MODE_ARM)

    def is_entry(insn):
        if insn is None:
            return False
        m, o = insn.mnemonic, insn.op_str
        if m in ("push", "stmdb") and "lr" in o:
            return True
        if m == "str" and o.startswith("lr, [sp"):
            return True
        if m == "sub" and o.startswith("sp, sp"):
            return True
        if m == "mov" and o.startswith("ip, sp"):
            return True
        return False

    entries = {}
    for val in candidates:
        insn = next(md.disasm(img.mem[val:val + 4], val), None)
        if is_entry(insn):
            entries[val] = insn

    print(f"known functions: {len(ranges)}  known instructions: {len(insn_addrs)}")
    print(f"missed code pointers: {len(candidates)}  prologue entries: {len(entries)}")
    print("--- first 60 prologue entries ---")
    for val in sorted(entries)[:60]:
        insn = entries[val]
        print(f"  0x{val:06x}  refs={len(candidates[val])}  {insn.mnemonic} {insn.op_str}")

    out = ROOT / "analysis/notes/missed-functions.txt"
    out.write_text("\n".join(f"0x{v:06x}" for v in sorted(entries)) + "\n")
    print(f"wrote {out} ({len(entries)} entries)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
