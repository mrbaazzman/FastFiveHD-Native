/*
 * Native Windows harness: real Win32 + ANGLE GLES2 context, translated game code running 1:1.
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <mmsystem.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "cpu.h"
#include "mem.h"
#include "platform.h"
#include "resolution.h"

#define IMAGE_END 0x8B85BCu
#define STACK_TOP 0x8F0000u

void fn_29c830(Cpu *c, uint8_t *mem);
void fn_29cb14(Cpu *c, uint8_t *mem);
void fn_29cac8(Cpu *c, uint8_t *mem);
void fn_29cf94(Cpu *c, uint8_t *mem);
void fn_29d1a0(Cpu *c, uint8_t *mem);
void fn_299d28(Cpu *c, uint8_t *mem); /* nativeSetOnKeyDown(env, obj, keycode) */
void fn_299cf4(Cpu *c, uint8_t *mem); /* nativeSetOnKeyUp(env, obj, keycode)   */
void fn_299e28(Cpu *c, uint8_t *mem); /* nativeAccelerometer(env, obj, x, y, z) */
void call_target(Cpu *c, uint8_t *mem, uint32_t target);
void init_guest_data(uint8_t *mem, uint32_t size);
int gl_bridge_init(void);
int gl_bridge_bound_fbo(void);
void gl_bridge_frame_tick(unsigned frame);
unsigned gl_bridge_texture_id_peak(void);
void guest_heap_stats(uint32_t *bump, uint32_t *free_bytes, uint32_t *largest,
                      uint32_t *blocks, uint32_t *live, uint32_t *peak);
void guest_import_profile_dump(void);
void guest_sleep_stats_dump(void);
void guest_pc_profile_dump(void);

uint32_t guest_mem_size;
static uint8_t *g_mem;
static volatile LONG g_render_active;

/* Frame counter shared with the host diagnostics (runtime/overrides.c). */
uint32_t g_host_frame;

extern Cpu *g_cpu;
static HANDLE g_game_thread;

/*
 * The 2D layer of the game is authored in a fixed pixel space (800x480 on the reference
 * device) and the HUD anchors itself to the screen size. The engine itself runs at the
 * *window* resolution so that its render targets, the camera and the scene are native, and
 * the screen-size globals it uses for 2D layout are converted back into the design space
 * afterwards — so every 2D coordinate in the game stays in one space and the host can
 * present it at any window size.
 */
#define DESIGN_WIDTH 800
#define DESIGN_HEIGHT 480

/* The presentation size. 1920x1080 is the default window; FAST5_WIDTH/FAST5_HEIGHT override it. */
#define WINDOW_WIDTH 1920
#define WINDOW_HEIGHT 1080

/* Guest globals: the screen size the game reads for 2D layout. */
#define SCR_WIDTH_ADDR 0x7d7820u
#define SCR_HEIGHT_ADDR 0x7d7824u

/*
 * `Application::s_pInstance` — the engine's own pointer to the live application object.
 *
 * The application is destroyed and re-created during the boot (the Universal logo screen
 * does that), so the instance cannot be cached: the engine's singleton is the only
 * authoritative source.  ELF 0x7f8758 (`_ZN11Application11s_pInstanceE`).
 */
#define APPLICATION_INSTANCE_ADDR 0x7f8758u

/* 1 when the engine itself runs at the window size (native render targets). */
int g_engine_native = 1;

/*
 * Auto-test touch coordinates are authored against the 800x480 design layout, which is the
 * space the guest engine works in (the window size only affects the presentation).
 */

/*
 * Persistent session log: build/fast5.log.
 *
 * stderr is redirected into it at startup, so every host and game message lands in one file the
 * player can hand back. Logging is always on; FAST5_LOG=<path> overrides the destination.
 */
static int g_log_active;

void fast5_log_open(void) {
    if (g_log_active) return;
    const char *path = getenv("FAST5_LOG");
    if (!path || !*path) path = "build/fast5.log";
    /* Redirect stderr itself: one handle, so ordering and offsets stay consistent. */
    if (!freopen(path, "w", stderr)) return;
    setvbuf(stderr, NULL, _IOLBF, 0);
    g_log_active = 1;
    time_t now = time(NULL);
    struct tm tmv;
    localtime_s(&tmv, &now);
    fprintf(stderr, "=== Fast Five HD session %04d-%02d-%02d %02d:%02d:%02d ===\n",
            tmv.tm_year + 1900, tmv.tm_mon + 1, tmv.tm_mday,
            tmv.tm_hour, tmv.tm_min, tmv.tm_sec);
    fprintf(stderr, "exe_base=0x%llx\n",
            (unsigned long long)(uintptr_t)GetModuleHandleA(NULL));
    fflush(stderr);
}

void fast5_log_note(const char *fmt, ...) {
    if (!g_log_active) return;
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
    fflush(stderr);
}

/*
 * Guest PC sampler (FAST5_PROFILE=1). The translated game is native C, so the useful signal is the
 * host instruction pointer inside build/link/chunk_*.c, not Cpu::pc (which only changes at call
 * boundaries and therefore names the last callee rather than the code actually running). Samples are
 * module-relative offsets so the log can be resolved with addr2line against fast5_win.exe.
 */
