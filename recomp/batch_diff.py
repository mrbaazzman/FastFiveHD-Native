#!/usr/bin/env python3
"""Run the differential test over many function addresses and summarise.

Usage:
    python3 recomp/batch_diff.py analysis/reports/pcode.tsv 0x2ac800 0x2ac810 ...
    python3 recomp/batch_diff.py analysis/reports/pcode.tsv --safe 20
"""
import argparse
import re
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("tsv")
    ap.add_argument("addresses", nargs="*")
    ap.add_argument("--safe", type=int, default=0, help="auto-select N single-load functions")
    ap.add_argument("--cases", type=int, default=6)
    ap.add_argument("--float-scratch", action="store_true")
    ap.add_argument("--closure", action="store_true")
    args = ap.parse_args()

    addresses = list(args.addresses)
    if args.safe:
        sys.path.insert(0, str(ROOT / "recomp"))
        from pcode import parse
        _h, funcs = parse(Path(args.tsv))
        picked = []
        for f in funcs:
            if f.external or not f.insns:
                continue
            addrs = {i.addr for i in f.insns}
            ops = [op for i in f.insns for op in i.ops]
            names = [op.opcode for op in ops]
            if names.count("LOAD") > 1 or "STORE" in names:
                continue
            if any(o in ("CALL", "CALLIND", "BRANCHIND") for o in names):
                continue
            if any(o.startswith("FLOAT") for o in names):
                continue
            if "RETURN" not in names:
                continue
            if any(op.opcode in ("BRANCH", "CBRANCH")
                   and int(op.ins[0].value, 10) not in addrs for op in ops):
                continue
            if not (2 <= len(f.insns) <= 12):
                continue
            picked.append(f)
        picked.sort(key=lambda f: (len(f.insns), f.entry))
        addresses += [f"0x{f.entry:x}" for f in picked[:args.safe]]

    if not addresses:
        print("no addresses", file=sys.stderr)
        return 2

    ok = fail = err = 0
    failures = []
    for addr in addresses:
        cmd = [sys.executable, str(ROOT / "recomp/diff_test.py"), args.tsv, addr,
               "--cases", str(args.cases)]
        if args.float_scratch:
            cmd.append("--float-scratch")
        if args.closure:
            cmd.append("--closure")
        proc = subprocess.run(cmd, cwd=ROOT, capture_output=True, text=True)
        out = proc.stdout + proc.stderr
        if "RESULT: PASS" in out:
            ok += 1
            print(f"PASS {addr}")
        elif "RESULT: FAIL" in out:
            fail += 1
            bad = [l for l in out.splitlines() if "MISMATCH" in l][:2]
            print(f"FAIL {addr}  {' | '.join(bad)}")
            failures.append((addr, "mismatch", bad))
        else:
            err += 1
            tail = [l for l in out.strip().splitlines() if l.strip()][-2:]
            print(f"ERR  {addr}  {tail[0] if tail else ''}")
            failures.append((addr, "error", tail))
    print(f"\nsummary: {ok} pass, {fail} mismatch, {err} error of {len(addresses)}")
    return 0 if fail == 0 and err == 0 else 1


if __name__ == "__main__":
    raise SystemExit(main())
