#!/usr/bin/env python3
"""Instruction census for libFast5.so using Capstone.

Disassembles every function that has a non-zero size in .dynsym and reports the mix the
ARM->C emitter must support: mnemonics, condition codes, VFP/NEON, direct/indirect calls,
and which compiler/runtime helpers are actually invoked.

Usage: python3 tools/instruction_census.py <file.so> > analysis/reports/instruction-census.md
"""
import struct
import sys
from collections import Counter, defaultdict
from pathlib import Path

import capstone

try:
    from capstone.arm_const import ARM_CC_AL, ARM_CC_INVALID
except ImportError:  # capstone < 4.0
    from capstone.arm import ARM_CC_AL, ARM_CC_INVALID

CC_NAMES = ["INVALID", "EQ", "NE", "HS", "LO", "MI", "PL", "VS",
            "VC", "HI", "LS", "GE", "LT", "GT", "LE", "AL"]

STT_FUNC = 2
SHN_UNDEF = 0

VFP_ARITH = {"vadd", "vsub", "vmul", "vdiv", "vmla", "vmls", "vnmla", "vnmls",
             "vnmul", "vabs", "vneg", "vsqrt", "vcmp", "vcmpe", "vcvt", "vmov",
             "vmrs", "vmsr", "vldr", "vstr", "vpush", "vpop"}
NEON_HINT = ("vadd.", "vsub.", "vmul.", "vld1.", "vst1.", "vld2.", "vst2.",
             "vld3.", "vst3.", "vld4.", "vst4.", "vdup.", "vmovn.", "vmovl.",
             "vq", "vshl.", "vshr.", "vand.", "vorr.", "veor.", "vmin.", "vmax.")


def load_elf(path: Path):
    data = path.read_bytes()
    if data[:4] != b"\x7fELF" or data[4] != 1:
        raise SystemExit("expected an ELF32 file")
    (_type, _machine, _ver, entry, _phoff, shoff, _flags, _ehsize,
     _phentsize, _phnum, shentsize, shnum, shstrndx) = struct.unpack_from("<HHIIIIIHHHHHH", data, 16)
    sections = []
    for i in range(shnum):
        vals = struct.unpack_from("<IIIIIIIIII", data, shoff + i * shentsize)
        sections.append(dict(zip(
            ("name_off", "type", "flags", "addr", "offset", "size", "link", "info", "align", "entsize"), vals)))
    shstr = sections[shstrndx]
    raw = data[shstr["offset"]:shstr["offset"] + shstr["size"]]
    for s in sections:
        end = raw.find(b"\0", s["name_off"])
        s["name"] = raw[s["name_off"]:end].decode("latin-1")
    return data, sections, entry


