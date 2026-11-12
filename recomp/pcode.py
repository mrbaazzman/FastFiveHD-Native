#!/usr/bin/env python3
"""Parser for the TSV produced by recomp/ghidra_scripts/DumpPcode.java."""
from dataclasses import dataclass, field
from pathlib import Path
from typing import List, Optional


@dataclass
class Varnode:
    space: str
    value: str
    size: int

    @property
    def is_const(self) -> bool:
        return self.space == "const"

    @property
    def is_reg(self) -> bool:
        return self.space == "reg"

    @property
    def is_unique(self) -> bool:
        return self.space == "uniq"

    @property
    def is_ram(self) -> bool:
        return self.space == "ram"

    def __str__(self) -> str:
        return f"{self.space}:{self.value}:{self.size}"


@dataclass
class Op:
    opcode: str
    out: Optional[Varnode]
    ins: List[Varnode]
    userop: Optional[str] = None


@dataclass
class Insn:
    addr: int
    length: int
    mnemonic: str
    ops: List[Op] = field(default_factory=list)


@dataclass
class Func:
    entry: int
    name: str
    size: int
    external: bool
    thunk: bool
    insns: List[Insn] = field(default_factory=list)

    @property
    def addr_to_insn(self):
        return {i.addr: i for i in self.insns}


def parse_varnode(text: str, image_base: int = 0) -> Varnode:
    if text == "-":
        return None
    parts = text.split(":")
    if len(parts) != 3:
        raise ValueError(f"bad varnode {text!r}")
    space, value, size = parts
    if space == "ram":
        # Ghidra addresses carry the image base; normalize to ELF addresses.
        value = str(int(value, 10) - image_base)
    return Varnode(space, value, int(size))


def parse(path: Path):
    funcs = []
    header = None
    image_base = 0
    cur_func = None
    cur_insn = None
    with path.open("r", encoding="latin-1") as fh:
        for line in fh:
            line = line.rstrip("\n")
            if not line:
                continue
            tag, _, rest = line.partition("\t")
            if tag == "H":
                header = rest.split("\t")
                image_base = int(header[1], 16)
            elif tag == "F":
                entry_s, name, size, external, thunk = rest.split("\t")
                cur_func = Func(_addr(entry_s) - image_base,
                                name, int(size), external == "1", thunk == "1")
                funcs.append(cur_func)
                cur_insn = None
            elif tag == "I":
                addr_s, length, mnemonic = rest.split("\t")
                cur_insn = Insn(_addr(addr_s) - image_base, int(length), mnemonic)
                cur_func.insns.append(cur_insn)
            elif tag == "P":
                fields = rest.split("\t")
                opcode_field = fields[0]
                userop = None
                if ":" in opcode_field:
                    opcode_field, userop = opcode_field.split(":", 1)
                out = parse_varnode(fields[1], image_base)
                ins = [parse_varnode(x, image_base) for x in fields[2:]]
                cur_insn.ops.append(Op(opcode_field, out, ins, userop))
            elif tag == "X":
                break
    return header, funcs


def _addr(text: str) -> int:
    return int(text.strip(), 16)


if __name__ == "__main__":
    import sys
    header, funcs = parse(Path(sys.argv[1] if len(sys.argv) > 1 else
                               "analysis/reports/pcode.tsv"))
    print("header:", header)
    print("functions:", len(funcs))
    print("instructions:", sum(len(f.insns) for f in funcs))
    print("pcode ops:", sum(len(i.ops) for f in funcs for i in f.insns))
    cands = [f for f in funcs if not f.external and not f.thunk and f.insns]
    print("candidates:", len(cands))
    for f in sorted(cands, key=lambda x: (len(x.insns), x.size))[:25]:
        print(f"  0x{f.entry:06x} {f.size:>4} {len(f.insns):>3} insn  {f.name}")
    small = [f for f in cands if 2 <= len(f.insns) <= 6]
    print(f"\nsmall (2-6 instructions): {len(small)}")
    for f in sorted(small, key=lambda x: (len(x.insns), x.size))[:20]:
        print(f"  0x{f.entry:06x} {f.size:>4} {len(f.insns):>3} insn  {f.name}")
    if len(sys.argv) > 2:
        needle = sys.argv[2].lower()
        for f in funcs:
            if needle in f.name.lower() or needle == f"0x{f.entry:x}":
                print(f"\n=== 0x{f.entry:06x} {f.name} size={f.size} external={f.external} thunk={f.thunk}")
                for insn in f.insns:
                    print(f"  {insn.addr:06x}  {insn.mnemonic}")
                    for op in insn.ops:
                        print(f"      {op.opcode:<12} {op.out}  <-  " +
                              ", ".join(str(x) for x in op.ins))