#define SAMPLE_SLOTS 8192
static volatile LONG g_sample_run;
static uint64_t g_sample_off[SAMPLE_SLOTS];
static volatile LONG g_sample_n;

static DWORD WINAPI sampler_thread(LPVOID unused) {
    (void)unused;
    for (;;) {
        Sleep(1);
        if (!g_sample_run || !g_game_thread) continue;
        if (SuspendThread(g_game_thread) == (DWORD)-1) continue;
        CONTEXT ctx;
        memset(&ctx, 0, sizeof ctx);
        ctx.ContextFlags = CONTEXT_CONTROL;
        uint64_t rip = 0;
        if (GetThreadContext(g_game_thread, &ctx)) rip = (uint64_t)ctx.Rip;
        ResumeThread(g_game_thread);
        if (!rip) continue;
        LONG n = InterlockedIncrement(&g_sample_n) - 1;
        if (n >= SAMPLE_SLOTS) {
            InterlockedDecrement(&g_sample_n);
            continue;
        }
        g_sample_off[n] = rip - (uint64_t)(uintptr_t)GetModuleHandleA(NULL);
    }
    return 0;
}

static int cmp_u64(const void *a, const void *b) {
    uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b;
    return x < y ? -1 : (x > y ? 1 : 0);
}

typedef struct { uint64_t off; uint32_t count; } PcHit;
static int cmp_hit(const void *a, const void *b) {
    uint32_t x = ((const PcHit *)a)->count, y = ((const PcHit *)b)->count;
    return x < y ? 1 : (x > y ? -1 : 0);   /* descending */
}

void guest_pc_profile_dump(void) {
    LONG n = g_sample_n;
    if (n <= 0) return;
    if (n > SAMPLE_SLOTS) n = SAMPLE_SLOTS;
    uint64_t *tmp = (uint64_t *)malloc((size_t)n * sizeof(uint64_t));
    PcHit *hits = (PcHit *)malloc((size_t)n * sizeof(PcHit));
    if (!tmp || !hits) { free(tmp); free(hits); return; }
    memcpy(tmp, g_sample_off, (size_t)n * sizeof(uint64_t));
    qsort(tmp, (size_t)n, sizeof(uint64_t), cmp_u64);
    size_t nh = 0;
    for (LONG i = 0; i < n; ) {
        uint64_t off = tmp[i];
        LONG c = 0;
        while (i < n && tmp[i] == off) { c++; i++; }
        hits[nh].off = off;
        hits[nh].count = (uint32_t)c;
        nh++;
    }
    qsort(hits, nh, sizeof(PcHit), cmp_hit);
    fprintf(stderr, "[pc] %ld host samples, hottest offsets:\n", n);
    for (size_t rank = 0; rank < nh && rank < 15; rank++) {
        fprintf(stderr, "[pc] 0x%llx  %5.1f%%  (%u)\n",
                (unsigned long long)hits[rank].off,
                100.0 * (double)hits[rank].count / (double)n, hits[rank].count);
    }
    free(tmp);
    free(hits);
}

static DWORD WINAPI watchdog_thread(LPVOID unused) {
    (void)unused;
    if (!getenv("FAST5_WATCHDOG")) return 0;
    int host_samples = 0;
    for (;;) {
        Sleep(3000);
        if (!g_render_active || !g_cpu) continue;
        Cpu *c = g_cpu;
        fprintf(stderr, "watchdog pc=0x%08x lr=0x%08x sp=0x%08x r0=0x%08x r1=0x%08x r2=0x%08x r3=0x%08x\n",
                c->pc, c->lr, c->sp, c->r0, c->r1, c->r2, c->r3);
        if (g_mem && c->sp < 0x10000000u) {
            uint32_t *st = (uint32_t *)(g_mem + c->sp);
            fprintf(stderr, "watchdog stack: %08x %08x %08x %08x %08x %08x %08x %08x\n",
                    st[0], st[1], st[2], st[3], st[4], st[5], st[6], st[7]);
        }
        if (g_game_thread && host_samples < 4) {
            if (SuspendThread(g_game_thread) != (DWORD)-1) {
                CONTEXT ctx;
                memset(&ctx, 0, sizeof ctx);
                ctx.ContextFlags = CONTEXT_CONTROL;
                if (GetThreadContext(g_game_thread, &ctx)) {
                    uintptr_t base = (uintptr_t)GetModuleHandleA(NULL);
                    fprintf(stderr, "main_rip=0x%llx main_off=0x%llx\n",
                            (unsigned long long)ctx.Rip,
                            (unsigned long long)((uintptr_t)ctx.Rip - base));
                }
                ResumeThread(g_game_thread);
            }
            host_samples++;
        }
    }
    return 0;
}