def main() -> int:
    if len(sys.argv) != 2:
        print(__doc__, file=sys.stderr)
        return 2
    path = Path(sys.argv[1])
    data, sections, entry = load_elf(path)
    by_name = {s["name"]: s for s in sections}
    text = by_name[".text"]
    dynsym = by_name[".dynsym"]
    dynstr = sections[dynsym["link"]]
    strtab = data[dynstr["offset"]:dynstr["offset"] + dynstr["size"]]

    def name_at(off):
        end = strtab.find(b"\0", off)
        return strtab[off:end].decode("latin-1", "replace")

    functions = []
    for i in range(dynsym["size"] // 16):
        st_name, st_value, st_size, st_info, _other, st_shndx = struct.unpack_from(
            "<IIIBBH", data, dynsym["offset"] + i * 16)
        if st_shndx == SHN_UNDEF or (st_info & 0xF) != STT_FUNC or st_size == 0:
            continue
        functions.append((name_at(st_name), st_value & ~1, st_size))
    functions.sort(key=lambda f: f[1])

    md = capstone.Cs(capstone.CS_ARCH_ARM, capstone.CS_MODE_ARM)
    md.detail = True
    md.skipdata = False

    mnemonics = Counter()
    conds = Counter()
    groups = Counter()
    call_targets = Counter()
    funcs_with = Counter()
    per_func_counts = []
    total = 0
    direct_calls = indirect_calls = 0
    resolved_targets = Counter()
    unresolved_indirect = 0

    addr_to_name = {a: n for n, a, _s in functions}

    for name, addr, size in functions:
        off = addr - text["addr"] + text["offset"]
        blob = data[off:off + size]
        n_insn = 0
        local_flags = set()
        for insn in md.disasm(blob, addr):
            total += 1
            n_insn += 1
            mn = insn.mnemonic.split(".")[0]
            mnemonics[insn.mnemonic] += 1
            conds[insn.cc] += 1
            for g in insn.groups:
                groups[g] += 1
            if insn.mnemonic in VFP_ARITH or insn.mnemonic.startswith("v"):
                local_flags.add("vfp")
            if insn.mnemonic.startswith(NEON_HINT):
                local_flags.add("neon")
            if insn.cc not in (ARM_CC_AL, ARM_CC_INVALID):
                local_flags.add("conditional")
            if insn.mnemonic in ("bl", "blx"):
                if insn.operands and insn.operands[0].type == capstone.arm.ARM_OP_IMM:
                    direct_calls += 1
                    target = insn.operands[0].imm
                    resolved_targets[addr_to_name.get(target, f"sub_{target:x}")] += 1
                else:
                    indirect_calls += 1
            if insn.mnemonic == "bx" or insn.mnemonic == "ldr" and "pc" in insn.op_str:
                unresolved_indirect += 1
            if insn.mnemonic in ("svc", "mcr", "mrc", "mcrr", "mrrc"):
                local_flags.add("cp15")
            if insn.mnemonic in ("ldrex", "strex", "ldrexd", "strexd"):
                local_flags.add("atomics")
            if insn.mnemonic in ("push", "pop"):
                local_flags.add("stack")
        per_func_counts.append(n_insn)
        for f in local_flags:
            funcs_with[f] += 1

    per_func_counts.sort()
    n_funcs = len(per_func_counts) or 1

    print(f"# Instruction census — `{path.name}`\n")
    print(f"- Functions analysed: **{len(functions):,}**")
    print(f"- Instructions decoded: **{total:,}**")
    print(f"- Mean instructions/function: **{total // n_funcs:,}**, median **{per_func_counts[n_funcs//2]:,}**, max **{per_func_counts[-1]:,}**")
    print(f"- Entry point: `0x{entry:x}`\n")

    print("## Condition codes\n")
    print("| CC | Instructions |")
    print("|---|---|")
    for cc, count in conds.most_common():
        print(f"| {CC_NAMES[cc] if 0 <= cc < len(CC_NAMES) else cc} | {count:,} |")

    print("\n## Top 50 mnemonics\n")
    print("| Mnemonic | Count |")
    print("|---|---|")
    for m, c in mnemonics.most_common(50):
        print(f"| `{m}` | {c:,} |")

    print("\n## Categories\n")
    print("| Category | Instructions | Functions using it |")
    print("|---|---|---|")
    cats = {
        "VFP (any `v*`)": lambda m: m.startswith("v"),
        "NEON-like": lambda m: m.startswith(NEON_HINT),
        "branches `b/bl`": lambda m: m in ("b", "bl"),
        "indirect `bx/blx`": lambda m: m in ("bx", "blx"),
        "load/store `ldr/str`": lambda m: m.startswith(("ldr", "str")),
        "block `ldm/stm`": lambda m: m.startswith(("ldm", "stm")),
        "stack `push/pop`": lambda m: m in ("push", "pop"),
        "multiply `mul/mla/umull/smull`": lambda m: m.startswith(("mul", "mla", "umull", "smull", "smlal", "umlal")),
        "atomics `ldrex/strex`": lambda m: m.startswith(("ldrex", "strex")),
        "syscall `svc`": lambda m: m == "svc",
        "coprocessor `mcr/mrc`": lambda m: m in ("mcr", "mrc", "mcrr", "mrrc"),
        "saturating `q*`": lambda m: m.startswith("q"),
        "bitfield `ubfx/sbfx/bfi/bfc`": lambda m: m in ("ubfx", "sbfx", "bfi", "bfc"),
    }
    for label, pred in cats.items():
        c = sum(v for k, v in mnemonics.items() if pred(k.split(".")[0]) or pred(k))
        key = {"NEON-like": "neon", "VFP (any `v*`)": "vfp", "atomics `ldrex/strex`": "atomics",
               "syscall `svc`": "svc", "coprocessor `mcr/mrc`": "cp15"}.get(label, "")
        fu = funcs_with.get(key, 0) if key else ""
        print(f"| {label} | {c:,} | {fu if fu == '' else format(fu, ',')} |")

    print("\n## Calls\n")
    print("| Kind | Count |")
    print("|---|---|")
    print(f"| Direct `bl` (immediate) | {direct_calls:,} |")
    print(f"| Indirect `blx`/register | {indirect_calls:,} |")
    print(f"| `bx` / `ldr pc` sites | {unresolved_indirect:,} |")

    print("\n## Most-called internal symbols (direct calls)\n")
    print("| Symbol | Calls |")
    print("|---|---|")
    for n, c in resolved_targets.most_common(40):
        print(f"| `{n}` | {c:,} |")

    print("\n## Compiler / runtime helper calls\n")
    print("| Group | Direct calls |")
    print("|---|---|")
    for prefix in ("__aeabi_", "__cxa_", "__gxx_", "pthread_", "dl", "gl", "fopen", "fread",
                   "fwrite", "fclose", "malloc", "free", "memcpy", "memset", "snprintf",
                   "sqrt", "sin", "cos", "atan", "pow", "exp", "floor", "fmod",
                   "socket", "connect", "send", "recv", "abort", "exit"):
        c = sum(v for k, v in resolved_targets.items() if k.startswith(prefix))
        if c:
            print(f"| `{prefix}*` | {c:,} |")

    print("\n## Functions using 63 k+ byte bodies are excluded only from size buckets, not counts.\n")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
