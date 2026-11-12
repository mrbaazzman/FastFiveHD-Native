#!/usr/bin/env python3
"""Execute the real obfuscation-map decode loop from libFast5.so with Unicorn.

The loop is taken byte-for-byte from glitch::io::CFileSystem::addObfuscationFileMap
(ELF text 0x619888-0x619938). Only __aeabi_idivmod (PLT at 0x2986f4) is stubbed, because
it lives outside the copied region. This is an execution of the shipped instructions, not
a re-implementation of them.
"""
import struct
import sys
from pathlib import Path

from unicorn import Uc, UC_ARCH_ARM, UC_MODE_ARM, UC_HOOK_CODE
from unicorn.arm_const import (
    UC_ARM_REG_R0, UC_ARM_REG_R1, UC_ARM_REG_R2, UC_ARM_REG_R3, UC_ARM_REG_R4,
    UC_ARM_REG_R5, UC_ARM_REG_R6, UC_ARM_REG_R7, UC_ARM_REG_R9, UC_ARM_REG_R10,
    UC_ARM_REG_R11, UC_ARM_REG_R12, UC_ARM_REG_SP, UC_ARM_REG_LR, UC_ARM_REG_PC,
)

SO = Path("analysis/apk/lib/armeabi-v7a/libFast5.so")
TEXT_OFF = 0x298b60
LOOP_ENTRY = 0x619888
LOOP_SKIP_ADD = 0x61988c
LOOP_END = 0x619938
LOOP_EXIT = 0x619934
IDIVMOD_PLT = 0x2986f4

CODE_BASE = 0x200000
CODE_SIZE = 0x440000
STACK_BASE = 0x10000000
STACK_SIZE = 0x100000
BUF_BASE = 0x20000000
RETURN_MAGIC = 0xDEADBEEF


def trunc_div(a, b):
    q = abs(a) // abs(b)
    return -q if (a < 0) != (b < 0) else q


def run(data: bytes, key: int) -> bytes:
    so = SO.read_bytes()
    buf = bytearray(data)

    mu = Uc(UC_ARCH_ARM, UC_MODE_ARM)
    mu.mem_map(CODE_BASE, CODE_SIZE)
    mu.mem_map(STACK_BASE, STACK_SIZE)
    mu.mem_map(BUF_BASE, (len(buf) + 0xFFF) & ~0xFFF)

    # Copy the real loop bytes (file offset == vaddr for this ELF's .text).
    loop_bytes = so[LOOP_ENTRY:LOOP_END + 4]
    mu.mem_write(LOOP_ENTRY, loop_bytes)
    mu.mem_write(BUF_BASE, bytes(buf))

    sp = STACK_BASE + STACK_SIZE - 0x1000
    mu.mem_write(sp + 4, struct.pack("<I", key))
    mu.mem_write(sp + 0x4C, b"\x00\x00\x00")
    for reg, val in (
        (UC_ARM_REG_SP, sp),
        (UC_ARM_REG_R4, len(buf)),
        (UC_ARM_REG_R5, 0),
        (UC_ARM_REG_R7, key),
        (UC_ARM_REG_R9, 0xAAAAAAAB),
        (UC_ARM_REG_R10, 0x55555556),
        (UC_ARM_REG_R11, BUF_BASE),
        (UC_ARM_REG_R12, 0),
        (UC_ARM_REG_LR, RETURN_MAGIC),
        (UC_ARM_REG_PC, LOOP_SKIP_ADD),
    ):
        mu.reg_write(reg, val)

    calls = {"n": 0}

    def on_code(mu, address, size, _user):
        if address == IDIVMOD_PLT:
            r0 = mu.reg_read(UC_ARM_REG_R0)
            r1 = mu.reg_read(UC_ARM_REG_R1)
            if r1 == 0:
                raise ZeroDivisionError("idivmod by zero")
            q = trunc_div(r0, r1)
            rem = r0 - q * r1
            mu.reg_write(UC_ARM_REG_R0, q & 0xFFFFFFFF)
            mu.reg_write(UC_ARM_REG_R1, rem & 0xFFFFFFFF)
            mu.reg_write(UC_ARM_REG_PC, mu.reg_read(UC_ARM_REG_LR))
            calls["n"] += 1
            return
        if address == RETURN_MAGIC:
            mu.emu_stop()

    mu.hook_add(UC_HOOK_CODE, on_code)
    mu.emu_start(LOOP_SKIP_ADD, LOOP_EXIT, count=len(buf) * 64 + 10000)

    out = bytes(mu.mem_read(BUF_BASE, len(buf)))
    return out


def main() -> int:
    path = Path(sys.argv[1] if len(sys.argv) > 1 else "GloftF5HP/res/file000000.dat")
    data = path.read_bytes()
    keys = [int(sys.argv[2], 0)] if len(sys.argv) > 2 else list(range(256))
    for key in keys:
        out = run(data, key)
        stripped = out.rstrip(b"\x00")
        ok = stripped[-6:] == b"c|-|3k"
        print(f"key 0x{key:02x}: trailer={stripped[-6:]!r} marker={ok}")
        if ok:
            Path("analysis/reports/dat-map.raw").write_bytes(out)
            print("decoded map written to analysis/reports/dat-map.raw")
            return 0
    return 1


if __name__ == "__main__":
    raise SystemExit(main())
