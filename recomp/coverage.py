#!/usr/bin/env python3
"""Stream the full P-code TSV and measure emitter coverage.

Produces a summary of how many functions the current emitter can translate, which
opcodes/userops are still unhandled, and examples. This is the work list for M1/M2.

Usage:
    python3 recomp/coverage.py analysis/reports/pcode.tsv [--out analysis/reports/pcode-coverage.md]
"""
import argparse
import sys
from collections import Counter, defaultdict
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from pcode import Func, Insn, Op, parse_varnode, _addr  # noqa: E402
from emit_c import Emitter  # noqa: E402


def iter_funcs(path: Path, base: int = 0x10000):
    cur = None
    cur_insn = None
    with path.open("r", encoding="latin-1") as fh:
        for line in fh:
            line = line.rstrip("\n")
            if not line:
                continue
            tag, _, rest = line.partition("\t")
            if tag == "F":
                if cur is not None:
                    yield cur
                entry_s, name, size, external, thunk = rest.split("\t")
                cur = Func(_addr(entry_s) - base, name, int(size),
                           external == "1", thunk == "1")
                cur_insn = None
            elif tag == "I":
                if cur is None:
                    continue
                addr_s, length, mnemonic = rest.split("\t")
                cur_insn = Insn(_addr(addr_s) - base, int(length), mnemonic)
                cur.insns.append(cur_insn)
            elif tag == "P":
                if cur_insn is None:
                    continue
                fields = rest.split("\t")
                opcode_field = fields[0]
                userop = None
                if ":" in opcode_field:
                    opcode_field, userop = opcode_field.split(":", 1)
                cur_insn.ops.append(Op(opcode_field,
                                       parse_varnode(fields[1], base),
                                       [parse_varnode(x, base) for x in fields[2:]],
                                       userop))
            elif tag == "X":
                continue
    if cur is not None:
        yield cur


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("tsv")
    ap.add_argument("--out", default="analysis/reports/pcode-coverage.md")
    args = ap.parse_args()

    total = external = no_code = emitted = 0
    failures = Counter()
    examples = defaultdict(list)
    userops = Counter()

    for f in iter_funcs(Path(args.tsv)):
        total += 1
        for insn in f.insns:
            for op in insn.ops:
                if op.opcode == "CALLOTHER" and op.userop:
                    userops[op.userop] += 1
        if f.external:
            external += 1
            continue
        if not f.insns:
            no_code += 1
            continue
        try:
            Emitter(f).emit_body()
            emitted += 1
        except Exception as ex:  # noqa: BLE001 - report every emitter gap
            key = f"{type(ex).__name__}: {ex}"
            failures[key] += 1
            if len(examples[key]) < 5:
                examples[key].append((f.entry, f.name))

    lines = []
    lines.append("# Emitter coverage — full function set\n")
    lines.append(f"- Functions in dump: **{total:,}**")
    lines.append(f"- External / no-code: {external:,} / {no_code:,}")
    lines.append(f"- Emitted successfully: **{emitted:,}**")
    lines.append(f"- Failed: **{sum(failures.values()):,}**\n")
    lines.append("## Unhandled operations\n")
    lines.append("| Error | Functions | Examples |")
    lines.append("|---|---|---|")
    for key, count in failures.most_common():
        ex = ", ".join(f"0x{a:06x} {n}" for a, n in examples[key])
        lines.append(f"| `{key}` | {count:,} | {ex} |")
    lines.append("\n## Userops seen\n")
    lines.append("| Userop | Occurrences |")
    lines.append("|---|---|")
    for name, count in userops.most_common():
        lines.append(f"| `{name}` | {count:,} |")

    text = "\n".join(lines) + "\n"
    Path(args.out).parent.mkdir(parents=True, exist_ok=True)
    Path(args.out).write_text(text)
    print(text)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
