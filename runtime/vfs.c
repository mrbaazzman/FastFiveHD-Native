/*
 * Host-backed VFS for the translated game.
 *
 * The Android build reads its assets from /sdcard/gameloft/games/GloftF5HP/res/ and writes user
 * data under /data/data/<package>/. On the host those become:
 *   read : GloftF5HP/res/<name>   (the shipped, read-only data tree)
 *   write: build/userdata/<name>  (a separate writable area; game data is never modified)
 *
 * Guest FILE* values are small integer handles, not host pointers: the guest never dereferences
 * them, only passes them back to stdio imports.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#ifdef _WIN32
#include <direct.h>
#define mkdir(path, mode) _mkdir(path)
#endif

#define VFS_MAX_FILES 64
#define VFS_HANDLE_BASE 0x2000u
#define VFS_FD_BASE 0x400

typedef struct {
    FILE *fp;
    int used;
} VFile;

static VFile g_files[VFS_MAX_FILES];
static int g_userdir_ready;

static int vfs_dbg(void) {
    static int v = -1;
    if (v < 0) v = getenv("FAST5_DEBUG_VFS") != NULL;
    return v;
}

static void ensure_userdir(void) {
    if (g_userdir_ready) return;
    mkdir("build", 0777);
    mkdir("build/userdata", 0777);
    g_userdir_ready = 1;
}

static int is_write_mode(const char *mode) {
    return mode && (strchr(mode, 'w') || strchr(mode, 'a') || strchr(mode, '+'));
}

static void join(char *out, size_t n, const char *prefix, const char *path) {
    while (*path == '/') path++;
    snprintf(out, n, "%s/%s", prefix, path);
}

static FILE *try_open(const char *guest, const char *path, const char *mode) {
    FILE *f = fopen(path, mode);
    if (f && vfs_dbg()) {
        fprintf(stderr, "vfs: %s -> %s\n", guest, path);
    }
    return f;
}

/* Try each plausible host location; returns an open FILE* or NULL. */
static FILE *open_guest(const char *guest, const char *mode) {
    char path[1024];
    const char *mark;
    int write_mode = is_write_mode(mode);

    if (strstr(guest, "..")) return NULL;

    if (write_mode) {
        ensure_userdir();
        const char *base = strrchr(guest, '/');
        base = base ? base + 1 : guest;
        snprintf(path, sizeof path, "build/userdata/%s", base);
        return try_open(guest, path, mode);
    }

    FILE *f;

    /*
     * Saved data shadows shipped data. Writes always land in build/userdata/<base>, so reads must
     * consult that first — otherwise a saved profile is silently replaced by the pristine copy in
     * GloftF5HP/res on the next launch, which is exactly how progress and money reset to zero.
     */
    {
        const char *base = strrchr(guest, '/');
        base = base ? base + 1 : guest;
        snprintf(path, sizeof path, "build/userdata/%s", base);
        f = try_open(guest, path, mode);
        if (f) return f;
    }

    mark = strstr(guest, "/GloftF5HP/res/");
    if (mark) {
        join(path, sizeof path, "GloftF5HP/res", mark + strlen("/GloftF5HP/res/"));
        f = try_open(guest, path, mode);
        if (f) return f;
    }
    mark = strstr(guest, "/GloftF5HP/");
    if (mark) {
        join(path, sizeof path, "GloftF5HP", mark + strlen("/GloftF5HP/"));
        f = try_open(guest, path, mode);
        if (f) return f;
    }
    join(path, sizeof path, "GloftF5HP/res", guest);
    f = try_open(guest, path, mode);
    if (f) return f;
    join(path, sizeof path, "GloftF5HP", guest);
    f = try_open(guest, path, mode);
    if (f) return f;

    /* Derived overlay: assets extracted from shipped archives, never game data itself. */
    mark = strstr(guest, "/GloftF5HP/res/");
    const char *rel = mark ? mark + strlen("/GloftF5HP/res/") : guest;
    join(path, sizeof path, "build/overlay", rel);
    f = try_open(guest, path, mode);
    if (f) return f;

    f = fopen(guest, mode);
    if (f) return f;
    if (vfs_dbg()) fprintf(stderr, "vfs: %s -> MISS\n", guest);
    return NULL;
}

static VFile *slot_of(uint32_t handle) {
    if (handle < VFS_HANDLE_BASE) return NULL;
    uint32_t idx = (handle - VFS_HANDLE_BASE) / 0x20u;
    if (idx >= VFS_MAX_FILES || !g_files[idx].used) return NULL;
    return &g_files[idx];
}

static VFile *slot_of_fd(int fd) {
    int idx = fd - VFS_FD_BASE;
    if (idx < 0 || idx >= VFS_MAX_FILES || !g_files[idx].used) return NULL;
    return &g_files[idx];
}

