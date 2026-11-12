/*
 * Import layer for translated ARM code.
 *
 * The shipped library imports 300 functions through its PLT (see imports_table.h, generated
 * from .rel.plt). Translated code calls those PLT addresses; this file maps each address to a
 * host implementation. Arguments follow the ARM softfp ABI: core registers carry both integer
 * arguments and float bit patterns.
 *
 * This is bring-up quality: it is honest about what is implemented, logs unknown imports once,
 * and never pretends to be a full libc. Imports used by the game's constructors and early boot
 * are implemented first.
 */
#define _GNU_SOURCE
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>
#include <strings.h>

#include "cpu.h"
#include "mem.h"
#include "imports_table.h"
#include "thread_win32.h"
#ifdef _WIN32
#include "platform.h"
#endif

#ifdef _WIN32
#include <windows.h>
#endif

#ifdef _WIN32
static struct tm *localtime_r(const time_t *t, struct tm *out) {
    struct tm *p = localtime(t);
    if (!p) return NULL;
    *out = *p;
    return out;
}
static struct tm *gmtime_r(const time_t *t, struct tm *out) {
    struct tm *p = gmtime(t);
    if (!p) return NULL;
    *out = *p;
    return out;
}
#endif

extern uint32_t guest_mem_size;

#define GUEST_HEAP_BASE 0x08000000u             /* first byte after the loaded image and its data */

static uint8_t *g_mem;
static uint8_t g_seen[IMPORT_COUNT];
static uint32_t g_errno = 0x8B0000u;
static uint32_t g_gl_id = 1u;
static int g_verbose = -1;

void call_import(Cpu *c, uint8_t *mem, uint32_t plt);
static void import_dispatch(Cpu *c, uint8_t *mem, const char *n);

static void *hostp(uint32_t addr, uint32_t n);
static void f64_to(double d, uint32_t *lo, uint32_t *hi);
#ifdef _WIN32
int gl_bridge_init(void);
int gl_bridge_call(Cpu *c, uint8_t *mem, const char *name);
#endif

uint32_t vfs_open(const char *guest, const char *mode);
uint32_t vfs_read(uint32_t handle, void *dst, uint32_t size, uint32_t n);
uint32_t vfs_write(uint32_t handle, const void *src, uint32_t size, uint32_t n);
int vfs_close(uint32_t handle);
int vfs_seek(uint32_t handle, int32_t off, int whence);
int32_t vfs_tell(uint32_t handle);
int vfs_getc(uint32_t handle);
int vfs_putc(int ch, uint32_t handle);
char *vfs_gets(char *dst, int n, uint32_t handle);
int vfs_puts(const char *s, uint32_t handle);
int vfs_eof(uint32_t handle);
int vfs_error(uint32_t handle);
int vfs_fileno(uint32_t handle);
uint32_t vfs_fdopen(int fd, const char *mode);
int vfs_fstat_fd(int fd, void *st);
int vfs_lseek_fd(int fd, int32_t off, int whence);
int vfs_read_fd(int fd, void *dst, uint32_t n);
int vfs_write_fd(int fd, const void *src, uint32_t n);
int vfs_close_fd(int fd);
int vfs_chdir(const char *path);
void call_target(Cpu *c, uint8_t *mem, uint32_t target);

static void put_str(uint8_t *mem, uint32_t addr, const char *s) {
    size_t n = strlen(s) + 1;
    if (addr > guest_mem_size || n > guest_mem_size - addr) {
        guest_fault(addr, 1);
        return;
    }
    memcpy(mem + addr, s, n);
}

static void w32(uint8_t *mem, uint32_t addr, uint32_t v) {
    if (addr > guest_mem_size || 4u > guest_mem_size - addr) {
        guest_fault(addr, 1);
        return;
    }
    memcpy(mem + addr, &v, 4);
}

static uint32_t r32(uint8_t *mem, uint32_t addr) {
    uint32_t v = 0;
    if (addr > guest_mem_size || 4u > guest_mem_size - addr) {
        guest_fault(addr, 0);
        return 0;
    }
    memcpy(&v, mem + addr, 4);
    return v;
}

/* ARM softfp varargs: first two pointers in r2/r3, the rest on the guest stack. */
static uint32_t sscanf_arg(Cpu *c, uint8_t *mem, uint32_t *idx) {
    uint32_t i = (*idx)++;
    if (i == 0) return c->r2;
    if (i == 1) return c->r3;
    return r32(mem, c->sp + (i - 2u) * 4u);
}

typedef struct {
    Cpu *c;
    uint8_t *mem;
    uint32_t regs[3];
    int nregs;
    uint32_t idx;
} ArgReader;

static uint32_t next_arg(ArgReader *a) {
    uint32_t i = a->idx++;
    if (i < (uint32_t)a->nregs) return a->regs[i];
    return r32(a->mem, a->c->sp + (i - (uint32_t)a->nregs) * 4u);
}

static void print_format(uint8_t *mem, const char *fmt, Cpu *c,
                         const uint32_t *regs, int nregs) {
    ArgReader a = {c, mem, {0, 0, 0}, nregs, 0};
    for (int i = 0; i < nregs && i < 3; i++) a.regs[i] = regs[i];

    while (*fmt) {
        if (*fmt != '%') { fputc(*fmt++, stderr); continue; }
        fmt++;
        if (*fmt == '%') { fputc('%', stderr); fmt++; continue; }
        while (*fmt && !strchr("diuxXscfpg", *fmt)) fmt++;
        char conv = *fmt ? *fmt++ : 0;
        uint32_t arg = next_arg(&a);
        switch (conv) {
        case 's': {
            const char *s = (const char *)hostp(arg, 1);
            fputs(s ? s : "(null)", stderr);
            break;
        }
        case 'd': fprintf(stderr, "%d", (int32_t)arg); break;
        case 'u': fprintf(stderr, "%u", arg); break;
        case 'x': fprintf(stderr, "%x", arg); break;
        case 'X': fprintf(stderr, "%X", arg); break;
        case 'p': fprintf(stderr, "%08x", arg); break;
        case 'c': fputc((int)(arg & 0xFF), stderr); break;
        case 'f': fputs("<float>", stderr); break;
        default: break;
        }
    }
}

typedef struct {
    uint8_t *mem;
    uint32_t addr;
    uint32_t cap;
    uint32_t len;
} OutBuf;

static void out_ch(OutBuf *o, char ch) {
    if (o->cap && o->len + 1 < o->cap && o->addr + o->len < guest_mem_size) {
        o->mem[o->addr + o->len] = (uint8_t)ch;
    }
    o->len++;
}

static void out_text(OutBuf *o, const char *text) {
    while (*text) out_ch(o, *text++);
}

static void out_finish(OutBuf *o) {
    uint32_t n = o->len < o->cap ? o->len : (o->cap ? o->cap - 1u : 0u);
    if (o->cap && o->addr + n < guest_mem_size) o->mem[o->addr + n] = 0;
}

static void make_spec(char *spec, size_t size, int left, int plus, int space, int zero,
                      int alt, int width, int prec, int lmod, char conv) {
    size_t k = 0;
    spec[k++] = '%';
    if (left) spec[k++] = '-';
    if (plus) spec[k++] = '+';
    if (space) spec[k++] = ' ';
    if (alt) spec[k++] = '#';
    if (zero) spec[k++] = '0';
    if (width > 0) k += (size_t)snprintf(spec + k, size - k, "%d", width);
    if (prec >= 0) {
        if (k + 1 < size) spec[k++] = '.';
        k += (size_t)snprintf(spec + k, size - k, "%d", prec);
    }
    if (lmod >= 2) {
        if (k + 2 < size) { spec[k++] = 'l'; spec[k++] = 'l'; }
    } else if (lmod == 1) {
        if (k + 1 < size) spec[k++] = 'l';
    }
    if (k + 1 < size) spec[k++] = conv;
    spec[k < size ? k : size - 1u] = 0;
}

