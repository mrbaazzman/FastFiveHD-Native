#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cpu.h"
#include "thread_win32.h"

#define MAX_GUEST_THREADS 8
#define MAX_GUEST_MUTEXES 128
#define THREAD_STACK_SIZE 0x00100000u
#define THREAD_STACK_TOP0 0x00A00000u
#define MAIN_THREAD_ID 1u

void call_target(Cpu *c, uint8_t *mem, uint32_t target);

typedef struct GuestThread {
    HANDLE handle;
    Cpu *cpu;
    uint32_t id;
    int used;
} GuestThread;

typedef struct ThreadStart {
    Cpu *cpu;
    uint8_t *mem;
    uint32_t start;
    uint32_t id;
} ThreadStart;

static GuestThread g_threads[MAX_GUEST_THREADS];
static CRITICAL_SECTION g_threads_lock;
static CRITICAL_SECTION g_mutexes_lock;
static CRITICAL_SECTION g_mutexes[MAX_GUEST_MUTEXES];
static unsigned char g_mutex_used[MAX_GUEST_MUTEXES];
static DWORD g_tls_index;
static INIT_ONCE g_once = INIT_ONCE_STATIC_INIT;

static BOOL CALLBACK thread_init_once(PINIT_ONCE once, PVOID parameter, PVOID *context) {
    (void)once;
    (void)parameter;
    (void)context;
    InitializeCriticalSection(&g_threads_lock);
    InitializeCriticalSection(&g_mutexes_lock);
    g_tls_index = TlsAlloc();
    return g_tls_index != TLS_OUT_OF_INDEXES;
}

static int thread_ensure_init(void) {
    return InitOnceExecuteOnce(&g_once, thread_init_once, NULL, NULL) != 0;
}

static uint32_t get_u32(uint8_t *mem, uint32_t addr) {
    uint32_t v;
    memcpy(&v, mem + addr, sizeof v);
    return v;
}

static void put_u32(uint8_t *mem, uint32_t addr, uint32_t v) {
    memcpy(mem + addr, &v, sizeof v);
}

static DWORD WINAPI guest_thread_entry(LPVOID parameter) {
    ThreadStart *start = (ThreadStart *)parameter;
    if (!start) return 0;
    TlsSetValue(g_tls_index, (LPVOID)(uintptr_t)start->id);
    if (getenv("FAST5_THREAD_TRACE")) {
        fprintf(stderr, "thread_win32: start id=%u entry=0x%08x sp=0x%08x\n",
                start->id, start->start, start->cpu->sp);
    }
    call_target(start->cpu, start->mem, start->start);
    if (getenv("FAST5_THREAD_TRACE")) {
        fprintf(stderr, "thread_win32: exit id=%u\n", start->id);
    }
    free(start);
    return 0;
}

int thread_win32_create(uint8_t *mem, uint32_t mem_size, uint32_t thread_ptr,
                        uint32_t start, uint32_t arg) {
    if (!thread_ensure_init() || !mem || thread_ptr > mem_size - 4u) return 11;
    if (!start) return 22;

    EnterCriticalSection(&g_threads_lock);
    int slot = -1;
    for (int i = 0; i < MAX_GUEST_THREADS; i++) {
        if (!g_threads[i].used) { slot = i; break; }
    }
    if (slot < 0) {
        LeaveCriticalSection(&g_threads_lock);
        return 11;
    }

    uint32_t stack_top = THREAD_STACK_TOP0 + (uint32_t)slot * THREAD_STACK_SIZE;
    if (stack_top >= mem_size) {
        LeaveCriticalSection(&g_threads_lock);
        return 11;
    }

    Cpu *cpu = (Cpu *)calloc(1, sizeof *cpu);
    ThreadStart *ctx = (ThreadStart *)calloc(1, sizeof *ctx);
    if (!cpu || !ctx) {
        free(cpu);
        free(ctx);
        LeaveCriticalSection(&g_threads_lock);
        return 12;
    }

    cpu->sp = stack_top;
    cpu->lr = 0xDEADBEE0u;
    cpu->r0 = arg;
    ctx->cpu = cpu;
    ctx->mem = mem;
    ctx->start = start;
    ctx->id = (uint32_t)slot + 2u;

    HANDLE handle = CreateThread(NULL, THREAD_STACK_SIZE, guest_thread_entry, ctx, 0, NULL);
    if (!handle) {
        free(cpu);
        free(ctx);
        LeaveCriticalSection(&g_threads_lock);
        return 12;
    }

    g_threads[slot].handle = handle;
    g_threads[slot].cpu = cpu;
    g_threads[slot].id = ctx->id;
    g_threads[slot].used = 1;
    put_u32(mem, thread_ptr, ctx->id);
    if (getenv("FAST5_THREAD_TRACE")) {
        fprintf(stderr, "thread_win32: create id=%u entry=0x%08x sp=0x%08x\n",
                ctx->id, start, stack_top);
    }
    LeaveCriticalSection(&g_threads_lock);
    return 0;
}

