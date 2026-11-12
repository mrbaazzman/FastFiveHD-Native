#!/usr/bin/env python3
"""Trace the real obfuscation loop registers (development diagnostic).

Executes the shipped loop from libFast5.so and records the values at the exact
instructions that define the key recurrence.
"""
import struct
import sys
from pathlib import Path

from unicorn import Uc, UC_ARCH_ARM, UC_MODE_ARM, UC_HOOK_CODE
from unicorn.arm_const import (
    UC_ARM_REG_R0, UC_ARM_REG_R1, UC_ARM_REG_R2, UC_ARM_REG_R3, UC_ARM_REG_R4,
    UC_ARM_REG_R5, UC_ARM_REG_R6, UC_ARM_REG_R7, UC_ARM_REG_R8, UC_ARM_REG_R9,
    UC_ARM_REG_R10, UC_ARM_REG_R11, UC_ARM_REG_SP, UC_ARM_REG_LR, UC_ARM_REG_PC,
)

SO = Path("analysis/apk/lib/armeabi-v7a/libFast5.so")
LOOP_SKIP = 0x61988C
LOOP_TOP = 0x619888
LOOP_EXIT = 0x619934
IDIVMOD = 0x2986F4


def td(a, b):
    q = abs(a) // abs(b)
    return -q if (a < 0) != (b < 0) else q


def main():
    path = Path(sys.argv[1] if len(sys.argv) > 1 else "GloftF5HP/res/file000000.dat")
    n = int(sys.argv[2]) if len(sys.argv) > 2 else 8
    key = int(sys.argv[3], 0) if len(sys.argv) > 3 else 0x4F
    data = path.read_bytes()[:n]

    mu = Uc(UC_ARCH_ARM, UC_MODE_ARM)
    mu.mem_map(0x200000, 0x440000)
    mu.mem_map(0x10000000, 0x100000)
    mu.mem_map(0x20000000, 0x1000)
    mu.mem_write(0x619888, SO.read_bytes()[0x619888:0x61993C])
    mu.mem_write(0x20000000, data)

    sp = 0x10000000 + 0x100000 - 0x1000
    mu.mem_write(sp + 4, struct.pack("<I", key))
    mu.mem_write(sp + 0x4C, b"\0\0\0")
    for reg, val in ((UC_ARM_REG_SP, sp), (UC_ARM_REG_R4, len(data)), (UC_ARM_REG_R5, 0),
                     (UC_ARM_REG_R7, key), (UC_ARM_REG_R9, 0xAAAAAAAB),
                     (UC_ARM_REG_R10, 0x55555556), (UC_ARM_REG_R11, 0x20000000),
                     (UC_ARM_REG_LR, 0xDEADBEEF), (UC_ARM_REG_PC, LOOP_SKIP)):
        mu.reg_write(reg, val)

    rows = {}
    cur = {"i": None}

    def row():
        return rows.setdefault(cur["i"], {"i": cur["i"]})

    def hook(mu, address, size, _):
        if address == LOOP_SKIP:
            cur["i"] = mu.reg_read(UC_ARM_REG_R5)
            r = row()
            r["key"] = mu.reg_read(UC_ARM_REG_R7)
            r["byte"] = mu.mem_read(0x20000000 + cur["i"], 1)[0]
        elif address == 0x6198E4:  # after strb, before hist loads
            r = row()
            r["hist"] = list(mu.mem_read(sp + 0x4C, 3))
            r["x_u8"] = mu.reg_read(UC_ARM_REG_R8)
        elif address == 0x6198F8:  # after sum
            r = row()
            r["sum"] = mu.reg_read(UC_ARM_REG_R2)
        elif address == 0x619904:  # after signed div by 3
            r = row()
            r["q3"] = mu.reg_read(UC_ARM_REG_R2)
        elif address == 0x619908:  # before mla
            r = row()
            r["imat3"] = mu.reg_read(UC_ARM_REG_R6)
            r["rem"] = mu.reg_read(UC_ARM_REG_R1)
            r["q3b"] = mu.reg_read(UC_ARM_REG_R2)
        elif address == 0x61990C:  # add r7,r2,r7
            r = row()
            r["delta"] = mu.reg_read(UC_ARM_REG_R2)
            r["key_old"] = mu.reg_read(UC_ARM_REG_R7)
        elif address == IDIVMOD:
            r = row()
            r["prod"] = mu.reg_read(UC_ARM_REG_R0)
            r["key_div"] = mu.reg_read(UC_ARM_REG_R1)
            den = mu.reg_read(UC_ARM_REG_R1)
            num = mu.reg_read(UC_ARM_REG_R0)
            q = td(num, den)
            mu.reg_write(UC_ARM_REG_R0, q & 0xFFFFFFFF)
            mu.reg_write(UC_ARM_REG_R1, (num - q * den) & 0xFFFFFFFF)
            mu.reg_write(UC_ARM_REG_PC, mu.reg_read(UC_ARM_REG_LR))
            return
        elif address == LOOP_EXIT:
            mu.emu_stop()

    mu.hook_add(UC_HOOK_CODE, hook)
    mu.emu_start(LOOP_SKIP, LOOP_EXIT, count=len(data) * 64 + 1000)

    for i in sorted(rows):
        r = rows[i]
        print(f"i={i:2d} b=0x{r.get('byte',0):02x} key=0x{r.get('key',0):02x} x8={r.get('x_u8',0):3d} "
              f"hist={r.get('hist')} sum={r.get('sum')} q3={r.get('q3')} "
              f"i3-1={r.get('imat3')} rem={r.get('rem')} delta={r.get('delta')} "
              f"key_old=0x{r.get('key_old',0):02x} prod={r.get('prod')} key_div=0x{r.get('key_div',0):02x}")
    print("decoded:", bytes(mu.mem_read(0x20000000, len(data))).hex())
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