static uint32_t guest_format(Cpu *c, uint8_t *mem, uint32_t dst, uint32_t cap, const char *fmt,
                             int nregs, const uint32_t *regs) {
    ArgReader a = {c, mem, {0, 0, 0}, nregs, 0};
    for (int i = 0; i < nregs && i < 3; i++) a.regs[i] = regs[i];
    OutBuf out = {mem, dst, cap, 0};

    while (*fmt) {
        if (*fmt != '%') { out_ch(&out, *fmt++); continue; }
        fmt++;
        if (*fmt == '%') { out_ch(&out, '%'); fmt++; continue; }

        int left = 0, plus = 0, space = 0, zero = 0, alt = 0;
        for (;;) {
            if (*fmt == '-') left = 1;
            else if (*fmt == '+') plus = 1;
            else if (*fmt == ' ') space = 1;
            else if (*fmt == '0') zero = 1;
            else if (*fmt == '#') alt = 1;
            else break;
            fmt++;
        }
        int width = 0;
        while (*fmt >= '0' && *fmt <= '9') width = width * 10 + (*fmt++ - '0');
        int prec = -1;
        if (*fmt == '.') {
            fmt++;
            prec = 0;
            while (*fmt >= '0' && *fmt <= '9') prec = prec * 10 + (*fmt++ - '0');
        }
        int lmod = 0;
        while (*fmt == 'l') { lmod++; fmt++; }
        if (*fmt == 'h') { fmt++; if (*fmt == 'h') fmt++; }
        else if (*fmt == 'z' || *fmt == 't' || *fmt == 'j') fmt++;

        char conv = *fmt ? *fmt++ : 0;
        uint32_t arg = next_arg(&a);
        char spec[40];
        char tmp[256];

        if (conv == 'd' || conv == 'i') {
            make_spec(spec, sizeof spec, left, plus, space, zero, alt, width, prec, 2, 'd');
            snprintf(tmp, sizeof tmp, spec, (long long)(int32_t)arg);
            out_text(&out, tmp);
        } else if (conv == 'u' || conv == 'x' || conv == 'X' || conv == 'o') {
            make_spec(spec, sizeof spec, left, plus, space, zero, alt, width, prec, 2, conv);
            snprintf(tmp, sizeof tmp, spec, (unsigned long long)arg);
            out_text(&out, tmp);
        } else if (conv == 'p') {
            make_spec(spec, sizeof spec, left, 0, space, zero, alt, width, prec, 2, 'x');
            snprintf(tmp, sizeof tmp, spec, (unsigned long long)arg);
            out_text(&out, tmp);
        } else if (conv == 'c') {
            make_spec(spec, sizeof spec, left, 0, 0, 0, 0, width, prec, 0, 'c');
            snprintf(tmp, sizeof tmp, spec, (int)(arg & 0xffu));
            out_text(&out, tmp);
        } else if (conv == 's') {
            const char *s = (const char *)hostp(arg, 1);
            if (!s) s = "(null)";
            make_spec(spec, sizeof spec, left, 0, 0, 0, 0, width, prec, 0, 's');
            snprintf(tmp, sizeof tmp, spec, s);
            out_text(&out, tmp);
        } else if (conv == 'f' || conv == 'e' || conv == 'E' || conv == 'g' || conv == 'G') {
            uint32_t hi = next_arg(&a);
            uint32_t lo = arg;
            double d;
            memcpy(&d, &lo, 4);
            memcpy(((uint8_t *)&d) + 4, &hi, 4);
            make_spec(spec, sizeof spec, left, plus, space, zero, alt, width, prec, 0, conv);
            snprintf(tmp, sizeof tmp, spec, d);
            out_text(&out, tmp);
        } else {
            out_ch(&out, '%');
            if (conv) out_ch(&out, conv);
        }
    }
    out_finish(&out);
    return out.len;
}

static uint32_t guest_sprintf(Cpu *c, uint8_t *mem) {
    const char *fmt = (const char *)hostp(c->r1, 1);
    if (!fmt) return 0;
    uint32_t regs[2] = {c->r2, c->r3};
    uint32_t r = guest_format(c, mem, c->r0, 4096u, fmt, 2, regs);
    if (getenv("FAST5_TEXTURE_TRACE") && c->r0 + 20u < guest_mem_size) {
        static int z_logs = 0;
        const char *out = (const char *)(mem + c->r0);
        int allz = out[0] == '0';
        for (int i = 0; i < 20 && allz; i++) if (out[i] != '0') allz = 0;
        if (allz && (out[20] == 0 || out[20] == '_' || out[20] == '-') && z_logs < 20) {
            fprintf(stderr, "zeroName: fmt='%s' r2=%08x r3=%08x lr=0x%06x out='%s'\n",
                    fmt, c->r2, c->r3, c->lr, out);
            z_logs++;
        }
    }
    return r;
}

static uint32_t guest_snprintf(Cpu *c, uint8_t *mem) {
    const char *fmt = (const char *)hostp(c->r2, 1);
    if (!fmt) return 0;
    uint32_t regs[1] = {c->r3};
    uint32_t r = guest_format(c, mem, c->r0, c->r1, fmt, 1, regs);
    if (getenv("FAST5_TEXTURE_TRACE") && c->r0 + 20u < guest_mem_size) {
        static int zs_logs = 0;
        const char *out = (const char *)(mem + c->r0);
        int allz = out[0] == '0';
        for (int i = 0; i < 20 && allz; i++) if (out[i] != '0') allz = 0;
        if (allz && (out[20] == 0 || out[20] == '_' || out[20] == '-') && zs_logs < 20) {
            fprintf(stderr, "zeroName(sn): fmt='%s' r3=%08x lr=0x%06x out='%s'\n",
                    fmt, c->r3, c->lr, out);
            zs_logs++;
        }
    }
    return r;
}

static uint32_t guest_sscanf(Cpu *c, uint8_t *mem) {    const char *s = (const char *)hostp(c->r0, 1);
    const char *fmt = (const char *)hostp(c->r1, 1);
    if (!s || !fmt) return 0;
    uint32_t argi = 0, assigned = 0;

    while (*fmt) {
        if (*fmt == ' ' || *fmt == '\t') {
            while (*s == ' ' || *s == '\t' || *s == '\n') s++;
            fmt++;
            continue;
        }
        if (*fmt != '%') {
            if (*s != *fmt) break;
            s++; fmt++;
            continue;
        }
        fmt++;
        if (*fmt == '%') {
            if (*s != '%') break;
            s++; fmt++;
            continue;
        }
        int width = 0;
        while (*fmt >= '0' && *fmt <= '9') width = width * 10 + (*fmt++ - '0');
        int is_long = 0;
        if (*fmt == 'l') { is_long = 1; fmt++; }
        else if (*fmt == 'h') { fmt++; }
        char conv = *fmt ? *fmt++ : 0;
        while (*s == ' ' || *s == '\t' || *s == '\n') s++;

        if (conv == 'u' || conv == 'd' || conv == 'i' || conv == 'x' || conv == 'X') {
            char *end = NULL;
            unsigned long v = (conv == 'x' || conv == 'X')
                                  ? strtoul(s, &end, 16)
                                  : strtoul(s, &end, 10);
            if (end == s) break;
            s = end;
            uint32_t dst = sscanf_arg(c, mem, &argi);
            w32(mem, dst, (uint32_t)v);
            assigned++;
        } else if (conv == 'f' || conv == 'e' || conv == 'g') {
            char *end = NULL;
            double v = strtod(s, &end);
            if (end == s) break;
            s = end;
            uint32_t dst = sscanf_arg(c, mem, &argi);
            if (is_long) {
                uint32_t lo, hi;
                f64_to(v, &lo, &hi);
                w32(mem, dst, lo);
                w32(mem, dst + 4u, hi);
            } else {
                float f = (float)v;
                uint32_t bits;
                memcpy(&bits, &f, 4);
                w32(mem, dst, bits);
            }
            assigned++;
        } else if (conv == 's') {
            uint32_t dst = sscanf_arg(c, mem, &argi);
            int n = 0;
            while (*s && *s != ' ' && *s != '\t' && *s != '\n' && (!width || n < width)) {
                if (dst + (uint32_t)n < guest_mem_size) mem[dst + n] = *s;
                n++; s++;
            }
            if (dst + (uint32_t)n < guest_mem_size) mem[dst + n] = 0;
            assigned++;
        } else if (conv == 'c') {
            uint32_t dst = sscanf_arg(c, mem, &argi);
            if (!*s) break;
            if (dst < guest_mem_size) mem[dst] = *s;
            s++;
            assigned++;
        } else {
            break;
        }
    }
    return assigned;
}

static void *hostp(uint32_t addr, uint32_t n) {
    if (!g_mem) {
        return NULL;
    }
    if (addr > guest_mem_size || n > guest_mem_size - addr) {
        guest_fault(addr, 0);
        return NULL;
    }
    return g_mem + addr;
}

/*
 * Guest heap: boundary-tagged allocator with coalescing.
 *
 * The game allocates and frees texture/scene memory on every state change. The previous
 * implementation kept a flat free list and never merged neighbouring blocks, so memory returned
 * by a torn-down scene could never satisfy a later large request. Measured over one
 * race -> menu -> race session: live memory peaked at 43.8 MiB, but the arena bumped to 1014 MiB
 * while ~750k free fragments held 765 MiB with a largest block of only 4.7 MiB. A 5.6 MB PVR
 * decode then failed with 765 MiB free. Boundary tags let a freed block merge with its physical
 * neighbours, so large blocks stay available.
 *
 * Block layout (8-byte aligned, header 8 bytes):
 *     +------------+------------+------------------------------+
 *     | size|flags | prev_size  | payload ...                  |
 *     +------------+------------+------------------------------+
 *   size      payload bytes; low two bits are USED and PREV_USED
 *   prev_size payload bytes of the physically previous block
 * A free block stores its bin links as two Block* at the start of its payload.
 */
#define BLK_HDR 8u
#define BLK_USED 1u
#define BLK_PREV_USED 2u
#define BLK_ALIGN 7u
#define NBINS 32

typedef struct Block {
    uint32_t size;
    uint32_t prev_size;
} Block;

/* Bin links live in a free block's payload, so a free block must be at least this big. On a
 * 64-bit host two pointers are 16 bytes; using 8 here wrote the links over the next block's
 * header and corrupted the heap. */
