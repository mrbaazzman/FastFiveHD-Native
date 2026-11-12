#ifndef FAST5_CPU_H
#define FAST5_CPU_H

#include <stdint.h>
#include <string.h>

/*
 * Guest CPU state for translated ARMv7 code.
 *
 * The layout mirrors what Ghidra's P-code names in this binary:
 *   - r0..r12, sp, lr, pc
 *   - the ARM condition-flag registers used by Ghidra's ARM P-code
 *   - mult_addr, the scratch writeback register used by ldm/stm lowering
 *   - vfp[64] as 32-bit words: sN is vfp[N], dN is the pair vfp[2N], vfp[2N+1]
 */
typedef struct Cpu {
    uint32_t r0, r1, r2, r3, r4, r5, r6, r7, r8, r9, r10, r11, r12;
    uint32_t sp, lr, pc;

    uint32_t vfp[64];
    uint32_t fpscr;

    uint32_t mult_addr;

    uint32_t CY, OV, NG, ZR, TB, shift_carry;
    uint32_t tmpCY, tmpOV, tmpNG, tmpZR;
} Cpu;

static inline uint64_t cpu_get_d(const Cpu *c, int n) {
    uint64_t v;
    memcpy(&v, &c->vfp[2 * n], sizeof v);
    return v;
}

static inline void cpu_set_d(Cpu *c, int n, uint64_t v) {
    memcpy(&c->vfp[2 * n], &v, sizeof v);
}

static inline uint32_t cpu_get_s(const Cpu *c, int n) {
    return c->vfp[n];
}

static inline void cpu_set_s(Cpu *c, int n, uint32_t v) {
    c->vfp[n] = v;
}

#endif /* FAST5_CPU_H */
