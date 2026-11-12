#!/usr/bin/env python3
"""Find code addresses materialized inside instructions (adr / add pc / ldr pc-literal).

Scanning data misses function pointers a compiler builds with `adr rX, func` and stores into a
table at runtime. This linearly disassembles .text, resolves every PC-relative code reference and
reports targets that no dumped function covers.

Usage:
    python3 tools/find_code_refs.py [target-hex]
"""
import importlib.util
import struct
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "recomp"))

import capstone
from capstone import arm as cs_arm


def load(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


def main() -> int:
    cov = load("coverage", ROOT / "recomp/coverage.py")
    data = (ROOT / "analysis/apk/lib/armeabi-v7a/libFast5.so").read_bytes()

    insn_addrs = set()
    for f in cov.iter_funcs(ROOT / "analysis/reports/pcode.tsv"):
        if f.insns:
            insn_addrs.update(i.addr for i in f.insns)

    text_start, text_end = 0x298B60, 0x7022D8
    focus = int(sys.argv[1], 16) if len(sys.argv) > 1 else None

    md = capstone.Cs(capstone.CS_ARCH_ARM, capstone.CS_MODE_ARM)
    md.detail = True
    refs = {}
    focus_sites = []

    for insn in md.disasm(data[text_start:text_end], text_start):
        target = None
        m = insn.mnemonic
        if m == "adr":
            target = insn.operands[1].imm
        elif m in ("add", "sub") and len(insn.operands) >= 3:
            ops = insn.operands
            if ops[0].type == cs_arm.ARM_OP_REG and ops[1].type == cs_arm.ARM_OP_REG \
               and ops[1].reg == cs_arm.ARM_REG_PC and ops[2].type == cs_arm.ARM_OP_IMM:
                target = insn.address + 8 + (ops[2].imm if m == "add" else -ops[2].imm)
        elif m == "ldr" and len(insn.operands) >= 2:
            ops = insn.operands
            if ops[1].type == cs_arm.ARM_OP_MEM and ops[1].mem.base == cs_arm.ARM_REG_PC:
                lit = insn.address + 8 + ops[1].mem.disp
                if 0 <= lit <= len(data) - 4:
                    v = struct.unpack_from("<I", data, lit)[0]
                    if text_start <= v < text_end:
                        target = v
        if target is None:
            continue
        if focus is not None and target == focus:
            focus_sites.append((insn.address, insn.mnemonic, insn.op_str, target))
        if text_start <= target < text_end:
            refs.setdefault(target, []).append(insn.address)

    missed = {t: s for t, s in refs.items() if t not in insn_addrs}
    print(f"known instructions: {len(insn_addrs)}")
    print(f"code refs found: {len(refs)}  targets not covered: {len(missed)}")
    if focus is not None:
        print(f"refs to 0x{focus:06x}: {len(focus_sites)}")
        for site in focus_sites[:20]:
            print(f"  at 0x{site[0]:06x}: {site[1]} {site[2]} -> 0x{site[3]:06x}")
    for t in sorted(missed)[:60]:
        sites = missed[t]
        print(f"  0x{t:06x}  refs={len(sites)}  first=0x{sites[0]:06x}")

    out = ROOT / "analysis/notes/code-refs.txt"
    out.write_text("\n".join(f"0x{t:06x}" for t in sorted(missed)) + "\n")
    print(f"wrote {out}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