typedef struct FreeLinks {
    Block *next;
    Block *prev;
} FreeLinks;

#define BLK_MIN_PAYLOAD ((uint32_t)sizeof(FreeLinks))

static Block *g_bins[NBINS];
static Block *g_last;      /* last physical block, so the top can grow in place */
static uint32_t g_top;     /* one past the last physical block */
static uint32_t g_live;
static uint32_t g_peak_live;
static CRITICAL_SECTION g_alloc_lock;
static INIT_ONCE g_alloc_once = INIT_ONCE_STATIC_INIT;

static BOOL CALLBACK alloc_init_once(PINIT_ONCE once, PVOID parameter, PVOID *context) {
    (void)once;
    (void)parameter;
    (void)context;
    InitializeCriticalSection(&g_alloc_lock);
    return TRUE;
}

static void ensure_alloc_lock(void) {
    InitOnceExecuteOnce(&g_alloc_once, alloc_init_once, NULL, NULL);
}

static inline uint32_t blk_size(const Block *b) { return b->size & ~7u; }
static inline int blk_used(const Block *b) { return (b->size & BLK_USED) != 0; }
static inline int blk_prev_used(const Block *b) { return (b->size & BLK_PREV_USED) != 0; }
static inline void blk_set(Block *b, uint32_t payload, int used, int prev_used) {
    b->size = payload | (used ? BLK_USED : 0u) | (prev_used ? BLK_PREV_USED : 0u);
}
static inline uint32_t blk_total(const Block *b) { return BLK_HDR + blk_size(b); }
static inline Block *blk_next(Block *b) {
    return (Block *)((uint8_t *)b + blk_total(b));
}
static inline Block *blk_prev(Block *b) {
    return (Block *)((uint8_t *)b - BLK_HDR - b->prev_size);
}
static inline void *blk_payload(Block *b) { return (uint8_t *)b + BLK_HDR; }
static inline FreeLinks *blk_links(Block *b) { return (FreeLinks *)blk_payload(b); }

/* Bin i holds blocks whose payload is at least 2^i. Values above 2^(NBINS-1) go in the last bin,
 * so a request always looks in the bin matching (or just below) its size. */
static int bin_index(uint32_t n) {
    int i = 0;
    uint32_t v = n >> 1;
    while (v) { v >>= 1; i++; }
    if (i >= NBINS) i = NBINS - 1;
    return i;
}

static void bin_insert(Block *b) {
    int i = bin_index(blk_size(b));
    FreeLinks *l = blk_links(b);
    l->prev = NULL;
    l->next = g_bins[i];
    if (g_bins[i]) blk_links(g_bins[i])->prev = b;
    g_bins[i] = b;
}

static void bin_remove(Block *b) {
    int i = bin_index(blk_size(b));
    FreeLinks *l = blk_links(b);
    if (l->prev) blk_links(l->prev)->next = l->next;
    else g_bins[i] = l->next;
    if (l->next) blk_links(l->next)->prev = l->prev;
    l->next = NULL;
    l->prev = NULL;
}

static Block *bin_find(uint32_t need) {
    for (int i = bin_index(need); i < NBINS; i++) {
        for (Block *b = g_bins[i]; b; b = blk_links(b)->next) {
            if (blk_size(b) >= need) return b;
        }
    }
    return NULL;
}

/* Grow the arena. Returns an unlinked free block of at least `need` payload bytes, or NULL. */
static Block *heap_extend(uint32_t need) {
    uint32_t want = need + BLK_HDR;
    uint32_t chunk = 256u * 1024u;
    if (want > chunk) chunk = (want + 0xFFFFu) & ~0xFFFFu;

    if (g_top == 0) {
        g_top = GUEST_HEAP_BASE;
        if ((uint8_t *)g_mem + g_top < (uint8_t *)g_mem) return NULL;
    }

    if (g_last && !blk_used(g_last)) {
        /* Top block is already free: extend it in place rather than fragmenting. */
        Block *b = g_last;
        bin_remove(b);
        uint32_t grow = chunk;
        if (g_top + grow > guest_mem_size) grow = guest_mem_size - g_top;
        if (grow > 0) {
            uint32_t sz = blk_size(b);
            int pu = blk_prev_used(b);
            blk_set(b, sz + grow, 0, pu);
            g_top += grow;
        }
        return blk_size(b) >= need ? b : NULL;
    }

    if (g_top + chunk > guest_mem_size) {
        chunk = guest_mem_size - g_top;
        if (chunk < want) return NULL;
    }
    Block *b = (Block *)(g_mem + g_top);
    blk_set(b, chunk - BLK_HDR, 0, g_last ? blk_used(g_last) : 1);
    b->prev_size = g_last ? blk_size(g_last) : 0;
    g_last = b;
    g_top += chunk;
    return b;
}

/* Merge a free block with its free neighbours and file it in a bin.
 * Invariant: g_live counts only USED payload bytes, so callers adjust g_live themselves and this
 * function never touches it (a block reaching here is already outside the live total). */
static void heap_free_block(Block *b) {
    uint32_t size = blk_size(b);

    if (g_last != b) {
        Block *n = blk_next(b);
        if (!blk_used(n)) {
            bin_remove(n);
            uint32_t merged = size + blk_total(n);
            int pu = blk_prev_used(b);
            blk_set(b, merged, 0, pu);
            size = merged;
            if (g_last == n) g_last = b;
        }
    }
    if (!blk_prev_used(b)) {
        Block *p = blk_prev(b);
        bin_remove(p);
        uint32_t merged = blk_size(p) + blk_total(b);
        int pu = blk_prev_used(p);
        blk_set(p, merged, 0, pu);
        if (g_last == b) g_last = p;
        b = p;
    }
    if (g_last != b) {
        Block *n = blk_next(b);
        n->prev_size = blk_size(b);
        n->size &= ~BLK_PREV_USED;
    }
    bin_insert(b);
}

static uint32_t gmalloc_locked(uint32_t n) {
    n = (n + BLK_ALIGN) & ~BLK_ALIGN;
    /* Every block must be able to hold bin links once it is freed. */
    if (n < BLK_MIN_PAYLOAD) n = BLK_MIN_PAYLOAD;

    Block *b = bin_find(n);
    if (b) {
        bin_remove(b);
    } else {
        b = heap_extend(n);
        if (!b) {
            static int oom_reported = 0;
            if (!oom_reported) {
                fprintf(stderr,
                        "import: guest heap exhausted (requested %u bytes, arena at 0x%08x of "
                        "0x%08x, live %u bytes)\n", n, g_top, guest_mem_size, g_live);
                oom_reported = 1;
            }
            return 0;
        }
    }

    uint32_t size = blk_size(b);
    int prev_used = blk_prev_used(b);
    if (size >= n + BLK_HDR + BLK_MIN_PAYLOAD) {
        int was_last = (g_last == b);
        Block *rest = (Block *)((uint8_t *)b + BLK_HDR + n);
        blk_set(rest, size - n - BLK_HDR, 0, 1);
        rest->prev_size = n;
        blk_set(b, n, 1, prev_used);
        if (was_last) {
            g_last = rest;
        } else {
            Block *nn = blk_next(rest);
            nn->prev_size = blk_size(rest);
            nn->size &= ~BLK_PREV_USED;
        }
        bin_insert(rest);
    } else {
        blk_set(b, size, 1, prev_used);
        if (g_last != b) {
            Block *nn = blk_next(b);
            nn->prev_size = size;
            nn->size |= BLK_PREV_USED;
        }
    }

    g_live += blk_size(b);
    if (g_live > g_peak_live) g_peak_live = g_live;
    return (uint32_t)((uint8_t *)blk_payload(b) - g_mem);
}

static void gfree_locked(uint32_t p) {
    if (p < GUEST_HEAP_BASE || p >= g_top) return;
    Block *b = (Block *)((uint8_t *)g_mem + p - BLK_HDR);
    if ((uint8_t *)b < (uint8_t *)g_mem + GUEST_HEAP_BASE) return;
    if (!blk_used(b)) return; /* double free */
    g_live -= blk_size(b);
    heap_free_block(b);
}

static uint32_t gmalloc(uint32_t n) {
    ensure_alloc_lock();
    EnterCriticalSection(&g_alloc_lock);
    uint32_t result = gmalloc_locked(n);
    LeaveCriticalSection(&g_alloc_lock);
    return result;
}

/* Heap accounting. All values are bytes.
 *   bump     - end of the arena in use (high-water mark)
 *   free     - total payload bytes on the free bins
 *   largest  - biggest single free block; a request above this cannot be satisfied
 *   blocks   - number of free blocks (fragmentation indicator)
 *   live     - bytes handed out and not yet freed
 *   peak     - highest `live` seen since startup
 */
