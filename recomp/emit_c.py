#!/usr/bin/env python3
"""Emit C from Ghidra P-code (first translator).

The generated function is an instruction state machine: each original instruction is a
label, each P-code operation becomes C, and control flow becomes goto/PC dispatch. This is
deliberately literal: the goal is bit-identical behaviour, not pretty code.

Current scope: integer ops, flags, memory, branches, calls, returns. Floating-point and
CALLOTHER userops are left explicit so they cannot be silently mis-translated.

Usage:
    python3 recomp/emit_c.py analysis/reports/pcode.tsv <name-or-entry> [--out FILE]
"""
import argparse
import re
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from pcode import parse, Func, Insn, Op, Varnode  # noqa: E402

R_ARM_NONE = 0
RAM_SPACE_ID = 417

# ELF .plt range for this binary (see docs/01-TARGET.md): these are import stubs,
# not translated functions.
PLT_START = 0x297D38
PLT_END = 0x298B5C


def call_targets(func: Func):
    """Direct CALL targets of a function as ELF addresses."""
    for insn in func.insns:
        for op in insn.ops:
            if op.opcode != "CALL":
                continue
            v = op.ins[0]
            if v.is_ram:
                yield int(v.value, 10)
            elif v.is_const:
                yield int(v.value, 10) - 0x10000


def collect_closure(root: Func, by_entry: dict):
    seen = set()
    order = []
    externals = set()
    pending = [root]
    while pending:
        f = pending.pop()
        if f.entry in seen:
            continue
        seen.add(f.entry)
        order.append(f)
        for target in call_targets(f):
            if target in by_entry and not (PLT_START <= target < PLT_END):
                if target not in seen:
                    pending.append(by_entry[target])
            else:
                externals.add(target)
    order.sort(key=lambda f: f.entry)
    return order, externals


def emit_closure(root: Func, by_entry: dict) -> str:
    order, externals = collect_closure(root, by_entry)
    lines = []
    lines.append("/* generated from Ghidra P-code; literal instruction machines */")
    lines.append(f"/* closure of fn_{root.entry:06x}: {len(order)} functions, "
                 f"{len(externals)} external target(s) */")
    lines.append('#include "cpu.h"')
    lines.append('#include "mem.h"')
    lines.append("#include <math.h>")
    lines.append("")
    lines.append(PRELUDE.rstrip())
    lines.append("")
    lines.append("void call_target(Cpu *c, uint8_t *mem, uint32_t target);")
    lines.append("void call_indirect(Cpu *c, uint8_t *mem, uint32_t target);")
    lines.append("void call_import(Cpu *c, uint8_t *mem, uint32_t plt);")
    lines.append("")
    for f in order:
        lines.append(f"void fn_{f.entry:06x}(Cpu *c, uint8_t *mem);")
    lines.append("")
    for f in order:
        lines.append(Emitter(f).emit_body())
    lines.append("")
    lines.append("void call_target(Cpu *c, uint8_t *mem, uint32_t target) {")
    lines.append("    switch (target) {")
    for f in order:
        lines.append(f"    case 0x{f.entry:06x}u: fn_{f.entry:06x}(c, mem); return;")
    lines.append("    default: guest_fault(target, 1); return;")
    lines.append("    }")
    lines.append("}")
    lines.append("")
    lines.append("void call_indirect(Cpu *c, uint8_t *mem, uint32_t target) {")
    lines.append("    call_target(c, mem, target);")
    lines.append("}")
    return "\n".join(lines) + "\n"


REGS = {
    "sp": "c->sp", "lr": "c->lr", "pc": "c->pc", "mult_addr": "c->mult_addr",
    "fpscr": "c->fpscr",
}
for _i in range(13):
    REGS[f"r{_i}"] = f"c->r{_i}"


def reg_expr(name: str) -> str:
    if name in REGS:
        return REGS[name]
    if re.fullmatch(r"s\d+", name):
        return f"cpu_get_s(c, {int(name[1:])})"
    if re.fullmatch(r"d\d+", name):
        return f"cpu_get_d(c, {int(name[1:])})"
    return f"c->{name}"


def reg_write(name: str, expr: str, size: int) -> str:
    cast = cast_expr(expr, size)
    if re.fullmatch(r"s\d+", name):
        return f"cpu_set_s(c, {int(name[1:])}, {cast});"
    if re.fullmatch(r"d\d+", name):
        return f"cpu_set_d(c, {int(name[1:])}, {cast});"
    return f"{reg_expr(name)} = {cast};"


