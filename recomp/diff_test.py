#!/usr/bin/env python3
"""Differential test: translated C vs the shipped ARM instructions under Unicorn.

For each case it builds a guest image with deterministic scratch data, runs the translated
function on host x86-64, runs the original function under Unicorn on the identical
memory/register state, and compares r0-r3 plus a memory hash. Guest faults are compared
too: both sides use the same mapped size, so a wild pointer must fault at the same address.

Usage:
    python3 recomp/diff_test.py analysis/reports/pcode.tsv 0x3564c8 [--cases 8]
"""
import argparse
import importlib.util
import subprocess
import sys
from pathlib import Path

from unicorn import Uc, UC_ARCH_ARM, UC_MODE_ARM, UC_HOOK_MEM_UNMAPPED
from unicorn.arm_const import (
    UC_ARM_REG_R0, UC_ARM_REG_R1, UC_ARM_REG_R2, UC_ARM_REG_R3,
    UC_ARM_REG_SP, UC_ARM_REG_LR, UC_ARM_REG_PC,
    UC_ARM_REG_C1_C0_2, UC_ARM_REG_FPEXC,
)

ROOT = Path(__file__).resolve().parent.parent
SO = ROOT / "analysis/apk/lib/armeabi-v7a/libFast5.so"
BIN = ROOT / "build"

RET = 0xDEADBEE0


def load_module(path: Path, name: str):
    spec = importlib.util.spec_from_file_location(name, path)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


def build_guest_image() -> bytes:
    elf_image = load_module(ROOT / "recomp/elf_image.py", "elf_image")
    img = elf_image.Image(SO)
    img.apply_relocations()
    return bytes(img.mem)


def lcg(seed: int, n: int) -> bytes:
    x = seed & 0xFFFFFFFF
    out = bytearray(n)
    for i in range(n):
        x = (1664525 * x + 1013904223) & 0xFFFFFFFF
        out[i] = (x >> 16) & 0xFF
    return bytes(out)