void guest_heap_stats(uint32_t *bump, uint32_t *free_bytes, uint32_t *largest,
                      uint32_t *blocks, uint32_t *live, uint32_t *peak) {
    ensure_alloc_lock();
    EnterCriticalSection(&g_alloc_lock);
    uint32_t fb = 0, lg = 0, nb = 0;
    for (int i = 0; i < NBINS; i++) {
        for (Block *b = g_bins[i]; b; b = blk_links(b)->next) {
            uint32_t s = blk_size(b);
            fb += s;
            if (s > lg) lg = s;
            nb++;
        }
    }
    if (bump) *bump = g_top;
    if (free_bytes) *free_bytes = fb;
    if (largest) *largest = lg;
    if (blocks) *blocks = nb;
    if (live) *live = g_live;
    if (peak) *peak = g_peak_live;
    LeaveCriticalSection(&g_alloc_lock);
}

static void gfree(uint32_t p) {
    ensure_alloc_lock();
    EnterCriticalSection(&g_alloc_lock);
    gfree_locked(p);
    LeaveCriticalSection(&g_alloc_lock);
}

static uint32_t grealloc(uint32_t p, uint32_t n) {
    if (!p) return gmalloc(n);
    if (n == 0) { gfree(p); return 0; }

    ensure_alloc_lock();
    EnterCriticalSection(&g_alloc_lock);

    uint32_t result = 0;
    if (p >= GUEST_HEAP_BASE && p < g_top) {
        Block *b = (Block *)((uint8_t *)g_mem + p - BLK_HDR);
        if (blk_used(b)) {
            uint32_t want = (n + BLK_ALIGN) & ~BLK_ALIGN;
            uint32_t have = blk_size(b);
            if (have >= want) {
                /* Shrink in place when the tail is worth splitting off. */
                if (have >= want + BLK_HDR + BLK_MIN_PAYLOAD) {
                    int prev_used = blk_prev_used(b);
                    int was_last = (g_last == b);
                    Block *rest = (Block *)((uint8_t *)b + BLK_HDR + want);
                    blk_set(rest, have - want - BLK_HDR, 0, 1);
                    rest->prev_size = want;
                    blk_set(b, want, 1, prev_used);
                    if (was_last) {
                        g_last = rest;
                    } else {
                        Block *nn = blk_next(rest);
                        nn->prev_size = blk_size(rest);
                        nn->size &= ~BLK_PREV_USED;
                    }
                    g_live -= (have - want);   /* the split-off tail leaves the live total */
                    heap_free_block(rest);
                }
                result = p;
            } else {
                /* Grow in place by absorbing a free physical successor, else at the top. */
                Block *nxt = (g_last == b) ? NULL : blk_next(b);
                int absorbed = 0;
                uint32_t gained = 0;
                if (nxt && !blk_used(nxt) && have + blk_total(nxt) >= want) {
                    bin_remove(nxt);
                    gained = blk_total(nxt);
                    int prev_used = blk_prev_used(b);
                    blk_set(b, have + gained, 1, prev_used);
                    absorbed = 1;
                } else if (!nxt) {
                    /* Top block: extend the arena directly. */
                    uint32_t grow = want - have;
                    if (g_top + grow <= guest_mem_size) {
                        blk_set(b, want, 1, blk_prev_used(b));
                        g_top += grow;
                        gained = grow;
                        absorbed = 1;
                    }
                }
                if (absorbed) {
                    if (g_last != b) {
                        Block *nn = blk_next(b);
                        nn->prev_size = blk_size(b);
                        nn->size |= BLK_PREV_USED;
                    }
                    g_live += gained;          /* the absorbed space becomes live */
                    result = p;
                }
            }
        }
    }

    if (!result) {
        /* Not in place: allocate, copy, free. */
        uint32_t old_size = 0;
        if (p >= GUEST_HEAP_BASE && p < g_top) {
            Block *b = (Block *)((uint8_t *)g_mem + p - BLK_HDR);
            if (blk_used(b)) old_size = blk_size(b);
        }
        uint32_t q = gmalloc_locked(n);
        if (q) {
            uint32_t copy = old_size < n ? old_size : n;
            memcpy(g_mem + q, g_mem + p, copy);
            if (old_size) gfree_locked(p);
            result = q;
        }
    }

    LeaveCriticalSection(&g_alloc_lock);
    return result;
}

static int streq(const char *a, const char *b) {
    return strcmp(a, b) == 0;
}

static int is_prefix(const char *s, const char *p) {
    return strncmp(s, p, strlen(p)) == 0;
}

static float f32_of(uint32_t bits) {
    float f;
    memcpy(&f, &bits, 4);
    return f;
}

static uint32_t f32_bits(float f) {
    uint32_t b;
    memcpy(&b, &f, 4);
    return b;
}

static double f64_of(uint32_t lo, uint32_t hi) {
    uint64_t bits = ((uint64_t)hi << 32) | lo;
    double d;
    memcpy(&d, &bits, 8);
    return d;
}

static void f64_to(double d, uint32_t *lo, uint32_t *hi) {
    uint64_t bits;
    memcpy(&bits, &d, 8);
    *lo = (uint32_t)bits;
    *hi = (uint32_t)(bits >> 32);
}

/*
 * PLT lookup. The generated table is densely packed (stride 12, see imports_table.h), so the slot
 * index is pure arithmetic instead of a 300-entry scan on every import call.
 */
#define PLT_BASE 0x297d4cu
#define PLT_STRIDE 12u

static const ImportEntry *find_import(uint32_t plt) {
    if (plt < PLT_BASE) return NULL;
    uint32_t off = plt - PLT_BASE;
    if (off % PLT_STRIDE) return NULL;
    uint32_t idx = off / PLT_STRIDE;
    if (idx >= IMPORT_COUNT || k_import_table[idx].plt != plt) return NULL;
    return &k_import_table[idx];
}

/*
 * Import profiler (FAST5_PROFILE=1): per-import call counts and self time, dumped every 300 frames
 * and at exit. Time is measured around the dispatch body, so it captures the host work an import
 * performs (file reads, texture decode, GL upload) rather than the guest code that called it.
 */
static int g_profile = -1;
static uint32_t g_prof_calls[IMPORT_COUNT];
static uint64_t g_prof_ticks[IMPORT_COUNT];

static uint64_t prof_now(void) {
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return (uint64_t)t.QuadPart;
}

/*
 * Sleep on Windows is only accurate to the current timer resolution, which defaults to ~15.6 ms.
 * A guest that asks for 1-2 ms therefore parks for up to 15.6 ms per call — and the game sleeps
 * from asset-loading loops, so a stall that should take milliseconds takes seconds. timeBeginPeriod
 * raises the resolution to 1 ms for the process lifetime.
 */
static void timer_resolution_raise(void) {
    static int raised = -1;
    if (raised < 0) {
        raised = 1;
        timeBeginPeriod(1);
    }
}

/* usleep diagnostics: how much longer than requested each sleep actually took. */
static uint32_t g_sleep_calls;
static uint64_t g_sleep_req_us;
static uint64_t g_sleep_act_us;
static uint32_t g_sleep_bucket[8];   /* <1ms, 1-2, 2-4, 4-8, 8-16, 16-17, 17-33, >=33 ms */

static void sleep_bucket_count(uint32_t want_us) {
    uint32_t b = want_us < 1000u ? 0 : want_us < 2000u ? 1 : want_us < 4000u ? 2 :
                 want_us < 8000u ? 3 : want_us < 16000u ? 4 : want_us < 17000u ? 5 :
                 want_us < 33000u ? 6 : 7;
    g_sleep_bucket[b]++;
}

void guest_sleep_stats_dump(void) {
    static uint64_t freq = 0;
    if (!freq) {
        LARGE_INTEGER f;
        QueryPerformanceFrequency(&f);
        freq = (uint64_t)f.QuadPart;
    }
    if (!g_sleep_calls) return;
    fprintf(stderr,
            "[sleep] %u sleeps, asked %.1f ms, slept %.1f ms (%.2fx)\n",
            g_sleep_calls,
            (double)g_sleep_req_us / 1000.0,
            (double)g_sleep_act_us * 1000.0 / (double)freq,
            g_sleep_req_us ? (double)g_sleep_act_us * 1000.0 / (double)freq /
                             ((double)g_sleep_req_us / 1000.0) : 0.0);
    fprintf(stderr, "[sleep] requested buckets (ms): <1=%u 1-2=%u 2-4=%u 4-8=%u "
                    "8-16=%u 16-17=%u 17-33=%u 33+=%u\n",
            g_sleep_bucket[0], g_sleep_bucket[1], g_sleep_bucket[2], g_sleep_bucket[3],
            g_sleep_bucket[4], g_sleep_bucket[5], g_sleep_bucket[6], g_sleep_bucket[7]);
    fflush(stderr);
}