def cast_expr(expr: str, size: int) -> str:
    if size == 1:
        return f"(uint32_t)(uint8_t)({expr})"
    if size == 2:
        return f"(uint32_t)(uint16_t)({expr})"
    if size == 4:
        return f"(uint32_t)({expr})"
    if size == 8:
        return f"(uint64_t)({expr})"
    raise ValueError(f"unsupported size {size}")


class Emitter:
    def __init__(self, func: Func, image_base: int = 0x10000):
        self.f = func
        self.image_base = image_base
        self.uniq = {}          # offset -> size
        self.uses_flags = False
        self.lines = []
        self._collect()

    def _collect(self):
        for insn in self.f.insns:
            for op in insn.ops:
                for v in ([op.out] + op.ins):
                    if v and v.is_unique:
                        self.uniq[v.value] = max(self.uniq.get(v.value, 1), v.size)
                    if v and v.is_reg and v.value in (
                            "CY", "OV", "NG", "ZR", "TB", "shift_carry",
                            "tmpCY", "tmpOV", "tmpNG", "tmpZR"):
                        self.uses_flags = True

    # ---------- varnode access ----------
    def rd(self, v: Varnode) -> str:
        if v is None:
            raise ValueError("read of null varnode")
        if v.is_const:
            value = int(v.value, 10)
            if v.size == 8:
                return f"UINT64_C(0x{value:x})"
            return f"0x{value & ((1 << (8 * v.size)) - 1):x}u"
        if v.is_reg:
            return reg_expr(v.value)
        if v.is_unique:
            return self.u(v.value)
        if v.is_ram:
            return f"0x{int(v.value, 10):x}u"
        raise ValueError(f"unsupported read space {v.space}")

    def addr_expr(self, v: Varnode) -> str:
        """Expression for something used as a guest address."""
        if v.is_const:
            value = int(v.value, 10) - self.image_base
            return f"0x{value & 0xFFFFFFFF:x}u"
        if v.is_ram:
            return f"0x{int(v.value, 10):x}u"
        return self.rd(v)

    def code_const(self, v: Varnode) -> str:
        """A constant that denotes a code address (Ghidra base) becomes an ELF address."""
        if v.is_const:
            value = int(v.value, 10)
            if (value - self.image_base) in self.f.addr_to_insn:
                return f"0x{value - self.image_base:x}u"
        return self.rd(v)

    def u(self, off: str) -> str:
        return f"u{off}"

    def wr(self, v: Varnode, expr: str) -> str:
        if v is None:
            return f"/* discard: {expr} */;"
        if v.is_reg:
            return reg_write(v.value, expr, v.size)
        if v.is_unique:
            return f"{self.u(v.value)} = {cast_expr(expr, v.size)};"
        if v.is_ram:
            return f"/* write to ram rejected */;"
        raise ValueError(f"unsupported write space {v.space}")

    # ---------- ops ----------
    def emit_op(self, op: Op) -> None:
        o = op.opcode
        a = op.ins
        out = op.out
        if out is not None:
            w = out.size
        elif o == "STORE":
            w = a[2].size          # width is the value, not the space-ID operand
        else:
            w = a[0].size if a else 4
        bits = 32 if w <= 4 else 64

        if o == "COPY":
            self.line(self.wr(out, self.rd(a[0])))
        elif o in ("INT_ADD", "INT_SUB", "INT_AND", "INT_OR", "INT_XOR", "INT_MULT"):
            sym = {"INT_ADD": "+", "INT_SUB": "-", "INT_AND": "&",
                   "INT_OR": "|", "INT_XOR": "^", "INT_MULT": "*"}[o]
            lhs = self.code_const(a[0]) if o in ("INT_ADD", "INT_SUB") else self.rd(a[0])
            rhs = self.code_const(a[1]) if o in ("INT_ADD", "INT_SUB") else self.rd(a[1])
            self.line(self.wr(out, f"{lhs} {sym} {rhs}"))
        elif o == "INT_LEFT":
            self.line(self.wr(out, f"shl{bits}({self.rd(a[0])}, {self.rd(a[1])})"))
        elif o == "INT_RIGHT":
            self.line(self.wr(out, f"shr{bits}({self.rd(a[0])}, {self.rd(a[1])})"))
        elif o == "INT_SRIGHT":
            self.line(self.wr(out, f"asr{bits}({self.rd(a[0])}, {self.rd(a[1])})"))
        elif o == "INT_NEGATE":
            self.line(self.wr(out, f"~{self.rd(a[0])}"))
        elif o == "INT_2COMP":
            self.line(self.wr(out, f"(0 - {self.rd(a[0])})"))
        elif o in ("INT_DIV", "INT_REM"):
            fn = "udiv" if o == "INT_DIV" else "urem"
            self.line(self.wr(out, f"{fn}{bits}({self.rd(a[0])}, {self.rd(a[1])})"))
        elif o in ("INT_SDIV", "INT_SREM"):
            fn = "sdiv" if o == "INT_SDIV" else "srem"
            self.line(self.wr(out, f"{fn}{bits}({self.rd(a[0])}, {self.rd(a[1])})"))
        elif o in ("INT_EQUAL", "INT_NOTEQUAL"):
            sym = "==" if o == "INT_EQUAL" else "!="
            self.line(self.wr(out, f"({self.rd(a[0])} {sym} {self.rd(a[1])})"))
        elif o in ("INT_LESS", "INT_LESSEQUAL"):
            fn = "ult" if o == "INT_LESS" else "ule"
            self.line(self.wr(out, f"{fn}{bits}({self.rd(a[0])}, {self.rd(a[1])})"))
        elif o in ("INT_SLESS", "INT_SLESSEQUAL"):
            fn = "slt" if o == "INT_SLESS" else "sle"
            self.line(self.wr(out, f"{fn}{bits}({self.rd(a[0])}, {self.rd(a[1])})"))
        elif o in ("BOOL_AND", "BOOL_OR", "BOOL_XOR"):
            sym = {"BOOL_AND": "&&", "BOOL_OR": "||", "BOOL_XOR": "!="}[o]
            self.line(self.wr(out, f"({self.rd(a[0])} {sym} {self.rd(a[1])})"))
        elif o == "BOOL_NEGATE":
            self.line(self.wr(out, f"(!{self.rd(a[0])})"))
        elif o == "INT_ZEXT":
            self.line(self.wr(out, self.rd(a[0])))
        elif o == "INT_SEXT":
            src = a[0].size
            self.line(self.wr(out, f"(int32_t)(int{src * 8}_t){self.rd(a[0])}"))
        elif o == "SUBPIECE":
            off = int(a[1].value, 10) if a[1].is_const else 0
            self.line(self.wr(out, f"(({self.rd(a[0])}) >> {8 * off})"))
        elif o == "PIECE":
            self.line(self.wr(out, f"(({self.rd(a[0])} << {8 * a[1].size}) | {self.rd(a[1])})"))
        elif o == "INT_CARRY":
            self.line(self.wr(out, f"carry{bits}({self.rd(a[0])}, {self.rd(a[1])})"))
        elif o == "INT_SCARRY":
            self.line(self.wr(out, f"scarry{bits}({self.rd(a[0])}, {self.rd(a[1])})"))
        elif o == "INT_SBORROW":
            self.line(self.wr(out, f"sborrow{bits}({self.rd(a[0])}, {self.rd(a[1])})"))
        elif o == "LZCOUNT":
            self.line(self.wr(out, f"clz{bits}({self.rd(a[0])})"))
        elif o == "LOAD":
            addr = self.addr_expr(a[1])
            self.line(self.wr(out, f"ld{w}(mem, {addr})"))
        elif o == "STORE":
            addr = self.addr_expr(a[1])
            val = self.rd(a[2])
            self.line(f"st{w}(mem, {addr}, {val});")
        elif o == "BRANCH":
            self.goto(a[0])
        elif o == "CBRANCH":
            self.cond_goto(a[0], self.rd(a[1]))
        elif o == "BRANCHIND":
            self.line(f"c->pc = {self.rd(a[0])}; goto dispatch;")
        elif o == "CALL":
            self.line(f"call_target(c, mem, {self.rd(a[0])});")
        elif o == "CALLIND":
            self.line(f"call_indirect(c, mem, {self.rd(a[0])});")
        elif o == "RETURN":
            self.line("return;")
        elif o == "CALLOTHER":
            name = op.userop or ""
            if name == "setISAMode":
                self.line("/* setISAMode: ARM-only target, interworking is a no-op */")
            elif name in ("VectorSignedToFloat", "VectorUnsignedToFloat"):
                src = a[1]
                signed = name == "VectorSignedToFloat"
                fn = "i2f" if signed else "u2f"
                ctype = f"{'int' if signed else 'uint'}{src.size * 8}_t"
                self.line(self.wr(out, f"{fn}{bits}(({ctype}){self.rd(src)})"))
            elif name in ("VectorFloatToSigned", "VectorFloatToUnsigned"):
                src = a[1]
                fn = "f2i" if name == "VectorFloatToSigned" else "f2u"
                self.line(self.wr(out, f"{fn}{bits}({self.rd(src)})"))
            else:
                self.line(f"/* CALLOTHER {name or 'unknown'}: not yet translated */")
        elif o == "CAST":
            self.line(self.wr(out, self.rd(a[0])))
        elif o == "TRUNC":
            self.line(self.wr(out, f"truncf{bits}({self.rd(a[0])})"))
        elif o == "CEIL":
            self.line(self.wr(out, f"ceilf{bits}({self.rd(a[0])})"))
        elif o == "FLOOR":
            self.line(self.wr(out, f"floorf{bits}({self.rd(a[0])})"))
        elif o == "ROUND":
            self.line(self.wr(out, f"roundf{bits}({self.rd(a[0])})"))
        elif o == "INT2FLOAT":
            src_bits = a[0].size * 8
            self.line(self.wr(out, f"i2f{bits}((int{src_bits}_t){self.rd(a[0])})"))
        elif o == "FLOAT2FLOAT":
            src_bits = a[0].size * 8
            if src_bits == bits:
                self.line(self.wr(out, self.rd(a[0])))
            else:
                self.line(self.wr(out, f"f2f{src_bits}_{bits}({self.rd(a[0])})"))
        elif o == "FLOAT_ADD":
            self.line(self.wr(out, f"fadd{bits}({self.rd(a[0])}, {self.rd(a[1])})"))
        elif o == "FLOAT_SUB":
            self.line(self.wr(out, f"fsub{bits}({self.rd(a[0])}, {self.rd(a[1])})"))
        elif o == "FLOAT_MULT":
            self.line(self.wr(out, f"fmul{bits}({self.rd(a[0])}, {self.rd(a[1])})"))
        elif o == "FLOAT_DIV":
            self.line(self.wr(out, f"fdiv{bits}({self.rd(a[0])}, {self.rd(a[1])})"))
        elif o == "FLOAT_SQRT":
            self.line(self.wr(out, f"fsqrt{bits}({self.rd(a[0])})"))
        elif o == "FLOAT_ABS":
            self.line(self.wr(out, f"fabs{bits}({self.rd(a[0])})"))
        elif o == "FLOAT_NEG":
            self.line(self.wr(out, f"fneg{bits}({self.rd(a[0])})"))
        elif o in ("FLOAT_LESS", "FLOAT_LESSEQUAL", "FLOAT_EQUAL"):
            src_bits = a[0].size * 8
            fn = {"FLOAT_LESS": "flt", "FLOAT_LESSEQUAL": "fle", "FLOAT_EQUAL": "feq"}[o]
            self.line(self.wr(out, f"{fn}{src_bits}({self.rd(a[0])}, {self.rd(a[1])})"))
        elif o == "FLOAT_NAN":
            src_bits = a[0].size * 8
            self.line(self.wr(out, f"fnan{src_bits}({self.rd(a[0])})"))
        elif o.startswith("FLOAT_") or o.startswith("INT2FLOAT") or o.startswith("FLOAT2"):
            self.line(f"/* TODO: {o} */")
        else:
            raise NotImplementedError(f"unhandled opcode {o}")

    def target_addr(self, v: Varnode) -> int:
        value = int(v.value, 10)
        return value - self.image_base if v.is_const else value

    def goto(self, target: Varnode) -> None:
        addr = self.target_addr(target)
        if PLT_START <= addr < PLT_END:
            self.line(f"call_import(c, mem, 0x{addr:06x}u);")
            self.line("return;")
        elif addr in self.f.addr_to_insn:
            self.line(f"goto L_{addr:06x};")
        else:
            self.line(f"c->pc = 0x{addr:06x}u; goto dispatch;")

    def cond_goto(self, target: Varnode, cond: str) -> None:
        addr = self.target_addr(target)
        if PLT_START <= addr < PLT_END:
            self.line(f"if ({cond}) {{ call_import(c, mem, 0x{addr:06x}u); return; }}")
        elif addr in self.f.addr_to_insn:
            self.line(f"if ({cond}) goto L_{addr:06x};")
        else:
            self.line(f"if ({cond}) {{ c->pc = 0x{addr:06x}u; goto dispatch; }}")

    def line(self, text: str) -> None:
        self.lines.append("    " + text)

    # ---------- function ----------
    def emit_body(self) -> str:
        f = self.f
        self.lines = []
        self.lines.append(f"void fn_{f.entry:06x}(Cpu *c, uint8_t *mem) {{")
        for off, size in sorted(self.uniq.items(), key=lambda kv: int(kv[0])):
            ctype = "uint64_t" if size == 8 else "uint32_t"
            self.lines.append(f"    {ctype} {self.u(off)} __attribute__((unused)) = 0;")
        self.lines.append(f"    goto L_{f.entry:06x};")
        self.lines.append("")

        # Ghidra can merge PLT stubs into a function body; those are imports, not code.
        body_insns = [i for i in f.insns if not (PLT_START <= i.addr < PLT_END)]
        if not body_insns:
            self.lines.append(f"    call_import(c, mem, 0x{f.entry:06x}u);")
            self.lines.append("}")
            return "\n".join(self.lines) + "\n"
        insns = {i.addr: i for i in body_insns}
        for idx, insn in enumerate(body_insns):
            self.lines.append(f"L_{insn.addr:06x}: /* {insn.mnemonic} */")
            for op in insn.ops:
                self.emit_op(op)
            last = insn.ops[-1].opcode if insn.ops else ""
            if last not in ("BRANCH", "BRANCHIND", "RETURN"):
                nxt = insn.addr + insn.length
                if nxt in insns:
                    self.lines.append(f"    goto L_{nxt:06x};")
                else:
                    self.lines.append(f"    c->pc = 0x{nxt:06x}u; goto dispatch;")
            self.lines.append("")

        self.lines.append("    if (0) goto dispatch;")
        self.lines.append("dispatch:")
        self.lines.append("    switch (c->pc) {")
        for insn in body_insns:
            self.lines.append(f"    case 0x{insn.addr:06x}u: goto L_{insn.addr:06x};")
        self.lines.append("    default:")
        self.lines.append("        call_indirect(c, mem, c->pc);")
        self.lines.append("        return;")
        self.lines.append("    }")
        self.lines.append("}")
        return "\n".join(self.lines) + "\n"

    def emit(self) -> str:
        f = self.f
        self.lines = []
        self.lines.append("/* generated from Ghidra P-code; literal instruction machine */")
        self.lines.append('#include "cpu.h"')
        self.lines.append('#include "mem.h"')
        self.lines.append('#include <math.h>')
        self.lines.append("")
        self.lines.append(PRELUDE.rstrip())
        self.lines.append("")
        self.lines.append("void call_target(Cpu *c, uint8_t *mem, uint32_t target);")
        self.lines.append("void call_indirect(Cpu *c, uint8_t *mem, uint32_t target);")
        self.lines.append("void call_import(Cpu *c, uint8_t *mem, uint32_t plt);")
        self.lines.append("")
        self.lines.append(self.emit_body())
        return "\n".join(self.lines) + "\n"


