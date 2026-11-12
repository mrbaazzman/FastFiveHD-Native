/*
 * Minimal Win32 + EGL/GLES2 bring-up proof.
 *
 * Loads the local ANGLE DLLs, creates a real window and GLES2 context, clears the
 * framebuffer, captures one RGBA frame to build/angle_test.bmp, and exits.
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef void *EGLDisplay;
typedef void *EGLConfig;
typedef void *EGLSurface;
typedef void *EGLContext;
typedef int32_t EGLint;
typedef uint32_t EGLBoolean;
typedef uint32_t EGLenum;

#define EGL_DEFAULT_DISPLAY 0
#define EGL_OPENGL_ES_API 0x30A0u
#define EGL_SURFACE_TYPE 0x3033
#define EGL_WINDOW_BIT 0x0004
#define EGL_RENDERABLE_TYPE 0x3040
#define EGL_OPENGL_ES2_BIT 0x0004
#define EGL_RED_SIZE 0x3024
#define EGL_GREEN_SIZE 0x3023
#define EGL_BLUE_SIZE 0x3022
#define EGL_ALPHA_SIZE 0x3021
#define EGL_DEPTH_SIZE 0x3025
#define EGL_STENCIL_SIZE 0x3026
#define EGL_NONE 0x3038
#define EGL_NO_SURFACE ((EGLSurface)0)
#define EGL_NO_CONTEXT ((EGLContext)0)
#define EGL_CONTEXT_CLIENT_VERSION 0x3098
#define GL_COLOR_BUFFER_BIT 0x00004000
#define GL_RGBA 0x1908
#define GL_UNSIGNED_BYTE 0x1401
#define GL_VENDOR 0x1F00
#define GL_RENDERER 0x1F01
#define GL_VERSION 0x1F02

typedef EGLDisplay (*PFN_eglGetDisplay)(void *);
typedef EGLBoolean (*PFN_eglInitialize)(EGLDisplay, EGLint *, EGLint *);
typedef EGLBoolean (*PFN_eglBindAPI)(EGLenum);
typedef EGLBoolean (*PFN_eglChooseConfig)(EGLDisplay, const EGLint *, EGLConfig *, EGLint, EGLint *);
typedef EGLSurface (*PFN_eglCreateWindowSurface)(EGLDisplay, EGLConfig, void *, const EGLint *);
typedef EGLContext (*PFN_eglCreateContext)(EGLDisplay, EGLConfig, EGLContext, const EGLint *);
typedef EGLBoolean (*PFN_eglMakeCurrent)(EGLDisplay, EGLSurface, EGLSurface, EGLContext);
typedef EGLBoolean (*PFN_eglSwapBuffers)(EGLDisplay, EGLSurface);
typedef void *(*PFN_eglGetProcAddress)(const char *);

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
    uint16_t planes = 1, bpp = 24; memcpy(hdr + 26, &planes, 2); memcpy(hdr + 28, &bpp, 2);
    uint32_t raw = (uint32_t)row * (uint32_t)h; memcpy(hdr + 34, &raw, 4);

    FILE *f = fopen(path, "wb");
    if (!f) return;
    fwrite(hdr, 1, sizeof hdr, f);
    unsigned char *line = (unsigned char *)calloc(1, (size_t)row);
    for (int y = h - 1; y >= 0; y--) {
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

static LRESULT CALLBACK wndproc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m == WM_DESTROY) { PostQuitMessage(0); return 0; }
    return DefWindowProcA(h, m, w, l);
}

int main(void) {
    const char *angle_dir = "C:\\Users\\Baz\\AppData\\Local\\Programs\\Command Code";
    SetDllDirectoryA(angle_dir);
    char egl_path[512];
    snprintf(egl_path, sizeof egl_path, "%s\\libEGL.dll", angle_dir);
    HMODULE egl = LoadLibraryA(egl_path);
    if (!egl) {
        fprintf(stderr, "LoadLibrary(libEGL) failed: %lu\n", GetLastError());
        return 2;
    }

    PFN_eglGetDisplay get_display = (PFN_eglGetDisplay)GetProcAddress(egl, "eglGetDisplay");
    PFN_eglInitialize initialize = (PFN_eglInitialize)GetProcAddress(egl, "eglInitialize");
    PFN_eglBindAPI bind_api = (PFN_eglBindAPI)GetProcAddress(egl, "eglBindAPI");
    PFN_eglChooseConfig choose_config = (PFN_eglChooseConfig)GetProcAddress(egl, "eglChooseConfig");
    PFN_eglCreateWindowSurface create_surface = (PFN_eglCreateWindowSurface)GetProcAddress(egl, "eglCreateWindowSurface");
    PFN_eglCreateContext create_context = (PFN_eglCreateContext)GetProcAddress(egl, "eglCreateContext");
    PFN_eglMakeCurrent make_current = (PFN_eglMakeCurrent)GetProcAddress(egl, "eglMakeCurrent");
    PFN_eglSwapBuffers swap_buffers = (PFN_eglSwapBuffers)GetProcAddress(egl, "eglSwapBuffers");
    PFN_eglGetProcAddress get_proc = (PFN_eglGetProcAddress)GetProcAddress(egl, "eglGetProcAddress");
    if (!get_display || !initialize || !bind_api || !choose_config || !create_surface ||
        !create_context || !make_current || !swap_buffers || !get_proc) {
        fprintf(stderr, "missing EGL entry points\n");
        return 3;
    }

    WNDCLASSEXA wc;
    memset(&wc, 0, sizeof wc);
    wc.cbSize = sizeof wc;
    wc.style = CS_OWNDC | CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = wndproc;
    wc.hInstance = GetModuleHandleA(NULL);
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.lpszClassName = "Fast5AngleTest";
    if (!RegisterClassExA(&wc)) { fprintf(stderr, "RegisterClass failed\n"); return 4; }

    int width = 800, height = 480;
    RECT r = {0, 0, width, height};
    AdjustWindowRect(&r, WS_OVERLAPPEDWINDOW, FALSE);
    HWND hwnd = CreateWindowExA(0, wc.lpszClassName, "Fast Five HD - ANGLE test",
                                WS_OVERLAPPEDWINDOW | WS_CLIPSIBLINGS | WS_CLIPCHILDREN | WS_VISIBLE,
                                CW_USEDEFAULT, CW_USEDEFAULT, r.right - r.left, r.bottom - r.top,
                                NULL, NULL, wc.hInstance, NULL);
    if (!hwnd) { fprintf(stderr, "CreateWindow failed\n"); return 5; }
    ShowWindow(hwnd, SW_SHOW);
    UpdateWindow(hwnd);

    EGLDisplay dpy = get_display((void *)(uintptr_t)EGL_DEFAULT_DISPLAY);
    EGLint major = 0, minor = 0;
    if (!dpy || !initialize(dpy, &major, &minor)) {
        fprintf(stderr, "eglInitialize failed (0x%x)\n", dpy ? 0 : 1);
        return 6;
    }
    if (!bind_api(EGL_OPENGL_ES_API)) { fprintf(stderr, "eglBindAPI failed\n"); return 7; }

    const EGLint cfg_attr[] = {
        EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
        EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
        EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8,
        EGL_DEPTH_SIZE, 24, EGL_STENCIL_SIZE, 8,
        EGL_NONE
    };
    EGLConfig cfg = NULL;
    EGLint ncfg = 0;
    if (!choose_config(dpy, cfg_attr, &cfg, 1, &ncfg) || ncfg < 1) {
        fprintf(stderr, "eglChooseConfig failed\n");
        return 8;
    }
    EGLSurface surf = create_surface(dpy, cfg, hwnd, NULL);
    if (!surf) { fprintf(stderr, "eglCreateWindowSurface failed\n"); return 9; }
    const EGLint ctx_attr[] = { EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE };
    EGLContext ctx = create_context(dpy, cfg, NULL, ctx_attr);
    if (!ctx) { fprintf(stderr, "eglCreateContext failed\n"); return 10; }
    if (!make_current(dpy, surf, surf, ctx)) { fprintf(stderr, "eglMakeCurrent failed\n"); return 11; }

    typedef void (*PFN_glClearColor)(float, float, float, float);
    typedef void (*PFN_glClear)(uint32_t);
    typedef void (*PFN_glViewport)(int, int, int, int);
    typedef void (*PFN_glReadPixels)(int, int, int, int, uint32_t, uint32_t, void *);
    typedef const unsigned char *(*PFN_glGetString)(uint32_t);
    PFN_glClearColor clear_color = (PFN_glClearColor)get_proc("glClearColor");
    PFN_glClear clear = (PFN_glClear)get_proc("glClear");
    PFN_glViewport viewport = (PFN_glViewport)get_proc("glViewport");
    PFN_glReadPixels read_pixels = (PFN_glReadPixels)get_proc("glReadPixels");
    PFN_glGetString get_string = (PFN_glGetString)get_proc("glGetString");
    if (!clear_color || !clear || !viewport || !read_pixels || !get_string) {
        fprintf(stderr, "missing GL entry points\n");
        return 12;
    }
    printf("EGL %d.%d\n", major, minor);
    printf("GL_VERSION  %s\n", get_string(GL_VERSION));
    printf("GL_VENDOR   %s\n", get_string(GL_VENDOR));
    printf("GL_RENDERER %s\n", get_string(GL_RENDERER));

    viewport(0, 0, width, height);
    unsigned char *pixels = (unsigned char *)malloc((size_t)width * (size_t)height * 4u);
    for (int i = 0; i < 90; i++) {
        MSG msg;
        while (PeekMessageA(&msg, NULL, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageA(&msg);
        }
        clear_color(0.12f, 0.35f, 0.62f, 1.0f);
        clear(GL_COLOR_BUFFER_BIT);
        if (i == 10 && pixels) {
            read_pixels(0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
            write_bmp("build/angle_test.bmp", width, height, pixels);
            printf("captured build/angle_test.bmp\n");
        }
        swap_buffers(dpy, surf);
        Sleep(16);
    }
    free(pixels);
    make_current(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    return 0;
}
