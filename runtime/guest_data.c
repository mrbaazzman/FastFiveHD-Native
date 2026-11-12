/*
 * Guest data symbols that the Android C library would normally provide.
 *
 * The shipped library imports `_ctype_` as a data symbol (R_ARM_GLOB_DAT). The loader cannot
 * resolve it against a host libc, so this runtime provides a real bionic-compatible table and
 * the loader points the GOT slot at the pointer cell below.
 */
#include <stdint.h>
#include <string.h>

extern uint32_t guest_mem_size;

#define CTYPE_CELL  0x9A1000u
#define CTYPE_TABLE 0x9A1100u
#define TOLOWER_CELL  0x9A1200u
#define TOLOWER_TABLE 0x9A1300u
#define TOUPPER_CELL  0x9A1280u
#define TOUPPER_TABLE 0x9A1600u

void init_guest_data(uint8_t *mem, uint32_t size) {
    if (CTYPE_TABLE + 257u > size) {
        return;
    }
    *(uint32_t *)(mem + CTYPE_CELL) = CTYPE_TABLE;
    *(uint32_t *)(mem + TOLOWER_CELL) = TOLOWER_TABLE;
    *(uint32_t *)(mem + TOUPPER_CELL) = TOUPPER_TABLE;
    unsigned char *t = mem + CTYPE_TABLE;
    memset(t, 0, 257);
    for (int c = 0; c < 128; c++) {
        unsigned char f = 0;
        if (c < 32 || c == 127) f |= 0x20;                 /* _C */
        if (c == ' ' || (c >= 9 && c <= 13)) f |= 0x08;    /* _S */
        if (c == ' ' || c == '\t') f |= 0x80;              /* _B */
        if (c >= '0' && c <= '9') f |= 0x04;               /* _N */
        if (c >= 'A' && c <= 'Z') f |= 0x01;               /* _U */
        if (c >= 'a' && c <= 'z') f |= 0x02;               /* _L */
        if ((c >= '0' && c <= '9') || (c >= 'A' && c <= 'F') ||
            (c >= 'a' && c <= 'f')) f |= 0x40;             /* _X */
        if (c > 32 && c < 127 && !(f & 0x07)) f |= 0x10;   /* _P */
        t[c + 1] = f;
    }
    unsigned short *lo = (unsigned short *)(mem + TOLOWER_TABLE);
    unsigned short *up = (unsigned short *)(mem + TOUPPER_TABLE);
    memset(lo, 0, 257u * sizeof(unsigned short));
    memset(up, 0, 257u * sizeof(unsigned short));
    /* Bionic's _tolower_tab_/_toupper_tab_ are unsigned short[257], indexed by c + 1.
     * The shipped code lowercases ASCII shader names through the 16-bit table. */
    for (int c = 0; c < 128; c++) {
        int v = c;
        lo[c + 1] = (unsigned short)((v >= 'A' && v <= 'Z') ? v + 32 : v);
        up[c + 1] = (unsigned short)((v >= 'a' && v <= 'z') ? v - 32 : v);
    }
}