static LONG WINAPI crash_filter(EXCEPTION_POINTERS *ep) {
    fprintf(stderr, "\n*** HOST EXCEPTION %08lx at %p ***\n",
            (unsigned long)ep->ExceptionRecord->ExceptionCode,
            ep->ExceptionRecord->ExceptionAddress);
    if (g_cpu) {
        fprintf(stderr, "guest regs r0=%08x r1=%08x r2=%08x r3=%08x r4=%08x r5=%08x "
                "r6=%08x r7=%08x r8=%08x r9=%08x r10=%08x r11=%08x r12=%08x "
                "sp=%08x lr=%08x pc=%08x\n",
                g_cpu->r0, g_cpu->r1, g_cpu->r2, g_cpu->r3, g_cpu->r4, g_cpu->r5,
                g_cpu->r6, g_cpu->r7, g_cpu->r8, g_cpu->r9, g_cpu->r10, g_cpu->r11,
                g_cpu->r12, g_cpu->sp, g_cpu->lr, g_cpu->pc);
    }
    {
        uintptr_t base = (uintptr_t)GetModuleHandleA(NULL);
        void *frames[40];
        USHORT n = CaptureStackBackTrace(0, 40, frames, NULL);
        for (USHORT i = 0; i < n; i++) {
            fprintf(stderr, "bt %p (off 0x%llx)\n", frames[i],
                    (unsigned long long)((uintptr_t)frames[i] - base));
        }
    }
    fprintf(stderr, "*** end of host exception ***\n");
    fflush(stderr);
    return EXCEPTION_EXECUTE_HANDLER;
}

static void write_bmp(const char *path, int w, int h, const unsigned char *rgba) {
    int row = (w * 3 + 3) & ~3;
    uint32_t size = 54u + (uint32_t)row * (uint32_t)h;
    unsigned char hdr[54];
    memset(hdr, 0, sizeof hdr);
    hdr[0] = 'B'; hdr[1] = 'M';
    memcpy(hdr + 2, &size, 4);
    uint32_t off = 54; memcpy(hdr + 10, &off, 4);
    uint32_t dib = 40; memcpy(hdr + 14, &dib, 4);
    memcpy(hdr + 18, &w, 4); memcpy(hdr + 22, &h, 4);
    uint16_t planes = 1, bpp = 24;
    memcpy(hdr + 26, &planes, 2); memcpy(hdr + 28, &bpp, 2);
    uint32_t raw = (uint32_t)row * (uint32_t)h;
    memcpy(hdr + 34, &raw, 4);
    FILE *f = fopen(path, "wb");
    if (!f) return;
    fwrite(hdr, 1, sizeof hdr, f);
    unsigned char *line = (unsigned char *)calloc(1, (size_t)row);
    /* glReadPixels returns rows bottom-up (row 0 is the bottom of the framebuffer) and a BMP
     * stores rows bottom-up as well, so the rows are written in the order they arrive. Writing
     * them upside down produced captures that did not match the window. */
    for (int y = 0; y < h; y++) {
        memset(line, 0, (size_t)row);
        for (int x = 0; x < w; x++) {
            const unsigned char *p = rgba + ((size_t)y * (size_t)w + (size_t)x) * 4u;
            line[x * 3 + 0] = p[2];
            line[x * 3 + 1] = p[1];
            line[x * 3 + 2] = p[0];
        }
        fwrite(line, 1, (size_t)row, f);
    }
    free(line);
    fclose(f);
}

static void capture_frame(unsigned frame, const char *suffix) {
    typedef void (*PFN_glReadPixels)(int, int, int, int, unsigned, unsigned, void *);
    PFN_glReadPixels rp = (PFN_glReadPixels)platform_gl_proc("glReadPixels");
    if (!rp) return;
    int w = platform_width(), h = platform_height();
    unsigned char *px = (unsigned char *)malloc((size_t)w * (size_t)h * 4u);
    if (!px) return;
    rp(0, 0, w, h, 0x1908u, 0x1401u, px);
    char path[80];
    snprintf(path, sizeof path, "build/game_frame_%u%s.bmp", frame, suffix);
    write_bmp(path, w, h, px);
    fprintf(stderr, "captured %s\n", path);
    free(px);
}

void guest_fault(uint32_t addr, int write) {
    fprintf(stderr, "\n*** GUEST FAULT at 0x%08x (%s) ***\n", addr, write ? "write" : "read");
    if (g_cpu) {
        fprintf(stderr, "guest regs r0=%08x r1=%08x r2=%08x r3=%08x r4=%08x r5=%08x "
                "r6=%08x r7=%08x r8=%08x r9=%08x r10=%08x r11=%08x r12=%08x "
                "sp=%08x lr=%08x pc=%08x\n",
                g_cpu->r0, g_cpu->r1, g_cpu->r2, g_cpu->r3, g_cpu->r4, g_cpu->r5,
                g_cpu->r6, g_cpu->r7, g_cpu->r8, g_cpu->r9, g_cpu->r10, g_cpu->r11,
                g_cpu->r12, g_cpu->sp, g_cpu->lr, g_cpu->pc);
    }
    {
        uintptr_t base = (uintptr_t)GetModuleHandleA(NULL);
        void *frames[40];
        USHORT n = CaptureStackBackTrace(0, 40, frames, NULL);
        for (USHORT i = 0; i < n; i++) {
            fprintf(stderr, "bt %p (off 0x%llx)\n", frames[i],
                    (unsigned long long)((uintptr_t)frames[i] - base));
        }
    }
    if (g_cpu && g_mem && g_cpu->sp < 0x10000000u) {
        uint32_t *st = (uint32_t *)(g_mem + (g_cpu->sp & ~3u));
        fprintf(stderr, "stack @sp:");
        for (int i = 0; i < 16; i++) fprintf(stderr, " %08x", st[i]);
        fprintf(stderr, "\n");
    }
    fprintf(stderr, "*** end of guest fault ***\n");
    fflush(stderr);
    exit(3);
}