def make_payload(image: bytes, scratch: int, seed: int, float_scratch: bool = False) -> bytes:
    size = scratch + 0x8000
    payload = bytearray(size)
    payload[:len(image)] = image
    if float_scratch:
        # Normal, non-NaN single-precision values (1.0 + small mantissa offsets).
        for i in range(0x4000 // 4):
            value = 0x3F800000 + ((i * 13) % 1024)
            payload[scratch + i * 4:scratch + i * 4 + 4] = value.to_bytes(4, "little")
    else:
        payload[scratch:scratch + 0x4000] = lcg(seed, 0x4000)
    return bytes(payload)


def fnv1a(data: bytes) -> int:
    h = 2166136261
    for b in data:
        h = ((h ^ b) * 16777619) & 0xFFFFFFFF
    return h


def is_nan32(x: int) -> bool:
    return (x & 0x7F800000) == 0x7F800000 and (x & 0x007FFFFF) != 0


def outcomes_equal(a, b, nan_tolerant: bool) -> bool:
    if a[0] != b[0]:
        return False
    if a[0] == "fault":
        return a[1] == b[1]
    # Compare r0-r3; with float data, any NaN counts as equal (x86 SSE and ARM VFP
    # choose different NaN signs/payloads, which is a host property, not translated code).
    for x, y in zip(a[1][:4], b[1][:4]):
        if x == y:
            continue
        if nan_tolerant and is_nan32(x) and is_nan32(y):
            continue
        return False
    return a[1][4] == b[1][4]


def unicorn_run(entry: int, payload: bytes, scratch: int, regs, hash_len: int):
    mu = Uc(UC_ARCH_ARM, UC_MODE_ARM)
    size = (len(payload) + 0xFFF) & ~0xFFF
    mu.mem_map(0, size)
    mu.mem_write(0, payload)
    r0, r1, r2, r3 = regs
    for reg, val in ((UC_ARM_REG_R0, r0), (UC_ARM_REG_R1, r1),
                     (UC_ARM_REG_R2, r2), (UC_ARM_REG_R3, r3),
                     (UC_ARM_REG_SP, scratch + 0x7800), (UC_ARM_REG_LR, RET),
                     (UC_ARM_REG_PC, entry)):
        mu.reg_write(reg, val)
    # Enable VFP: CPACR cp10/cp11 full access + FPEXC.EN.
    mu.reg_write(UC_ARM_REG_C1_C0_2, 0x00F00000)
    mu.reg_write(UC_ARM_REG_FPEXC, 0x40000000)

    fault = {"addr": None}

    def on_unmapped(uc, access, address, length, value, user):
        fault["addr"] = address
        uc.emu_stop()
        return True  # handled: stop without raising, so both sides compare the address

    mu.hook_add(UC_HOOK_MEM_UNMAPPED, on_unmapped)
    mu.emu_start(entry, RET, count=2000000)
    if fault["addr"] is not None:
        return ("fault", fault["addr"])
    out = [mu.reg_read(r) for r in (UC_ARM_REG_R0, UC_ARM_REG_R1,
                                    UC_ARM_REG_R2, UC_ARM_REG_R3)]
    mem = bytes(mu.mem_read(scratch, hash_len))
    return ("ok", out + [fnv1a(mem)])


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("tsv")
    ap.add_argument("entry")
    ap.add_argument("--cases", type=int, default=8)
    ap.add_argument("--float-scratch", action="store_true",
                    help="fill scratch with normal floats instead of random bytes")
    ap.add_argument("--closure", action="store_true",
                    help="emit direct-call closure and use the generated call table")
    args = ap.parse_args()
    entry = int(args.entry, 16)

    image = build_guest_image()
    image_end = len(image)
    scratch = (image_end + 0x1000) & ~0xFFF
    payload_size = scratch + 0x8000
    BIN.mkdir(exist_ok=True)

    emit_cmd = [sys.executable, str(ROOT / "recomp/emit_c.py"), args.tsv,
                f"0x{entry:x}", "--out", str(BIN / f"fn_{entry:06x}.c")]
    if args.closure:
        emit_cmd.append("--closure")
    subprocess.run(emit_cmd, check=True)

    cases = []
    for i in range(args.cases):
        seed = 0x12345678 + i * 0x9E3779B9
        payload = make_payload(image, scratch, seed, args.float_scratch)
        path = BIN / f"case_{i}.bin"
        path.write_bytes(payload)
        regs = [scratch,
                (i * 7) & 0xFF,
                (i * 13) & 0x3FF,
                (i * 17) & 0x3FF]
        cases.append((path, regs))

    reg_lines = "\n".join(
        f'    {{ "{p.as_posix()}", {r[0]}u, {r[1]}u, {r[2]}u, {r[3]}u }},'
        for p, r in cases)
    stubs = "" if args.closure else (
        "void call_target(Cpu *c, uint8_t *mem, uint32_t t) { (void)c;(void)mem;(void)t; }\n"
        "void call_indirect(Cpu *c, uint8_t *mem, uint32_t t) { (void)c;(void)mem;(void)t; }\n")
    harness = f"""#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <setjmp.h>
#include "cpu.h"
#include "mem.h"
void fn_{entry:06x}(Cpu *c, uint8_t *mem);
{stubs}
uint32_t guest_mem_size;
static jmp_buf fault_jmp;
static uint32_t fault_addr;
void guest_fault(uint32_t addr, int write) {{
    (void)write;
    fault_addr = addr;
    longjmp(fault_jmp, 1);
}}

static uint32_t fnv1a(const uint8_t *p, size_t n) {{
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < n; i++) {{ h ^= p[i]; h *= 16777619u; }}
    return h;
}}

int main(void) {{
    static uint8_t mem[{payload_size}];
    struct Case {{ const char *path; uint32_t r0, r1, r2, r3; }};
    struct Case cases[] = {{
{reg_lines}
    }};
    unsigned n = sizeof(cases)/sizeof(cases[0]);
    for (volatile unsigned i = 0; i < n; i++) {{
        FILE *f = fopen(cases[i].path, "rb");
        if (!f) {{ perror(cases[i].path); return 2; }}
        if (fread(mem, 1, sizeof mem, f) != sizeof mem) {{ return 3; }}
        fclose(f);
        Cpu c;
        memset(&c, 0, sizeof c);
        c.r0 = cases[i].r0; c.r1 = cases[i].r1; c.r2 = cases[i].r2; c.r3 = cases[i].r3;
        c.sp = {scratch + 0x7800}u; c.lr = {RET}u;
        guest_mem_size = sizeof mem;
        fault_addr = 0;
        if (setjmp(fault_jmp) == 0) {{
            fn_{entry:06x}(&c, mem);
            printf("CASE %u OK %u %u %u %u %u\\n", i, c.r0, c.r1, c.r2, c.r3,
                   fnv1a(mem + {scratch}u, {0x4000}u));
        }} else {{
            printf("CASE %u FAULT %u\\n", i, fault_addr);
        }}
    }}
    return 0;
}}
"""
    (BIN / "diff_harness.c").write_text(harness)
    subprocess.run(["gcc", "-std=c11", "-O2", "-Wall", "-Wextra", "-Werror",
                    "-I", str(ROOT / "runtime"), "-I", str(BIN),
                    str(BIN / f"fn_{entry:06x}.c"), str(BIN / "diff_harness.c"),
                    "-o", str(BIN / "diff_harness")], check=True, cwd=ROOT)
    proc = subprocess.run([str(BIN / "diff_harness")], check=True, cwd=ROOT,
                          capture_output=True, text=True)
    c_results = {}
    for line in proc.stdout.splitlines():
        p = line.split()
        if len(p) >= 3 and p[0] == "CASE":
            i = int(p[1])
            if p[2] == "FAULT":
                c_results[i] = ("fault", int(p[3]))
            else:
                c_results[i] = ("ok", [int(x) for x in p[3:8]])

    ok = True
    for i, (path, regs) in enumerate(cases):
        payload = path.read_bytes()
        emu = unicorn_run(entry, payload, scratch, regs, 0x4000)
        cval = c_results[i]
        match = outcomes_equal(emu, cval, args.float_scratch)
        ok &= match
        detail = "" if match else f"  C={cval} EMU={emu}"
        print(f"case {i}: {'OK' if match else 'MISMATCH'}{detail}")
    print("RESULT:", "PASS" if ok else "FAIL")
    return 0 if ok else 1


if __name__ == "__main__":
    raise SystemExit(main())