void guest_import_profile_dump(void) {
    static uint64_t freq = 0;
    if (!freq) {
        LARGE_INTEGER f;
        QueryPerformanceFrequency(&f);
        freq = (uint64_t)f.QuadPart;
    }
    uint64_t total_ticks = 0;
    uint32_t total_calls = 0;
    for (uint32_t i = 0; i < IMPORT_COUNT; i++) {
        total_ticks += g_prof_ticks[i];
        total_calls += g_prof_calls[i];
    }
    if (!total_calls) return;
    fprintf(stderr, "[prof] %u import calls, %.1f ms total\n",
            total_calls, (double)total_ticks * 1000.0 / (double)freq);
    /* Top 12 by self time. */
    for (int rank = 0; rank < 12; rank++) {
        uint32_t best = IMPORT_COUNT;
        uint64_t best_ticks = 0;
        for (uint32_t i = 0; i < IMPORT_COUNT; i++) {
            if (g_prof_ticks[i] > best_ticks) { best_ticks = g_prof_ticks[i]; best = i; }
        }
        if (best == IMPORT_COUNT) break;
        fprintf(stderr, "[prof] %-28s calls=%-9u %9.1f ms  (%.1f us/call)\n",
                k_import_table[best].name, g_prof_calls[best],
                (double)best_ticks * 1000.0 / (double)freq,
                (double)best_ticks * 1000000.0 / (double)freq / (double)g_prof_calls[best]);
        g_prof_ticks[best] = 0;
    }
    fflush(stderr);
}

/*
 * Hot import fast paths. memcmp and __aeabi_idiv alone account for ~45M calls in one scene load;
 * routing them through the generic name-compare dispatch costs more than the operations themselves.
 * These PLT addresses are the ones the shipped binary uses (see imports_table.h).
 */
#define PLT_MEMCMP 0x298484u
#define PLT_IDIV 0x298208u
#define PLT_UIDIV 0x2989dcu
#define PLT_MEMCPY 0x298694u
#define PLT_MEMSET 0x298340u
#define PLT_MEMOVE 0x297ea8u
#define PLT_STRLEN 0x297de8u

void call_import(Cpu *c, uint8_t *mem, uint32_t plt) {
    g_mem = mem;
    if (plt == PLT_MEMCMP) {
        c->r0 = (uint32_t)memcmp(mem + c->r0, mem + c->r1, c->r2);
        return;
    }
    if (plt == PLT_MEMCPY) {
        memcpy(mem + c->r0, mem + c->r1, c->r2);
        return;
    }
    if (plt == PLT_MEMSET) {
        memset(mem + c->r0, (int)c->r1, c->r2);
        return;
    }
    if (plt == PLT_MEMOVE) {
        memmove(mem + c->r0, mem + c->r1, c->r2);
        return;
    }
    if (plt == PLT_STRLEN) {
        c->r0 = (uint32_t)strlen((const char *)mem + c->r0);
        return;
    }
    if (plt == PLT_IDIV) {
        int32_t a = (int32_t)c->r0, b = (int32_t)c->r1;
        c->r0 = b ? (uint32_t)(a / b) : 0u;
        return;
    }
    if (plt == PLT_UIDIV) {
        c->r0 = c->r1 ? c->r0 / c->r1 : 0u;
        return;
    }
    const ImportEntry *e = find_import(plt);
    if (!e) {
        fprintf(stderr, "import: unknown PLT 0x%08x\n", plt);
        c->r0 = 0;
        return;
    }
    uint32_t idx = (uint32_t)(e - k_import_table);
    if (!g_seen[idx]) {
        g_seen[idx] = 1;
        if (g_verbose < 0) g_verbose = getenv("FAST5_IMPORT_TRACE") != NULL;
        if (g_verbose) {
            fprintf(stderr, "import: %s (0x%08x) r0=%08x r1=%08x r2=%08x r3=%08x\n",
                    e->name, plt, c->r0, c->r1, c->r2, c->r3);
        }
    }
    if (g_profile < 0) g_profile = getenv("FAST5_PROFILE") != NULL;
    if (!g_profile) {
        import_dispatch(c, mem, e->name);
        return;
    }
    uint64_t t0 = prof_now();
    import_dispatch(c, mem, e->name);
    g_prof_calls[idx]++;
    g_prof_ticks[idx] += prof_now() - t0;
}