static void touch_press(int x, int y) {
    Cpu t;
    memset(&t, 0, sizeof t);
    t.r0 = (uint32_t)x;
    t.r1 = (uint32_t)y;
    t.r2 = 0u;
    t.sp = STACK_TOP;
    t.lr = 0xDEADBEE0u;
    fn_29cf94(&t, g_mem);
}

static void touch_release(int x, int y) {
    Cpu t;
    memset(&t, 0, sizeof t);
    t.r0 = (uint32_t)x;
    t.r1 = (uint32_t)y;
    t.r2 = 0u;
    t.sp = STACK_TOP;
    t.lr = 0xDEADBEE0u;
    fn_29d1a0(&t, g_mem);
}

static void key_event(int code, int down) {
    Cpu t;
    memset(&t, 0, sizeof t);
    t.r0 = 0u;                 /* JNIEnv*: unused by the handler */
    t.r1 = 0u;                 /* jclass/jobject: unused */
    t.r2 = (uint32_t)code;     /* Android key code */
    t.sp = STACK_TOP;
    t.lr = 0xDEADBEE0u;
    if (down) fn_299d28(&t, g_mem);
    else fn_299cf4(&t, g_mem);
}

/*
 * Accelerometer globals, for reference (not written by the port any more; steering uses the
 * engine's touch path): mAccelerometerZ = ELF 0x8B85A4, X = 0x8B85AC, Y = 0x8B85B0.
 */