PRELUDE = """/* arithmetic helpers matching P-code semantics (division by zero yields 0,
 * shifts take the count modulo the operand width). */
static inline uint32_t shl32(uint32_t v, uint32_t s) { return v << (s & 31); }
static inline uint32_t shr32(uint32_t v, uint32_t s) { return v >> (s & 31); }
static inline uint32_t asr32(uint32_t v, uint32_t s) { return (uint32_t)((int32_t)v >> (s & 31)); }
static inline uint64_t shl64(uint64_t v, uint64_t s) { return v << (s & 63); }
static inline uint64_t shr64(uint64_t v, uint64_t s) { return v >> (s & 63); }
static inline uint64_t asr64(uint64_t v, uint64_t s) { return (uint64_t)((int64_t)v >> (s & 63)); }
static inline uint32_t udiv32(uint32_t a, uint32_t b) { return b ? a / b : 0; }
static inline uint32_t urem32(uint32_t a, uint32_t b) { return b ? a % b : 0; }
static inline uint32_t sdiv32(int32_t a, int32_t b) { return b ? (uint32_t)(a / b) : 0; }
static inline uint32_t srem32(int32_t a, int32_t b) { return b ? (uint32_t)(a % b) : 0; }
static inline uint64_t udiv64(uint64_t a, uint64_t b) { return b ? a / b : 0; }
static inline uint64_t urem64(uint64_t a, uint64_t b) { return b ? a % b : 0; }
static inline uint64_t sdiv64(int64_t a, int64_t b) { return b ? (uint64_t)(a / b) : 0; }
static inline uint64_t srem64(int64_t a, int64_t b) { return b ? (uint64_t)(a % b) : 0; }
static inline uint32_t carry32(uint32_t a, uint32_t b) { return (uint32_t)(((uint64_t)a + b) >> 32) & 1u; }
static inline uint32_t carry64(uint64_t a, uint64_t b) { return (uint32_t)(((unsigned __int128)a + b) >> 64) & 1u; }
static inline uint32_t scarry32(uint32_t a, uint32_t b) { uint32_t r = a + b; return ((a ^ r) & (b ^ r)) >> 31; }
static inline uint32_t scarry64(uint64_t a, uint64_t b) { uint64_t r = a + b; return (uint32_t)(((a ^ r) & (b ^ r)) >> 63); }
static inline uint32_t sborrow32(uint32_t a, uint32_t b) { uint32_t r = a - b; return ((a ^ b) & (a ^ r)) >> 31; }
static inline uint32_t sborrow64(uint64_t a, uint64_t b) { uint64_t r = a - b; return (uint32_t)(((a ^ b) & (a ^ r)) >> 63); }
static inline uint32_t ult32(uint32_t a, uint32_t b) { return a < b; }
static inline uint32_t ule32(uint32_t a, uint32_t b) { return a <= b; }
static inline uint32_t slt32(uint32_t a, uint32_t b) { return (int32_t)a < (int32_t)b; }
static inline uint32_t sle32(uint32_t a, uint32_t b) { return (int32_t)a <= (int32_t)b; }
static inline uint32_t ult64(uint64_t a, uint64_t b) { return a < b; }
static inline uint32_t ule64(uint64_t a, uint64_t b) { return a <= b; }
static inline uint32_t slt64(uint64_t a, uint64_t b) { return (int64_t)a < (int64_t)b; }
static inline uint32_t sle64(uint64_t a, uint64_t b) { return (int64_t)a <= (int64_t)b; }
static inline uint32_t clz32(uint32_t x) { return x ? (uint32_t)__builtin_clz(x) : 32u; }
static inline uint32_t clz64(uint64_t x) { return x ? (uint32_t)__builtin_clzll(x) : 64u; }

/* VFP bit-pattern helpers (IEEE-754 preserved; D32/VFPv3). */
static inline float as_f32(uint32_t x) { float f; memcpy(&f, &x, 4); return f; }
static inline uint32_t as_u32(float f) { uint32_t x; memcpy(&x, &f, 4); return x; }
static inline double as_f64(uint64_t x) { double d; memcpy(&d, &x, 8); return d; }
static inline uint64_t as_u64(double d) { uint64_t x; memcpy(&x, &d, 8); return x; }
static inline uint32_t fadd32(uint32_t a, uint32_t b) { return as_u32(as_f32(a) + as_f32(b)); }
static inline uint32_t fsub32(uint32_t a, uint32_t b) { return as_u32(as_f32(a) - as_f32(b)); }
static inline uint32_t fmul32(uint32_t a, uint32_t b) { return as_u32(as_f32(a) * as_f32(b)); }
static inline uint32_t fdiv32(uint32_t a, uint32_t b) { return as_u32(as_f32(a) / as_f32(b)); }
static inline uint32_t fsqrt32(uint32_t a) { return as_u32(sqrtf(as_f32(a))); }
static inline uint32_t fabs32(uint32_t a) { return a & 0x7fffffffu; }
static inline uint32_t fneg32(uint32_t a) { return a ^ 0x80000000u; }
static inline uint32_t flt32(uint32_t a, uint32_t b) { return as_f32(a) < as_f32(b); }
static inline uint32_t fle32(uint32_t a, uint32_t b) { return as_f32(a) <= as_f32(b); }
static inline uint32_t feq32(uint32_t a, uint32_t b) { return as_f32(a) == as_f32(b); }
static inline uint32_t fnan32(uint32_t a) { return isnan(as_f32(a)); }
static inline uint32_t truncf32(uint32_t a) { return (uint32_t)(int32_t)as_f32(a); }
static inline uint32_t floorf32(uint32_t a) { return (uint32_t)(int32_t)floorf(as_f32(a)); }
static inline uint32_t ceilf32(uint32_t a) { return (uint32_t)(int32_t)ceilf(as_f32(a)); }
static inline uint32_t roundf32(uint32_t a) { return (uint32_t)(int32_t)roundf(as_f32(a)); }
static inline uint64_t fadd64(uint64_t a, uint64_t b) { return as_u64(as_f64(a) + as_f64(b)); }
static inline uint64_t fsub64(uint64_t a, uint64_t b) { return as_u64(as_f64(a) - as_f64(b)); }
static inline uint64_t fmul64(uint64_t a, uint64_t b) { return as_u64(as_f64(a) * as_f64(b)); }
static inline uint64_t fdiv64(uint64_t a, uint64_t b) { return as_u64(as_f64(a) / as_f64(b)); }
static inline uint64_t fsqrt64(uint64_t a) { return as_u64(sqrt(as_f64(a))); }
static inline uint64_t fabs64(uint64_t a) { return a & 0x7fffffffffffffffull; }
static inline uint64_t fneg64(uint64_t a) { return a ^ 0x8000000000000000ull; }
static inline uint32_t flt64(uint64_t a, uint64_t b) { return as_f64(a) < as_f64(b); }
static inline uint32_t fle64(uint64_t a, uint64_t b) { return as_f64(a) <= as_f64(b); }
static inline uint32_t feq64(uint64_t a, uint64_t b) { return as_f64(a) == as_f64(b); }
static inline uint32_t fnan64(uint64_t a) { return isnan(as_f64(a)); }
static inline uint64_t truncf64(uint64_t a) { return (uint64_t)(int64_t)as_f64(a); }
static inline uint64_t floorf64(uint64_t a) { return (uint64_t)(int64_t)floor(as_f64(a)); }
static inline uint64_t ceilf64(uint64_t a) { return (uint64_t)(int64_t)ceil(as_f64(a)); }
static inline uint64_t roundf64(uint64_t a) { return (uint64_t)(int64_t)round(as_f64(a)); }
static inline uint32_t i2f32(int32_t a) { return as_u32((float)a); }
static inline uint64_t i2f64(int64_t a) { return as_u64((double)a); }
static inline uint32_t u2f32(uint32_t a) { return as_u32((float)a); }
static inline uint64_t u2f64(uint64_t a) { return as_u64((double)a); }
static inline uint32_t f2i32(uint32_t a) { return (uint32_t)(int32_t)as_f32(a); }
static inline uint32_t f2u32(uint32_t a) { return (uint32_t)as_f32(a); }
static inline uint64_t f2i64(uint64_t a) { return (uint64_t)(int64_t)as_f64(a); }
static inline uint64_t f2u64(uint64_t a) { return (uint64_t)as_f64(a); }
static inline uint64_t f2f32_64(uint32_t a) { return as_u64((double)as_f32(a)); }
static inline uint32_t f2f64_32(uint64_t a) { return as_u32((float)as_f64(a)); }

"""


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("tsv")
    ap.add_argument("selector")
    ap.add_argument("--out")
    ap.add_argument("--no-prelude", action="store_true")
    ap.add_argument("--closure", action="store_true",
                    help="also emit all direct-call targets and a function table")
    args = ap.parse_args()

    _header, funcs = parse(Path(args.tsv))
    needle = args.selector.lower()
    matches = [f for f in funcs
               if needle in f.name.lower() or needle == f"0x{f.entry:x}"]
    if not matches:
        print(f"no function matches {args.selector!r}", file=sys.stderr)
        return 1
    if len(matches) > 1:
        print(f"{len(matches)} matches; choose an address:", file=sys.stderr)
        for f in matches[:20]:
            print(f"  0x{f.entry:06x} {f.name}", file=sys.stderr)
        return 2

    f = matches[0]
    image_base = int(_header[1], 16) if _header and len(_header) > 1 else 0x10000
    if args.closure:
        by_entry = {x.entry: x for x in funcs}
        text = emit_closure(f, by_entry)
    else:
        text = Emitter(f, image_base).emit()
    if args.out:
        out = Path(args.out)
        out.parent.mkdir(parents=True, exist_ok=True)
        out.write_text(text)
        print(f"wrote {out} ({len(text)} bytes)")
    else:
        sys.stdout.write(text)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