static void import_dispatch(Cpu *c, uint8_t *mem, const char *n) {

    /* --- compiler helper functions (softfp ABI) --- */
    if (streq(n, "__aeabi_idivmod")) {
        int32_t a = (int32_t)c->r0, b = (int32_t)c->r1;
        if (!b) { c->r0 = 0; c->r1 = 0; return; }
        c->r0 = (uint32_t)(a / b);
        c->r1 = (uint32_t)(a % b);
        return;
    }
    if (streq(n, "__aeabi_uidivmod")) {
        uint32_t a = c->r0, b = c->r1;
        if (!b) { c->r0 = 0; c->r1 = 0; return; }
        c->r0 = a / b;
        c->r1 = a % b;
        return;
    }
    if (streq(n, "__aeabi_idiv")) {
        int32_t a = (int32_t)c->r0, b = (int32_t)c->r1;
        c->r0 = b ? (uint32_t)(a / b) : 0;
        return;
    }
    if (streq(n, "__aeabi_uidiv")) {
        c->r0 = c->r1 ? c->r0 / c->r1 : 0;
        return;
    }
    if (streq(n, "__aeabi_ldivmod") || streq(n, "__aeabi_uldivmod")) {
        int64_t a = (int64_t)(((uint64_t)c->r1 << 32) | c->r0);
        int64_t b = (int64_t)(((uint64_t)c->r3 << 32) | c->r2);
        if (!b) { c->r0 = c->r1 = c->r2 = c->r3 = 0; return; }
        int64_t q = a / b, rem = a % b;
        c->r0 = (uint32_t)q; c->r1 = (uint32_t)((uint64_t)q >> 32);
        c->r2 = (uint32_t)rem; c->r3 = (uint32_t)((uint64_t)rem >> 32);
        return;
    }
    if (streq(n, "__aeabi_dadd") || streq(n, "__aeabi_dsub") ||
        streq(n, "__aeabi_dmul") || streq(n, "__aeabi_ddiv")) {
        double a = f64_of(c->r0, c->r1), b = f64_of(c->r2, c->r3), r;
        if (strstr(n, "add")) r = a + b; else if (strstr(n, "sub")) r = a - b;
        else if (strstr(n, "mul")) r = a * b; else r = a / b;
        f64_to(r, &c->r0, &c->r1);
        return;
    }
    if (streq(n, "__aeabi_fadd") || streq(n, "__aeabi_fsub") ||
        streq(n, "__aeabi_fmul") || streq(n, "__aeabi_fdiv")) {
        float a = f32_of(c->r0), b = f32_of(c->r1), r;
        if (strstr(n, "add")) r = a + b; else if (strstr(n, "sub")) r = a - b;
        else if (strstr(n, "mul")) r = a * b; else r = a / b;
        c->r0 = f32_bits(r);
        return;
    }
    if (streq(n, "__aeabi_i2f")) { c->r0 = f32_bits((float)(int32_t)c->r0); return; }
    if (streq(n, "__aeabi_i2d")) { double d = (double)(int32_t)c->r0; f64_to(d, &c->r0, &c->r1); return; }
    if (streq(n, "__aeabi_f2iz")) { c->r0 = (uint32_t)(int32_t)f32_of(c->r0); return; }
    if (streq(n, "__aeabi_d2iz")) { c->r0 = (uint32_t)(int32_t)f64_of(c->r0, c->r1); return; }
    if (streq(n, "__aeabi_f2d")) { double d = (double)f32_of(c->r0); f64_to(d, &c->r0, &c->r1); return; }
    if (streq(n, "__aeabi_d2f")) { c->r0 = f32_bits((float)f64_of(c->r0, c->r1)); return; }
    if (streq(n, "__aeabi_fcmpeq")) { c->r0 = f32_of(c->r0) == f32_of(c->r1); return; }
    if (streq(n, "__aeabi_fcmplt")) { c->r0 = f32_of(c->r0) < f32_of(c->r1); return; }
    if (streq(n, "__aeabi_fcmple")) { c->r0 = f32_of(c->r0) <= f32_of(c->r1); return; }
    if (streq(n, "__aeabi_fcmpgt")) { c->r0 = f32_of(c->r0) > f32_of(c->r1); return; }
    if (streq(n, "__aeabi_fcmpge")) { c->r0 = f32_of(c->r0) >= f32_of(c->r1); return; }

    /* --- C runtime --- */
    if (streq(n, "malloc")) { c->r0 = gmalloc(c->r0); return; }
    if (streq(n, "calloc")) {
        uint32_t total = c->r0 * c->r1;
        uint32_t p = gmalloc(total);
        memset(g_mem + p, 0, total);
        c->r0 = p;
        return;
    }
    if (streq(n, "realloc")) { c->r0 = grealloc(c->r0, c->r1); return; }
    if (streq(n, "free")) { gfree(c->r0); return; }
    if (streq(n, "memcpy")) { memcpy(hostp(c->r0, c->r2), hostp(c->r1, c->r2), c->r2); return; }
    if (streq(n, "memmove")) { memmove(hostp(c->r0, c->r2), hostp(c->r1, c->r2), c->r2); return; }
    if (streq(n, "memset")) { memset(hostp(c->r0, c->r2), (int)c->r1, c->r2); return; }
    if (streq(n, "memchr")) {
        void *p = memchr(hostp(c->r0, c->r2), (int)c->r1, c->r2);
        c->r0 = p ? (uint32_t)((uint8_t *)p - g_mem) : 0;
        return;
    }
    if (streq(n, "memcmp")) { c->r0 = (uint32_t)memcmp(hostp(c->r0, c->r2), hostp(c->r1, c->r2), c->r2); return; }
    if (streq(n, "strlen")) { c->r0 = (uint32_t)strlen((const char *)hostp(c->r0, 1)); return; }
    if (streq(n, "strcmp")) { c->r0 = (uint32_t)strcmp((const char *)hostp(c->r0, 1), (const char *)hostp(c->r1, 1)); return; }
    if (streq(n, "strncmp")) { c->r0 = (uint32_t)strncmp((const char *)hostp(c->r0, 1), (const char *)hostp(c->r1, 1), c->r2); return; }
    if (streq(n, "strcasecmp")) { c->r0 = (uint32_t)strcasecmp((const char *)hostp(c->r0, 1), (const char *)hostp(c->r1, 1)); return; }
    if (streq(n, "strncasecmp")) { c->r0 = (uint32_t)strncasecmp((const char *)hostp(c->r0, 1), (const char *)hostp(c->r1, 1), c->r2); return; }
    if (streq(n, "strchr")) {
        char *p = strchr((char *)hostp(c->r0, 1), (int)c->r1);
        c->r0 = p ? (uint32_t)(p - (char *)g_mem) : 0;
        return;
    }
    if (streq(n, "strrchr")) {
        char *p = strrchr((char *)hostp(c->r0, 1), (int)c->r1);
        c->r0 = p ? (uint32_t)(p - (char *)g_mem) : 0;
        return;
    }
    if (streq(n, "strstr")) {
        char *p = strstr((char *)hostp(c->r0, 1), (const char *)hostp(c->r1, 1));
        c->r0 = p ? (uint32_t)(p - (char *)g_mem) : 0;
        return;
    }
    if (streq(n, "strcpy")) { strcpy((char *)hostp(c->r0, 1), (const char *)hostp(c->r1, 1)); return; }
    if (streq(n, "strncpy")) { strncpy((char *)hostp(c->r0, 1), (const char *)hostp(c->r1, 1), c->r2); return; }
    if (streq(n, "strcat")) { strcat((char *)hostp(c->r0, 1), (const char *)hostp(c->r1, 1)); return; }
    if (streq(n, "strncat")) {
        /*
         * Missing import, and a load-bearing one: FormatNumber appends the digits of every number
         * with strncat but writes group separators with memcpy. As a no-op the digits vanished and
         * only separators survived, so FormatMoney produced "$," and the cash counter rendered
         * blank. Appends at most c->r2 bytes, stops at the source NUL, returns dest (already r0).
         */
        char *dst = (char *)hostp(c->r0, 1);
        const char *src = (const char *)hostp(c->r1, 1);
        size_t k = c->r2;
        char *p = dst + strlen(dst);
        while (k-- && *src) *p++ = *src++;
        *p = '\0';
        return;
    }
    if (streq(n, "sprintf")) { c->r0 = guest_sprintf(c, mem); return; }
    if (streq(n, "snprintf")) { c->r0 = guest_snprintf(c, mem); return; }
    if (streq(n, "sscanf")) { c->r0 = guest_sscanf(c, mem); return; }
    if (streq(n, "atoi")) {
        const char *s = (const char *)hostp(c->r0, 1);
        c->r0 = s ? (uint32_t)(int32_t)atoi(s) : 0;
        return;
    }
    if (streq(n, "strtol")) {
        const char *s = (const char *)hostp(c->r0, 1);
        char *end = NULL;
        long v = s ? strtol(s, &end, (int)c->r2) : 0;
        if (c->r1) w32(mem, c->r1, end ? (uint32_t)((uint8_t *)end - g_mem) : 0);
        c->r0 = (uint32_t)(int32_t)v;
        return;
    }
    if (streq(n, "strtod")) {
        const char *s = (const char *)hostp(c->r0, 1);
        char *end = NULL;
        double v = s ? strtod(s, &end) : 0.0;
        if (c->r1) w32(mem, c->r1, end ? (uint32_t)((uint8_t *)end - g_mem) : 0);
        f64_to(v, &c->r0, &c->r1);
        return;
    }
    if (streq(n, "strtok")) {
        char *s = c->r0 ? (char *)hostp(c->r0, 1) : NULL;
        const char *delim = (const char *)hostp(c->r1, 1);
        char *p = delim ? strtok(s, delim) : NULL;
        c->r0 = p ? (uint32_t)(p - (char *)g_mem) : 0;
        return;
    }

    /* --- 48-bit pseudo-random: CGenericBaker names every batch material with 20 chars from
     * lrand48(); without it every name is "00000000000000000000" and all materials collide. --- */
    {
        static uint64_t s48 = 0x1234ABCD330EULL;
        if (streq(n, "lrand48")) {
            s48 = (s48 * 0x5DEECE66DULL + 0xBULL) & 0xFFFFFFFFFFFFULL;
            c->r0 = (uint32_t)(s48 >> 17);
            return;
        }
        if (streq(n, "drand48")) {
            s48 = (s48 * 0x5DEECE66DULL + 0xBULL) & 0xFFFFFFFFFFFFULL;
            double d = (double)s48 / 281474976710656.0;
            union { double d; uint64_t u; } u;
            u.d = d;
            c->r0 = (uint32_t)u.u;
            c->r1 = (uint32_t)(u.u >> 32);
            return;
        }
        if (streq(n, "srand48")) {
            s48 = (((uint64_t)(uint32_t)c->r0) << 16) | 0x330EULL;
            return;
        }
        if (streq(n, "seed48")) {
            uint32_t old[3] = { (uint32_t)(s48 & 0xffffu), (uint32_t)((s48 >> 16) & 0xffffu),
                                (uint32_t)((s48 >> 32) & 0xffffu) };
            uint32_t src = c->r0;
            if (src + 6u < guest_mem_size) {
                uint32_t s0 = (uint32_t)mem[src] | ((uint32_t)mem[src + 1u] << 8);
                uint32_t s1 = (uint32_t)mem[src + 2u] | ((uint32_t)mem[src + 3u] << 8);
                uint32_t s2 = (uint32_t)mem[src + 4u] | ((uint32_t)mem[src + 5u] << 8);
                s48 = (((uint64_t)s2 << 32) | ((uint64_t)s1 << 16) | s0) & 0xFFFFFFFFFFFFULL;
            }
            if (src + 6u < guest_mem_size) {
                mem[src] = (uint8_t)(old[0] & 0xffu); mem[src + 1u] = (uint8_t)(old[0] >> 8);
                mem[src + 2u] = (uint8_t)(old[1] & 0xffu); mem[src + 3u] = (uint8_t)(old[1] >> 8);
                mem[src + 4u] = (uint8_t)(old[2] & 0xffu); mem[src + 5u] = (uint8_t)(old[2] >> 8);
            }
            c->r0 = src;
            return;
        }
    }

    /* --- time --- */
    if (streq(n, "gettimeofday")) {
        struct timeval tv;
        gettimeofday(&tv, NULL);
        w32(mem, c->r0, (uint32_t)tv.tv_sec);
        w32(mem, c->r0 + 4u, (uint32_t)tv.tv_usec);
        if (c->r1) {
            w32(mem, c->r1, 0);
            w32(mem, c->r1 + 4u, 0);
        }
        c->r0 = 0;
        return;
    }
    if (streq(n, "time")) {
        time_t t = time(NULL);
        if (c->r0) w32(mem, c->r0, (uint32_t)t);
        c->r0 = (uint32_t)t;
        return;
    }
    if (streq(n, "localtime_r") || streq(n, "gmtime_r")) {
        int32_t t = (int32_t)r32(mem, c->r0);
        time_t tt = (time_t)t;
        struct tm out;
        memset(&out, 0, sizeof out);
        if (streq(n, "gmtime_r")) gmtime_r(&tt, &out); else localtime_r(&tt, &out);
        uint32_t dst = c->r1;
        w32(mem, dst + 0u, (uint32_t)out.tm_sec);
        w32(mem, dst + 4u, (uint32_t)out.tm_min);
        w32(mem, dst + 8u, (uint32_t)out.tm_hour);
        w32(mem, dst + 12u, (uint32_t)out.tm_mday);
        w32(mem, dst + 16u, (uint32_t)out.tm_mon);
        w32(mem, dst + 20u, (uint32_t)out.tm_year);
        w32(mem, dst + 24u, (uint32_t)out.tm_wday);
        w32(mem, dst + 28u, (uint32_t)out.tm_yday);
        w32(mem, dst + 32u, (uint32_t)out.tm_isdst);
        c->r0 = dst;
        return;
    }

    /* --- stdio / VFS --- */
    if (streq(n, "fopen")) {
        const char *path = (const char *)hostp(c->r0, 1);
        const char *mode = (const char *)hostp(c->r1, 1);
        c->r0 = vfs_open(path, mode);
        return;
    }
    if (streq(n, "fread") || streq(n, "fwrite")) {
        uint64_t total = (uint64_t)c->r1 * c->r2;
        void *buf = hostp(c->r0, (uint32_t)total);
        if (!buf) { c->r0 = 0; return; }
        c->r0 = streq(n, "fread") ? vfs_read(c->r3, buf, c->r1, c->r2)
                                  : vfs_write(c->r3, buf, c->r1, c->r2);
        return;
    }
    if (streq(n, "fclose")) { c->r0 = (uint32_t)vfs_close(c->r0); return; }
    if (streq(n, "fseek")) { c->r0 = (uint32_t)vfs_seek(c->r0, (int32_t)c->r1, (int)c->r2); return; }
    if (streq(n, "ftell")) { c->r0 = (uint32_t)vfs_tell(c->r0); return; }
    if (streq(n, "feof")) { c->r0 = (uint32_t)vfs_eof(c->r0); return; }
    if (streq(n, "ferror")) { c->r0 = (uint32_t)vfs_error(c->r0); return; }
    if (streq(n, "fgetc")) { c->r0 = (uint32_t)vfs_getc(c->r0); return; }
    if (streq(n, "fputc")) { c->r0 = (uint32_t)vfs_putc((int)c->r0, c->r1); return; }
    if (streq(n, "fgets")) {
        char *dst = (char *)hostp(c->r0, c->r1);
        c->r0 = dst ? (uint32_t)(vfs_gets(dst, (int)c->r1, c->r2) ? c->r0 : 0) : 0;
        return;
    }
    if (streq(n, "fputs")) {
        const char *s = (const char *)hostp(c->r0, 1);
        c->r0 = s ? (uint32_t)vfs_puts(s, c->r1) : 0;
        return;
    }
    if (streq(n, "fileno")) { c->r0 = (uint32_t)vfs_fileno(c->r0); return; }
    if (streq(n, "fdopen")) {
        const char *mode = (const char *)hostp(c->r1, 1);
        c->r0 = vfs_fdopen((int)c->r0, mode);
        return;
    }
    if (streq(n, "fstat")) { c->r0 = (uint32_t)vfs_fstat_fd((int)c->r0, hostp(c->r1, 64)); return; }
    if (streq(n, "lseek")) { c->r0 = (uint32_t)vfs_lseek_fd((int)c->r0, (int32_t)c->r1, (int)c->r2); return; }
    if (streq(n, "read")) {
        void *dst = hostp(c->r1, c->r2);
        c->r0 = dst ? (uint32_t)vfs_read_fd((int)c->r0, dst, c->r2) : 0;
        return;
    }
    if (streq(n, "write")) {
        const void *src = hostp(c->r1, c->r2);
        c->r0 = src ? (uint32_t)vfs_write_fd((int)c->r0, src, c->r2) : 0;
        return;
    }
    if (streq(n, "close")) { c->r0 = (uint32_t)vfs_close_fd((int)c->r0); return; }
    if (streq(n, "chdir")) { vfs_chdir((const char *)hostp(c->r0, 1)); c->r0 = 0; return; }

    if (streq(n, "__errno")) { c->r0 = g_errno; return; }
    if (streq(n, "exit") || streq(n, "_exit") || streq(n, "abort") || streq(n, "__stack_chk_fail")) {
        fprintf(stderr, "import: %s(%u) -> terminating\n", n, c->r0);
        exit(3);
    }
    if (streq(n, "__cxa_atexit") || streq(n, "__cxa_finalize")) { c->r0 = 0; return; }
    if (streq(n, "__cxa_pure_virtual")) { fprintf(stderr, "import: pure virtual\n"); exit(4); }

    /* --- pthread subset over Win32 --- */
    if (streq(n, "pthread_once")) {
        uint32_t once = c->r0, fn = c->r1;
        if (r32(mem, once) != 2u) {
            w32(mem, once, 2u);
            Cpu saved = *c;
            c->r0 = 0;
            c->lr = 0xDEADBEE0u;
            call_target(c, mem, fn);
            *c = saved;
        }
        c->r0 = 0;
        return;
    }
#ifdef _WIN32
    if (streq(n, "pthread_create")) {
        c->r0 = (uint32_t)thread_win32_create(mem, guest_mem_size, c->r0, c->r2, c->r3);
        return;
    }
    if (streq(n, "pthread_join")) {
        c->r0 = (uint32_t)thread_win32_join(mem, c->r0, c->r1);
        return;
    }
    if (streq(n, "pthread_self")) { c->r0 = thread_win32_self(); return; }
    if (streq(n, "pthread_exit")) { ExitThread(0); return; }
    if (streq(n, "pthread_mutex_init")) {
        c->r0 = (uint32_t)thread_win32_mutex_init(mem, guest_mem_size, c->r0);
        return;
    }
    if (streq(n, "pthread_mutex_lock")) {
        c->r0 = (uint32_t)thread_win32_mutex_lock(mem, guest_mem_size, c->r0);
        return;
    }
    if (streq(n, "pthread_mutex_trylock")) {
        c->r0 = (uint32_t)thread_win32_mutex_trylock(mem, guest_mem_size, c->r0);
        return;
    }
    if (streq(n, "pthread_mutex_unlock")) {
        c->r0 = (uint32_t)thread_win32_mutex_unlock(mem, guest_mem_size, c->r0);
        return;
    }
    if (streq(n, "pthread_mutex_destroy")) {
        c->r0 = (uint32_t)thread_win32_mutex_destroy(mem, guest_mem_size, c->r0);
        return;
    }
    if (streq(n, "usleep")) {
        uint32_t want_us = c->r0;
        uint64_t t0 = prof_now();
        if (want_us < 1000u) {
            /* Sub-millisecond request. Windows cannot sleep shorter than the timer tick, so
             * Sleep(1) costs a full millisecond — and the engine spin-polls for ready work this
             * way (115k such calls in one menu load, i.e. over a minute of pure stall). Yield the
             * remainder of the time slice instead: the work being polled for is already done. */
            SwitchToThread();
        } else {
            timer_resolution_raise();
            /* Round up so the guest gets at least the delay it asked for. */
            Sleep((DWORD)((want_us + 999u) / 1000u));
        }
        if (g_profile) {
            g_sleep_calls++;
            g_sleep_req_us += want_us;
            g_sleep_act_us += prof_now() - t0;
            sleep_bucket_count(want_us);
            /* A long sleep is the engine waiting on something. Record who asked, so a stall can be
               attributed to a caller rather than guessed at. */
            if (want_us >= 4000u) {
                static uint32_t seen_lr[16], seen_us[16], seen_n[16];
                static int nseen;
                int found = -1;
                for (int k = 0; k < nseen; k++) {
                    if (seen_lr[k] == c->lr && seen_us[k] == want_us) { found = k; break; }
                }
                if (found < 0 && nseen < 16) {
                    found = nseen++;
                    seen_lr[found] = c->lr;
                    seen_us[found] = want_us;
                }
                if (found >= 0) {
                    seen_n[found]++;
                    if (seen_n[found] == 1 || (seen_n[found] % 500) == 0) {
                        fprintf(stderr, "[sleep] lr=0x%08x want=%u us  count=%u\n",
                                seen_lr[found], seen_us[found], seen_n[found]);
                    }
                }
            }
        }
        c->r0 = 0;
        return;
    }
    if (streq(n, "nanosleep")) {
        /* struct timespec { time_t tv_sec; long tv_nsec; } */
        uint32_t req = c->r0;
        uint64_t want_us = 0;
        if (req) {
            uint32_t sec = r32(mem, req), nsec = r32(mem, req + 4u);
            want_us = (uint64_t)sec * 1000000u + (uint64_t)nsec / 1000u;
        }
        uint64_t t0 = prof_now();
        if (want_us < 1000u) {
            SwitchToThread();     /* same sub-millisecond poll case as usleep above */
        } else {
            timer_resolution_raise();
            Sleep((DWORD)((want_us + 999u) / 1000u));
        }
        if (g_profile) {
            g_sleep_calls++;
            g_sleep_req_us += want_us;
            g_sleep_act_us += prof_now() - t0;
            sleep_bucket_count((uint32_t)want_us);
        }
        c->r0 = 0;
        return;
    }
#endif
    if (is_prefix(n, "pthread_")) { c->r0 = 0; return; }
    if (streq(n, "sem_init") || streq(n, "sem_wait") || streq(n, "sem_post") ||
        streq(n, "sem_destroy") || streq(n, "sem_trywait")) { c->r0 = 0; return; }

    /* --- logging --- */
    if (streq(n, "__android_log_print")) {
        const char *tag = (const char *)hostp(c->r1, 1);
        const char *fmt = (const char *)hostp(c->r2, 1);
        fprintf(stderr, "[android] %s: ", tag ? tag : "?");
        if (fmt) print_format(mem, fmt, c, (uint32_t[]){c->r3}, 1);
        fputc('\n', stderr);
        c->r0 = 0;
        return;
    }
    if (streq(n, "printf")) {
        const char *fmt = (const char *)hostp(c->r0, 1);
        if (fmt) print_format(mem, fmt, c, (uint32_t[]){c->r1, c->r2, c->r3}, 3);
        c->r0 = 0;
        return;
    }

    /* --- math (single precision) --- */
    if (streq(n, "sqrtf")) { c->r0 = f32_bits(sqrtf(f32_of(c->r0))); return; }
    if (streq(n, "sinf")) { c->r0 = f32_bits(sinf(f32_of(c->r0))); return; }
    if (streq(n, "cosf")) { c->r0 = f32_bits(cosf(f32_of(c->r0))); return; }
    if (streq(n, "atan2f")) { c->r0 = f32_bits(atan2f(f32_of(c->r0), f32_of(c->r1))); return; }
    if (streq(n, "powf")) { c->r0 = f32_bits(powf(f32_of(c->r0), f32_of(c->r1))); return; }
    if (streq(n, "fmodf")) { c->r0 = f32_bits(fmodf(f32_of(c->r0), f32_of(c->r1))); return; }

    /* --- math (double precision, softfp: first double r0:r1, second r2:r3) --- */
    if (streq(n, "sin")) { f64_to(sin(f64_of(c->r0, c->r1)), &c->r0, &c->r1); return; }
    if (streq(n, "cos")) { f64_to(cos(f64_of(c->r0, c->r1)), &c->r0, &c->r1); return; }
    if (streq(n, "tan")) { f64_to(tan(f64_of(c->r0, c->r1)), &c->r0, &c->r1); return; }
    if (streq(n, "asin")) { f64_to(asin(f64_of(c->r0, c->r1)), &c->r0, &c->r1); return; }
    if (streq(n, "acos")) { f64_to(acos(f64_of(c->r0, c->r1)), &c->r0, &c->r1); return; }
    if (streq(n, "atan")) { f64_to(atan(f64_of(c->r0, c->r1)), &c->r0, &c->r1); return; }
    if (streq(n, "atan2")) {
        f64_to(atan2(f64_of(c->r0, c->r1), f64_of(c->r2, c->r3)), &c->r0, &c->r1);
        return;
    }
    if (streq(n, "sqrt")) { f64_to(sqrt(f64_of(c->r0, c->r1)), &c->r0, &c->r1); return; }
    if (streq(n, "pow")) {
        f64_to(pow(f64_of(c->r0, c->r1), f64_of(c->r2, c->r3)), &c->r0, &c->r1);
        return;
    }
    if (streq(n, "exp")) { f64_to(exp(f64_of(c->r0, c->r1)), &c->r0, &c->r1); return; }
    if (streq(n, "log")) { f64_to(log(f64_of(c->r0, c->r1)), &c->r0, &c->r1); return; }
    if (streq(n, "log10")) { f64_to(log10(f64_of(c->r0, c->r1)), &c->r0, &c->r1); return; }
    if (streq(n, "floor")) { f64_to(floor(f64_of(c->r0, c->r1)), &c->r0, &c->r1); return; }
    if (streq(n, "ceil")) { f64_to(ceil(f64_of(c->r0, c->r1)), &c->r0, &c->r1); return; }
    if (streq(n, "fmod")) {
        f64_to(fmod(f64_of(c->r0, c->r1), f64_of(c->r2, c->r3)), &c->r0, &c->r1);
        return;
    }
    if (streq(n, "frexp")) {
        int e = 0;
        double r = frexp(f64_of(c->r0, c->r1), &e);
        if (c->r2 && c->r2 + 4u <= guest_mem_size) w32(mem, c->r2, (uint32_t)e);
        f64_to(r, &c->r0, &c->r1);
        return;
    }
    if (streq(n, "ldexp")) {
        f64_to(ldexp(f64_of(c->r0, c->r1), (int32_t)c->r2), &c->r0, &c->r1);
        return;
    }
    if (streq(n, "modf")) {
        double ip = 0.0;
        double fp = modf(f64_of(c->r0, c->r1), &ip);
        if (c->r2 && c->r2 + 8u <= guest_mem_size) {
            uint32_t lo, hi;
            f64_to(ip, &lo, &hi);
            w32(mem, c->r2, lo);
            w32(mem, c->r2 + 4u, hi);
        }
        f64_to(fp, &c->r0, &c->r1);
        return;
    }

    /* --- unwinder: recorded, not yet implemented --- */
    if (is_prefix(n, "_Unwind_")) { c->r0 = 0; return; }

    /* --- OpenGL ES 2.0 surface (bring-up: identity objects, plausible limits) --- */
#ifdef _WIN32
    if (n[0] == 'g' && n[1] == 'l' && gl_bridge_call(c, mem, n)) {
        return;
    }
#endif
    if (streq(n, "glGetError")) { c->r0 = 0; return; }
    if (streq(n, "glGetString")) {
        uint32_t a = 0;
        switch (c->r0) {
        case 0x1F00: a = 0x9A0100; put_str(g_mem, a, "Imagination Technologies"); break;
        case 0x1F01: a = 0x9A0200; put_str(g_mem, a, "PowerVR SGX 540"); break;
        case 0x1F02: a = 0x9A0000; put_str(g_mem, a, "OpenGL ES 2.0"); break;
        case 0x1F03: a = 0x9A0300; put_str(g_mem, a,
            "GL_OES_texture_npot GL_OES_compressed_ETC1_RGB8_texture "
            "GL_EXT_texture_filter_anisotropic"); break;
        case 0x8B8C: a = 0x9A0400; put_str(g_mem, a, "OpenGL ES GLSL ES 1.00"); break;
        default: a = 0;
        }
        c->r0 = a;
        return;
    }
    if (streq(n, "glGetIntegerv")) {
        static const struct { uint32_t pname, value; } ints[] = {
            {0x0D33, 2048}, {0x851C, 2048}, {0x8869, 16}, {0x8872, 8},
            {0x8B4C, 4}, {0x8B4D, 16}, {0x8DFC, 8}, {0x8DFB, 128},
            {0x8DFD, 64}, {0x0D56, 24}, {0x0D57, 8},
        };
        uint32_t pname = c->r0, dst = c->r1, value = 0;
        for (unsigned i = 0; i < sizeof ints / sizeof ints[0]; i++) {
            if (ints[i].pname == pname) { value = ints[i].value; break; }
        }
#ifdef _WIN32
        int screen_w = platform_width(), screen_h = platform_height();
#else
        int screen_w = 800, screen_h = 480;
#endif
        if (pname == 0x0D3A) {
            w32(g_mem, dst + 0, (uint32_t)screen_w);
            w32(g_mem, dst + 4, (uint32_t)screen_h);
        } else if (pname == 0x0BA2 || pname == 0x0C10) {
            w32(g_mem, dst + 0, 0);
            w32(g_mem, dst + 4, 0);
            w32(g_mem, dst + 8, (uint32_t)screen_w);
            w32(g_mem, dst + 12, (uint32_t)screen_h);
        } else {
            w32(g_mem, dst, value);
        }
        return;
    }
    if (streq(n, "glGetFloatv")) {
        w32(g_mem, c->r1, 0);
        return;
    }
    if (streq(n, "glCreateShader")) { c->r0 = g_gl_id++; return; }
    if (streq(n, "glCreateProgram")) { c->r0 = g_gl_id++; return; }
    if (streq(n, "glGetShaderiv") || streq(n, "glGetProgramiv")) {
        uint32_t pname = c->r1;
        w32(g_mem, c->r2, (pname == 0x8B81 || pname == 0x8B82) ? 1u : 0u);
        return;
    }
    if (streq(n, "glGetShaderInfoLog") || streq(n, "glGetProgramInfoLog")) {
        if (c->r2) { w32(g_mem, c->r2, 0); }
        if (c->r3) { g_mem[c->r3] = 0; }
        return;
    }
    if (streq(n, "glGetAttribLocation") || streq(n, "glGetUniformLocation")) {
        c->r0 = 0xFFFFFFFFu;
        return;
    }
    if (streq(n, "glGenBuffers") || streq(n, "glGenTextures") ||
        streq(n, "glGenFramebuffers") || streq(n, "glGenRenderbuffers")) {
        uint32_t count = c->r0, dst = c->r1;
        for (uint32_t i = 0; i < count; i++) { w32(g_mem, dst + i * 4u, g_gl_id++); }
        return;
    }
    if (streq(n, "glCheckFramebufferStatus")) { c->r0 = 0x8CD5u; return; }

    /* --- GL / EGL / networking: no-op for now, logged on first use --- */
    if (is_prefix(n, "gl") || is_prefix(n, "egl") || is_prefix(n, "socket") ||
        is_prefix(n, "connect") || is_prefix(n, "recv") || is_prefix(n, "send")) {
        c->r0 = 0;
        return;
    }

    /*
     * Default: keep boot moving. Silent no-ops here have already caused real bugs (missing atoi
     * crashed the cinematic thread; missing strncat blanked every formatted number), so record the
     * names once each when FAST5_IMPORT_TRACE=1 to keep the remaining audit cheap.
     */
    if (getenv("FAST5_IMPORT_TRACE")) {
        static char seen[96][40];
        static int nseen;
        int dup = 0;
        for (int i = 0; i < nseen; i++) {
            if (strncmp(seen[i], n, 39) == 0) { dup = 1; break; }
        }
        if (!dup && nseen < 96) {
            snprintf(seen[nseen], 40, "%.39s", n);
            nseen++;
            fprintf(stderr, "unimplemented import: %s\n", n);
        }
    }
    c->r0 = 0;
}