int main(void) {
    fast5_log_open();
    /* Default Windows timer granularity is ~15.6 ms, so every Sleep(1) actually parks for 15.6 ms.
     * The game sleeps from asset-loading and pacing loops, which turns short waits into multi-second
     * stalls. Raise it to 1 ms for the process lifetime. */
    timeBeginPeriod(1);
    SetUnhandledExceptionFilter(crash_filter);
    /* Guest arena. Measured live usage peaks around 44 MiB, so 512 MiB is ample as long as freed
     * scene memory can be reused — the original failure was fragmentation in the guest allocator,
     * not real demand (765 MiB free but split into ~750k blocks, largest 4.7 MiB). See the
     * boundary-tag allocator in runtime/imports.c. FAST5_ARENA_MB overrides the size. */
    size_t size = 0x20000000;
    {
        const char *mb = getenv("FAST5_ARENA_MB");
        if (mb && *mb) {
            long v = strtol(mb, NULL, 10);
            if (v >= 256 && v <= 3072) size = (size_t)v << 20;
        }
    }
    g_mem = (uint8_t *)calloc(1, size);
    if (!g_mem) return 2;

    FILE *f = fopen("build/guest.bin", "rb");
    if (!f) { perror("build/guest.bin"); return 2; }
    if (fread(g_mem, 1, IMAGE_END, f) != IMAGE_END) { fclose(f); return 2; }
    fclose(f);
    guest_mem_size = (uint32_t)size;
    init_guest_data(g_mem, guest_mem_size);

    platform_set_design_size(DESIGN_WIDTH, DESIGN_HEIGHT);
    if (!platform_init(WINDOW_WIDTH, WINDOW_HEIGHT, "Fast Five HD")) {
        fprintf(stderr, "platform init failed\n");
        return 3;
    }
    /*
     * The window size is the presentation size. The engine normally runs at the window size
     * too (native render targets, native camera); FAST5_ENGINE_RES=design keeps it at the
     * design resolution instead.
     */
    {
        const char *fit = getenv("FAST5_UI_FIT");
        int fill = !(fit && !strcmp(fit, "letterbox"));
        const char *mode = getenv("FAST5_ENGINE_RES");
        int native = (mode && !strcmp(mode, "design")) ? 0 : 1;
        g_engine_native = native;
        fast5_resolution_setup(DESIGN_WIDTH, DESIGN_HEIGHT,
                                platform_width(), platform_height(), fill, native);
    }
    if (!gl_bridge_init()) {
        fprintf(stderr, "GL bridge init failed\n");
        return 3;
    }

    uint32_t *init = (uint32_t *)(g_mem + 0x79f090u);
    for (unsigned i = 0; i < 433u; i++) {
        Cpu ci;
        memset(&ci, 0, sizeof ci);
        ci.sp = STACK_TOP;
        ci.lr = 0xDEADBEE0u;
        call_target(&ci, g_mem, init[i]);
    }
    fprintf(stderr, "constructors: 433 completed\n");

    Cpu c;
    memset(&c, 0, sizeof c);
    /*
     * The engine runs at the window resolution: its render targets, the camera aspect and
     * the scene are then native. FAST5_ENGINE_RES=design runs it at the design resolution
     * instead (the old behaviour, kept for A/B).
     */
    /*
     * The engine runs at the window size: its render targets are then window sized (native
     * road reflections and post-effect buffers) and the camera aspect is the window's.
     *
     * The 2D layout is kept in the game's own 800x480 design space: the screen-size globals
     * the engine reads for 2D positioning are converted back into it after every
     * initialisation, and the host scales the 2D draws from there to the window.
     *
     * FAST5_ENGINE_RES=design runs the engine at the design resolution instead (window-sized
     * render targets are then lost, but the 2D layout needs no conversion at all).
     */
    int engine_at_design = g_engine_native ? 0 : 1;
    int engine_w = engine_at_design ? DESIGN_WIDTH : platform_width();
    int engine_h = engine_at_design ? DESIGN_HEIGHT : platform_height();
    c.r0 = (uint32_t)engine_w;
    c.r1 = (uint32_t)engine_h;
    c.r2 = 0u;
    c.sp = STACK_TOP;
    c.lr = 0xDEADBEE0u;
    fprintf(stderr, "calling appInit(%d,%d,0)\n", engine_w, engine_h);
    fn_29c830(&c, g_mem);
    fprintf(stderr, "appInit returned r0=%u pc=0x%08x\n", c.r0, c.pc);
    /* `mbAppRunning` is a flag: the app is running when the init succeeded. */
    *(uint32_t *)(g_mem + 0x8B85A0) = c.r0 ? 1u : 0u;
    /*
     * From here on the 2D layout must work in the design space, so the screen-size globals
     * the game reads for 2D layout are converted back to it. The device, its render targets
     * and the camera keep the window size.
     */
    if (g_engine_native) {
        int32_t dw = DESIGN_WIDTH, dh = DESIGN_HEIGHT;
        int32_t lw = DESIGN_WIDTH / 2, lh = DESIGN_HEIGHT / 2;
        uint32_t app = *(uint32_t *)(g_mem + APPLICATION_INSTANCE_ADDR);
        *(int32_t *)(g_mem + SCR_WIDTH_ADDR) = dw;
        *(int32_t *)(g_mem + SCR_HEIGHT_ADDR) = dh;
        if (app && app + 0x258u < guest_mem_size) {
            *(int32_t *)(g_mem + app + 0x250u) = lw;
            *(int32_t *)(g_mem + app + 0x254u) = lh;
        }
        fprintf(stderr, "scr: SCR_WIDTH=%d SCR_HEIGHT=%d (design %dx%d)\n",
                dw, dh, DESIGN_WIDTH, DESIGN_HEIGHT);
    }

    Cpu s;
    memset(&s, 0, sizeof s);
    s.sp = STACK_TOP;
    s.lr = 0xDEADBEE0u;
    fn_29cac8(&s, g_mem);

    int touch_frames = 0;
    int touch_x = 400;
    int touch_y = 240;
    int auto_menu = getenv("FAST5_AUTO_MENU") != NULL;
    int auto_race = getenv("FAST5_AUTO_RACE") != NULL;
    const char *auto_back_env = getenv("FAST5_AUTO_BACK");
    int auto_back = auto_back_env != NULL;
    /* Frame captures are diagnostics, not part of normal play. Auto/test runs keep them. */
    int capture_on = auto_menu || auto_race || getenv("FAST5_CAPTURE") != NULL ||
                     getenv("FAST5_BURST") != NULL;
    /* FAST5_AUTO_TOUCH=<frame>:<guest-x>:<guest-y>,... drives exact menu coordinates for
       resolution testing; it is not part of the game path. */
    struct { unsigned frame; int x, y, done; } auto_touches[32];
    int auto_touch_count = 0;
    {
        const char *p = getenv("FAST5_AUTO_TOUCH");
        while (p && *p && auto_touch_count < (int)(sizeof auto_touches / sizeof auto_touches[0])) {
            unsigned f;
            int x, y, used = 0;
            if (sscanf(p, "%u:%d:%d%n", &f, &x, &y, &used) != 3 || used <= 0) break;
            auto_touches[auto_touch_count].frame = f;
            auto_touches[auto_touch_count].x = x;
            auto_touches[auto_touch_count].y = y;
            auto_touches[auto_touch_count].done = 0;
            auto_touch_count++;
            p += used;
            if (*p == ',') p++;
        }
    }
    /* FAST5_AUTO_STEER=<start>:<frames>:<dir> injects a held steering key from a frame, so the
       keyboard path can be verified without a human at the keyboard. dir: L, R, U, D. */
    const char *auto_steer_env = getenv("FAST5_AUTO_STEER");
    int steer_from = -1, steer_len = 0, steer_key = 0;
    if (auto_steer_env) {
        char dir = 0;
        if (sscanf(auto_steer_env, "%d:%d:%c", &steer_from, &steer_len, &dir) == 3) {
            switch (dir) {
            case 'L': steer_key = 21; break;   /* DPAD_LEFT  */
            case 'R': steer_key = 22; break;   /* DPAD_RIGHT */
            case 'U': steer_key = 19; break;   /* DPAD_UP    */
            case 'D': steer_key = 20; break;   /* DPAD_DOWN  */
            case 'N': steer_key = 62; break;   /* SPACE = nitro */
            case 'C': steer_key = 31; break;   /* C = camera */
            case 'Z': steer_key = 54; break;   /* Z = look back */
            case 'K': steer_key = 113; break;  /* CTRL = brake */
            default: steer_key = 0; break;
            }
            fprintf(stderr, "auto steer: key %d from frame %d for %d frames\n",
                    steer_key, steer_from, steer_len);
        } else {
            steer_from = -1;
        }
    }
    /* The auto-race script is calibrated for the 800x480 timing; a bigger window needs
     * more frames per menu screen (FAST5_AUTO_SCALE tunes it). */
    double auto_scale = 1.0;
    {
        const char *sc = getenv("FAST5_AUTO_SCALE");
        if (sc && *sc) {
            double v = atof(sc);
            if (v > 0.1 && v < 8.0) auto_scale = v;
        }
    }
    unsigned frames = 0;
    g_game_thread = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT, FALSE, GetCurrentThreadId());
    CreateThread(NULL, 0, watchdog_thread, NULL, 0, NULL);
    CreateThread(NULL, 0, sampler_thread, NULL, 0, NULL);
    while (!platform_should_quit()) {
        LARGE_INTEGER frame_start;
        QueryPerformanceCounter(&frame_start);
        g_host_frame = frames;
        platform_poll();
        if (guest_mem_size > 0x7d8c78u) {
            static int hm_logged = 0;
            if (!hm_logged) {
                fprintf(stderr, "deviceconfig highMem byte=0x%02x (forcing 1)\n", g_mem[0x7d8c78]);
                hm_logged = 1;
            }
            g_mem[0x7d8c78] = 1; /* DeviceConfig::m_bIsHighMemDevice: desktop port has 512 MiB */
        }
        int pointer_x, pointer_y, pointer_down;
        while (platform_take_pointer(&pointer_x, &pointer_y, &pointer_down)) {
            if (pointer_down) {
                fprintf(stderr, "[f%u] touch press  %d,%d\n", frames, pointer_x, pointer_y);
                touch_x = pointer_x;
                touch_y = pointer_y;
                touch_press(pointer_x, pointer_y);
            } else {
                fprintf(stderr, "[f%u] touch release %d,%d\n", frames, pointer_x, pointer_y);
                touch_release(pointer_x, pointer_y);
            }
        }
        int gk_code, gk_down;
        while (platform_take_game_key(&gk_code, &gk_down)) {
            fprintf(stderr, "[f%u] key %d %s\n", frames, gk_code, gk_down ? "down" : "up");
            key_event(gk_code, gk_down);
        }
        if (steer_key && steer_from >= 0) {
            static int steer_state = -1;   /* -1 unknown, 0 up, 1 down */
            int want = (frames >= (unsigned)steer_from &&
                        frames < (unsigned)(steer_from + steer_len)) ? 1 : 0;
            if (want != steer_state) {
                /* Route through the platform queue so held-key state is updated exactly as a real
                   key press would be. */
                if (steer_state != -1) platform_inject_key(steer_key, want);
                steer_state = want;
            }
        }
        /*
         * Steering is handled through the engine's own touch / onscreen-button scheme (see the
         * CarControl::SetManualInputFlags hook in overrides.c). Nothing is fed to the accelerometer
         * here: driving it tilted the whole camera, whereas the button scheme steers the car
         * without moving the view.
         */
        int key = platform_take_key();
        if (key) {
            fprintf(stderr, "key %d -> touch press\n", key);
            touch_x = 400;
            touch_y = 240;
            touch_press(touch_x, touch_y);
            touch_frames = 4;
        }
        if (frames == 120) {
            fprintf(stderr, "auto touch press\n");
            touch_x = 400;
            touch_y = 240;
            touch_press(touch_x, touch_y);
            touch_frames = 6;
        }
        for (int i = 0; i < auto_touch_count; i++) {
            if (!auto_touches[i].done && frames >= auto_touches[i].frame) {
                auto_touches[i].done = 1;
                touch_x = auto_touches[i].x;
                touch_y = auto_touches[i].y;
                fprintf(stderr, "auto touch %u: %d,%d\n", auto_touches[i].frame, touch_x, touch_y);
                touch_press(touch_x, touch_y);
                touch_frames = 6;
            }
        }
        if (auto_menu && frames == 240) {
            fprintf(stderr, "auto menu touch press\n");
            touch_x = 400;
            touch_y = 240;
            touch_press(touch_x, touch_y);
            touch_frames = 6;
        }
        /* The guest always runs at the design resolution, so the scripted touch points are
         * the ones authored for the 800x480 layout. */
        if (auto_race && frames == (unsigned)(480 * auto_scale)) {
            fprintf(stderr, "auto race touch press\n");
            touch_x = 400;
            touch_y = 100;
            touch_press(touch_x, touch_y);
            touch_frames = 6;
        }
        if (auto_race && frames == (unsigned)(720 * auto_scale)) {
            fprintf(stderr, "auto track touch press\n");
            touch_x = 530;
            touch_y = 70;
            touch_press(touch_x, touch_y);
            touch_frames = 6;
        }
        if (auto_race && frames == (unsigned)(960 * auto_scale)) {
            fprintf(stderr, "auto race type touch press\n");
            touch_x = 530;
            touch_y = 70;
            touch_press(touch_x, touch_y);
            touch_frames = 6;
        }
        if (auto_race && frames == (unsigned)(1200 * auto_scale)) {
            fprintf(stderr, "auto car select touch press\n");
            touch_x = 720;
            touch_y = 435;
            touch_press(touch_x, touch_y);
            touch_frames = 6;
        }
        if (auto_race && frames == (unsigned)(1400 * auto_scale)) {
            /* The transmission dialog (NO / YES) can appear after the car selection. */
            fprintf(stderr, "auto transmission yes touch press\n");
            touch_x = 760;
            touch_y = 413;
            touch_press(touch_x, touch_y);
            touch_frames = 6;
        }
        if (auto_race && frames == (unsigned)(1560 * auto_scale)) {
            fprintf(stderr, "auto tutorial skip touch press\n");
            touch_x = 330;
            touch_y = 215;
            touch_press(touch_x, touch_y);
            touch_frames = 6;
        }
        /* FAST5_AUTO_BACK=<n>: run n full race -> menu -> race cycles. This is the memory-stress
           path: every cycle tears down a TrackScene and rebuilds the menu, which is what exposed
           the allocator fragmentation. Cycle length is 2400 frames after the first race starts. */
        if (auto_back && frames >= 2600) {
            unsigned cyc = (frames - 2600) / 2400;      /* which cycle we are in */
            unsigned at = (frames - 2600) % 2400;       /* position inside the cycle */
            int hx = 0, hy = 0, act = 0;
            if (at == 0) {
                hx = 26; hy = 447; act = 1; /* pause */
                fprintf(stderr, "auto cycle %u: pause\n", cyc);
            } else if (at == 160) {
                hx = 110; hy = 433; act = 1; /* back to menu */
                fprintf(stderr, "auto cycle %u: back to menu\n", cyc);
            } else if (at == 700) {
                hx = 400; hy = 100; act = 1; /* Quick Race */
                fprintf(stderr, "auto cycle %u: quick race\n", cyc);
            } else if (at == 940) {
                hx = 530; hy = 70; act = 1; /* track */
            } else if (at == 1180) {
                hx = 530; hy = 70; act = 1; /* race type */
            } else if (at == 1420) {
                hx = 720; hy = 435; act = 1; /* car OK */
            } else if (at == 1700) {
                hx = 330; hy = 215; act = 1; /* skip tutorial */
                fprintf(stderr, "auto cycle %u: race start\n", cyc);
            }
            if (act) {
                touch_x = hx;
                touch_y = hy;
                touch_press(touch_x, touch_y);
                touch_frames = 6;
            }
        }
        if (touch_frames > 0 && --touch_frames == 0) {
            fprintf(stderr, "touch release\n");
            touch_release(touch_x, touch_y);
        }

        Cpu r;
        memset(&r, 0, sizeof r);
        r.sp = STACK_TOP;
        r.lr = 0xDEADBEE0u;
        /*
         * Keep the 2D layout in the design space.
         *
         * The engine owns its screen-size globals and rewrites them: `Application::Init`
         * computes the logical size, and `PostEffects::PostEffects` recomputes it from the
         * render target every time a scene is built (that is what put the Universal logo's
         * legal text and the loading widgets outside the window at 1920x1080). So the
         * conversion is re-applied every frame, after the engine has done its own work.
         */
        if (g_engine_native) {
            uint32_t app = *(uint32_t *)(g_mem + APPLICATION_INSTANCE_ADDR);
            *(int32_t *)(g_mem + SCR_WIDTH_ADDR) = DESIGN_WIDTH;
            *(int32_t *)(g_mem + SCR_HEIGHT_ADDR) = DESIGN_HEIGHT;
            if (app && app + 0x258u < guest_mem_size) {
                *(int32_t *)(g_mem + app + 0x250u) = DESIGN_WIDTH / 2;
                *(int32_t *)(g_mem + app + 0x254u) = DESIGN_HEIGHT / 2;
            }
        }
        g_render_active = 1;
        LARGE_INTEGER t0, t1;
        int profiling = getenv("FAST5_PROFILE") != NULL;
        if (profiling) {
            /* Only sample frames that run long, so the histogram lands on the stall itself. */
            g_sample_n = 0;
            g_sample_run = 1;
        }
        QueryPerformanceCounter(&t0);
        gl_bridge_frame_tick(frames);
        fn_29cb14(&r, g_mem);
        QueryPerformanceCounter(&t1);
        g_sample_run = 0;
        g_render_active = 0;
        {
            static LARGE_INTEGER prof_freq;
            if (!prof_freq.QuadPart) QueryPerformanceFrequency(&prof_freq);
            double ms = (double)(t1.QuadPart - t0.QuadPart) * 1000.0 / (double)prof_freq.QuadPart;
            /* Loading stalls are whole seconds; steady state is ~30 ms. Report only slow frames so
               the log shows where the wall-clock went without drowning in per-frame noise. */
            if (ms > 250.0) {
                fprintf(stderr, "[f%u] SLOW frame %.0f ms\n", frames, ms);
                if (profiling) {
                    guest_pc_profile_dump();
                    guest_import_profile_dump();
                }
            }
        }
        if (getenv("FAST5_PROFILE") && frames % 600 == 0 && frames) {
            guest_sleep_stats_dump();
        }

        if (capture_on && (frames == 30 || frames == 60 || frames == 120 ||
            frames == 240 || frames == 480 || frames == 720 ||
            frames == 840 || frames == 960 || frames == 1080 || frames == 1200 ||
            frames == 1320 || frames == 1440 || frames == 1560 || frames == 1800 ||
            frames == 2100 || frames == 2400 || frames == 2700 || frames == 3000 ||
            frames == 3300 || frames == 3600 || frames == 4200 || frames == 4500 ||
            frames == 4800 || frames == 5100 || frames == 5400 || frames == 6000 ||
            frames == 6600)) {
            int fb = gl_bridge_bound_fbo();
            fprintf(stderr, "capture frame %u bound_fbo=%d texIdPeak=%u\n",
                    frames, fb, (unsigned)gl_bridge_texture_id_peak());
            if (0x7f980cu + 4u < guest_mem_size) {
                uint32_t game = *(uint32_t *)(g_mem + 0x7f980cu);
                uint32_t pfx = (game && game + 0x168u < guest_mem_size)
                                   ? *(uint32_t *)(g_mem + game + 0x164u) : 0;
                if (pfx && pfx + 0x14u < guest_mem_size) {
                    fprintf(stderr, "postfx: pfx=0x%08x current=%d default=%d\n", pfx,
                            *(int *)(g_mem + pfx + 0xcu), *(int *)(g_mem + pfx + 0x10u));
                }
            }
            capture_frame(frames, "");
            if (fb != 0) {
                typedef void (*PFN_glBindFramebuffer)(unsigned, unsigned);
                PFN_glBindFramebuffer bf =
                    (PFN_glBindFramebuffer)platform_gl_proc("glBindFramebuffer");
                if (bf) {
                    bf(0x8D40u, 0);
                    capture_frame(frames, "_default");
                    bf(0x8D40u, (unsigned)fb);
                }
            }
        }
        /* FAST5_BURST=<start>:<count> captures consecutive frames (diagnostics only). */
        {
            static unsigned burst_start;
            static int burst_count, burst_parsed;
            if (!burst_parsed) {
                burst_parsed = 1;
                const char *e = getenv("FAST5_BURST");
                if (e) sscanf(e, "%u:%d", &burst_start, &burst_count);
            }
            if (burst_count > 0 && frames >= burst_start &&
                frames < burst_start + (unsigned)burst_count) {
                capture_frame(frames, "_burst");
            }
        }
        platform_swap();
        /* Pace to 60 FPS only when a frame finished early. Slow frames (scene loads, texture
           decode) run back-to-back instead of each paying an extra fixed 16 ms. */
        {
            static LARGE_INTEGER pace_freq;
            LARGE_INTEGER now;
            if (!pace_freq.QuadPart) QueryPerformanceFrequency(&pace_freq);
            QueryPerformanceCounter(&now);
            double spent = (double)(now.QuadPart - frame_start.QuadPart) * 1000.0 /
                           (double)pace_freq.QuadPart;
            if (spent < 16.0) Sleep((DWORD)(16.0 - spent));
        }
        frames++;
        if (frames % 120 == 0) {
            uint32_t bump = 0, fbytes = 0, largest = 0, blocks = 0, live = 0, peak = 0;
            guest_heap_stats(&bump, &fbytes, &largest, &blocks, &live, &peak);
            fprintf(stderr,
                    "[f%u] frame tick  heap: bump=%.1fMiB free=%.1fMiB largest=%.1fMiB "
                    "blocks=%u live=%.1fMiB peak=%.1fMiB\n",
                    frames, bump / 1048576.0, fbytes / 1048576.0, largest / 1048576.0,
                    blocks, live / 1048576.0, peak / 1048576.0);
            if (g_engine_native) {
                uint32_t app = *(uint32_t *)(g_mem + APPLICATION_INSTANCE_ADDR);
                fprintf(stderr, "[f%u] screen globals: SCR=%dx%d logical=%dx%d\n", frames,
                        *(int32_t *)(g_mem + SCR_WIDTH_ADDR),
                        *(int32_t *)(g_mem + SCR_HEIGHT_ADDR),
                        app && app + 0x258u < guest_mem_size
                            ? *(int32_t *)(g_mem + app + 0x250u) : -1,
                        app && app + 0x258u < guest_mem_size
                            ? *(int32_t *)(g_mem + app + 0x254u) : -1);
            }
        }
    }

    fprintf(stderr, "=== session end: %u frames (window closed) ===\n", frames);
    fflush(stderr);
    platform_shutdown();
    return 0;
}