int thread_win32_join(uint8_t *mem, uint32_t thread_id, uint32_t retval_ptr) {
    (void)mem;
    if (!thread_ensure_init()) return 11;
    if (thread_id < 2u || thread_id >= MAX_GUEST_THREADS + 2u) return 3;

    int slot = (int)thread_id - 2;
    EnterCriticalSection(&g_threads_lock);
    if (!g_threads[slot].used) {
        LeaveCriticalSection(&g_threads_lock);
        return 3;
    }
    HANDLE handle = g_threads[slot].handle;
    Cpu *cpu = g_threads[slot].cpu;
    LeaveCriticalSection(&g_threads_lock);

    WaitForSingleObject(handle, INFINITE);
    CloseHandle(handle);

    EnterCriticalSection(&g_threads_lock);
    g_threads[slot].handle = NULL;
    g_threads[slot].cpu = NULL;
    g_threads[slot].used = 0;
    LeaveCriticalSection(&g_threads_lock);
    free(cpu);

    if (retval_ptr && retval_ptr <= 0xFFFFFFFCu) {
        /* The thread entry returns void; the shipped callers pass NULL. */
        (void)retval_ptr;
    }
    return 0;
}

uint32_t thread_win32_self(void) {
    if (!thread_ensure_init()) return MAIN_THREAD_ID;
    uintptr_t value = (uintptr_t)TlsGetValue(g_tls_index);
    return value ? (uint32_t)value : MAIN_THREAD_ID;
}

static int mutex_index(uint8_t *mem, uint32_t mem_size, uint32_t mutex_ptr, int create) {
    if (!thread_ensure_init() || !mem || mutex_ptr > mem_size - 4u) return -1;

    uint32_t id = get_u32(mem, mutex_ptr);
    if (id >= 1u && id <= MAX_GUEST_MUTEXES && g_mutex_used[id - 1u]) {
        return (int)id - 1;
    }

    if (!create) return -1;

    EnterCriticalSection(&g_mutexes_lock);
    for (int i = 0; i < MAX_GUEST_MUTEXES; i++) {
        if (!g_mutex_used[i]) {
            InitializeCriticalSection(&g_mutexes[i]);
            g_mutex_used[i] = 1;
            put_u32(mem, mutex_ptr, (uint32_t)i + 1u);
            LeaveCriticalSection(&g_mutexes_lock);
            return i;
        }
    }
    LeaveCriticalSection(&g_mutexes_lock);
    return -1;
}

int thread_win32_mutex_init(uint8_t *mem, uint32_t mem_size, uint32_t mutex_ptr) {
    int index = mutex_index(mem, mem_size, mutex_ptr, 0);
    if (index >= 0) return 0;
    index = mutex_index(mem, mem_size, mutex_ptr, 1);
    return index >= 0 ? 0 : 11;
}

int thread_win32_mutex_lock(uint8_t *mem, uint32_t mem_size, uint32_t mutex_ptr) {
    int index = mutex_index(mem, mem_size, mutex_ptr, 1);
    if (index < 0) return 11;
    EnterCriticalSection(&g_mutexes[index]);
    return 0;
}

int thread_win32_mutex_trylock(uint8_t *mem, uint32_t mem_size, uint32_t mutex_ptr) {
    int index = mutex_index(mem, mem_size, mutex_ptr, 1);
    if (index < 0) return 11;
    return TryEnterCriticalSection(&g_mutexes[index]) ? 0 : 16;
}

int thread_win32_mutex_unlock(uint8_t *mem, uint32_t mem_size, uint32_t mutex_ptr) {
    int index = mutex_index(mem, mem_size, mutex_ptr, 0);
    if (index < 0) return 1;
    LeaveCriticalSection(&g_mutexes[index]);
    return 0;
}

int thread_win32_mutex_destroy(uint8_t *mem, uint32_t mem_size, uint32_t mutex_ptr) {
    int index = mutex_index(mem, mem_size, mutex_ptr, 0);
    if (index < 0) return 0;
    EnterCriticalSection(&g_mutexes_lock);
    DeleteCriticalSection(&g_mutexes[index]);
    memset(&g_mutexes[index], 0, sizeof g_mutexes[index]);
    g_mutex_used[index] = 0;
    put_u32(mem, mutex_ptr, 0);
    LeaveCriticalSection(&g_mutexes_lock);
    return 0;
}