uint32_t vfs_open(const char *guest, const char *mode) {
    if (!guest || !mode) return 0;
    FILE *fp = open_guest(guest, mode);
    if (!fp) return 0;
    for (int i = 0; i < VFS_MAX_FILES; i++) {
        if (!g_files[i].used) {
            g_files[i].used = 1;
            g_files[i].fp = fp;
            return VFS_HANDLE_BASE + (uint32_t)i * 0x20u;
        }
    }
    fclose(fp);
    return 0;
}

uint32_t vfs_read(uint32_t handle, void *dst, uint32_t size, uint32_t n) {
    VFile *v = slot_of(handle);
    if (!v || !dst) return 0;
    size_t got = fread(dst, size, n, v->fp);
    if (vfs_dbg()) {
        fprintf(stderr, "vfs: fread h=%x size=%u n=%u -> %zu first=%02x%02x%02x%02x\n",
                handle, size, n, got,
                got ? ((unsigned char *)dst)[0] : 0, got > 1 ? ((unsigned char *)dst)[1] : 0,
                got > 2 ? ((unsigned char *)dst)[2] : 0, got > 3 ? ((unsigned char *)dst)[3] : 0);
    }
    return (uint32_t)got;
}

uint32_t vfs_write(uint32_t handle, const void *src, uint32_t size, uint32_t n) {
    VFile *v = slot_of(handle);
    if (!v || !src) return 0;
    return (uint32_t)fwrite(src, size, n, v->fp);
}

int vfs_close(uint32_t handle) {
    VFile *v = slot_of(handle);
    if (!v) return -1;
    int rc = fclose(v->fp);
    v->fp = NULL;
    v->used = 0;
    return rc;
}

int vfs_seek(uint32_t handle, int32_t off, int whence) {
    VFile *v = slot_of(handle);
    int rc = v ? fseek(v->fp, off, whence) : -1;
    if (vfs_dbg()) fprintf(stderr, "vfs: fseek h=%x off=%d whence=%d -> %d\n", handle, off, whence, rc);
    return rc;
}

int32_t vfs_tell(uint32_t handle) {
    VFile *v = slot_of(handle);
    int32_t pos = v ? (int32_t)ftell(v->fp) : -1;
    if (vfs_dbg()) fprintf(stderr, "vfs: ftell h=%x -> %d\n", handle, pos);
    return pos;
}

int vfs_getc(uint32_t handle) {
    VFile *v = slot_of(handle);
    return v ? fgetc(v->fp) : -1;
}

int vfs_putc(int ch, uint32_t handle) {
    VFile *v = slot_of(handle);
    return v ? fputc(ch, v->fp) : -1;
}

char *vfs_gets(char *dst, int n, uint32_t handle) {
    VFile *v = slot_of(handle);
    return (v && dst && n > 0) ? fgets(dst, n, v->fp) : NULL;
}

int vfs_puts(const char *s, uint32_t handle) {
    VFile *v = slot_of(handle);
    return (v && s) ? fputs(s, v->fp) : -1;
}

int vfs_eof(uint32_t handle) {
    VFile *v = slot_of(handle);
    return v ? feof(v->fp) : 1;
}

int vfs_error(uint32_t handle) {
    VFile *v = slot_of(handle);
    return v ? ferror(v->fp) : 1;
}

int vfs_fileno(uint32_t handle) {
    VFile *v = slot_of(handle);
    if (!v) return -1;
    int idx = (int)(v - g_files);
    return VFS_FD_BASE + idx;
}

int vfs_fstat(uint32_t handle, void *st) {
    VFile *v = slot_of(handle);
    if (!v) return -1;
    if (st) memset(st, 0, 64);
    return 0;
}

uint32_t vfs_fdopen(int fd, const char *mode) {
    VFile *v = slot_of_fd(fd);
    (void)mode;
    if (!v) return 0;
    return VFS_HANDLE_BASE + (uint32_t)(v - g_files) * 0x20u;
}

int vfs_fstat_fd(int fd, void *st) {
    VFile *v = slot_of_fd(fd);
    if (!v) return -1;
    if (st) memset(st, 0, 64);
    if (vfs_dbg()) fprintf(stderr, "vfs: fstat fd=%d\n", fd);
    return 0;
}

int vfs_lseek_fd(int fd, int32_t off, int whence) {
    VFile *v = slot_of_fd(fd);
    return v ? fseek(v->fp, off, whence) : -1;
}

int vfs_read_fd(int fd, void *dst, uint32_t n) {
    VFile *v = slot_of_fd(fd);
    return (v && dst) ? (int)fread(dst, 1, n, v->fp) : -1;
}

int vfs_write_fd(int fd, const void *src, uint32_t n) {
    VFile *v = slot_of_fd(fd);
    return (v && src) ? (int)fwrite(src, 1, n, v->fp) : -1;
}

int vfs_close_fd(int fd) {
    VFile *v = slot_of_fd(fd);
    if (!v) return -1;
    int rc = fclose(v->fp);
    v->fp = NULL;
    v->used = 0;
    return rc;
}

int vfs_chdir(const char *path) {
    (void)path;                 /* paths are resolved explicitly; the host cwd never moves */
    return 0;
}
