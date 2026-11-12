/*
 * Win32 window + real GLES2 context through the local x64 ANGLE runtime.
 *
 * The window owns the EGL surface; the translated game's GL imports are dispatched by
 * gl_win32.c through platform_gl_proc(). Key presses are exposed to the harness so a key can be
 * mapped to a synthetic touch.
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "platform.h"
#include "resolution.h"

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
#define EGL_CONTEXT_CLIENT_VERSION 0x3098
#define EGL_WIDTH 0x3057
#define EGL_HEIGHT 0x3056
#define EGL_NO_SURFACE ((EGLSurface)0)
#define EGL_NO_CONTEXT ((EGLContext)0)

typedef EGLDisplay (*PFN_eglGetDisplay)(void *);
typedef EGLBoolean (*PFN_eglInitialize)(EGLDisplay, EGLint *, EGLint *);
typedef EGLBoolean (*PFN_eglBindAPI)(EGLenum);
typedef EGLBoolean (*PFN_eglChooseConfig)(EGLDisplay, const EGLint *, EGLConfig *, EGLint, EGLint *);
typedef EGLSurface (*PFN_eglCreateWindowSurface)(EGLDisplay, EGLConfig, void *, const EGLint *);
typedef EGLContext (*PFN_eglCreateContext)(EGLDisplay, EGLConfig, EGLContext, const EGLint *);
typedef EGLBoolean (*PFN_eglMakeCurrent)(EGLDisplay, EGLSurface, EGLSurface, EGLContext);
typedef EGLBoolean (*PFN_eglSwapBuffers)(EGLDisplay, EGLSurface);
typedef EGLBoolean (*PFN_eglTerminate)(EGLDisplay);
typedef EGLBoolean (*PFN_eglQuerySurface)(EGLDisplay, EGLSurface, EGLint, EGLint *);
typedef void *(*PFN_eglGetProcAddress)(const char *);

static HWND g_hwnd;
static HMODULE g_egl_module;
static EGLDisplay g_display;
static EGLSurface g_surface;
static EGLContext g_context;
static PFN_eglGetProcAddress g_get_proc;
static PFN_eglSwapBuffers g_swap;
static PFN_eglTerminate g_terminate;
static int g_quit;
static int g_key;
static int g_width = 800;
static int g_height = 480;
/* The resolution the guest engine runs at; the window may be any size. */
static int g_design_w = 800;
static int g_design_h = 480;

/* Real keyboard state: Android key codes, queued as press/release events. */
#define KEY_QUEUE_SIZE 64
static struct {
    int code;
    int down;
} g_key_queue[KEY_QUEUE_SIZE];
static int g_key_head;
static int g_key_tail;

/*
 * Live key state, kept separately from the event queue.
 *
 * platform_take_game_key() drains the queue, so scanning the queue for "is this key still down"
 * (the previous approach) always reported released after the event had been consumed. The engine
 * needs level-triggered state: it polls steering every frame. A bitmap indexed by Android key code
 * answers that in O(1) and survives queue consumption.
 */
#define KEY_STATE_MAX 320
static unsigned char g_key_down[KEY_STATE_MAX];

static void queue_key(int code, int down) {
    if (code > 0 && code < KEY_STATE_MAX) g_key_down[code] = (unsigned char)(down != 0);
    int next = (g_key_tail + 1) % KEY_QUEUE_SIZE;
    if (next == g_key_head) return;
    g_key_queue[g_key_tail].code = code;
    g_key_queue[g_key_tail].down = down;
    g_key_tail = next;
}

/* Android key codes used by the engine (android.view.KeyEvent). */
static int win32_to_android_key(WPARAM vk) {
    switch (vk) {
    case VK_LEFT:  return 21; /* KEYCODE_DPAD_LEFT */
    case VK_RIGHT: return 22; /* KEYCODE_DPAD_RIGHT */
    case VK_UP:    return 19; /* KEYCODE_DPAD_UP */
    case VK_DOWN:  return 20; /* KEYCODE_DPAD_DOWN */
    case VK_RETURN:return 66; /* KEYCODE_ENTER */
    case VK_ESCAPE:return 4;  /* KEYCODE_BACK */
    case 'A':      return 29; /* KEYCODE_A */
    case 'D':      return 32; /* KEYCODE_D */
    case 'W':      return 51; /* KEYCODE_W */
    case 'S':      return 47; /* KEYCODE_S */
    case VK_SPACE: return 62; /* KEYCODE_SPACE */
    case 'C':      return 31; /* KEYCODE_C */
    case 'R':      return 46; /* KEYCODE_R */
    case 'Z':      return 54; /* KEYCODE_Z */
    case VK_CONTROL: return 113; /* KEYCODE_CTRL_LEFT */
    case VK_LCONTROL:return 113;
    case VK_RCONTROL:return 114; /* KEYCODE_CTRL_RIGHT */
    default:       return 0;
    }
}

#define POINTER_QUEUE_SIZE 32
static struct {
    int x, y, down;
} g_pointer_queue[POINTER_QUEUE_SIZE];
static int g_pointer_head;
static int g_pointer_tail;

static void queue_pointer_event(HWND h, LPARAM l, int down) {
    RECT client;
    GetClientRect(h, &client);
    int width = client.right > 0 ? client.right : g_width;
    int height = client.bottom > 0 ? client.bottom : g_height;
    int x = (int)(short)LOWORD(l);
    int y = (int)(short)HIWORD(l);
    /* The guest engine always runs at the design resolution, so the window position is
     * mapped into the design space with the same transform the 2D layer uses. */
    double dx, dy;
    fast5_resolution_to_design((double)x, (double)y, &dx, &dy);
    x = (int)dx;
    y = (int)dy;
    if (getenv("FAST5_POINTER_TRACE")) {
        fprintf(stderr, "pointer: window %d,%d -> design %d,%d\n",
                (int)(short)LOWORD(l), (int)(short)HIWORD(l), x, y);
    }
    if (x < 0) x = 0;
    if (y < 0) y = 0;
    if (x >= g_design_w) x = g_design_w - 1;
    if (y >= g_design_h) y = g_design_h - 1;
    int next = (g_pointer_tail + 1) % POINTER_QUEUE_SIZE;
    if (next == g_pointer_head) return;
    g_pointer_queue[g_pointer_tail].x = x;
    g_pointer_queue[g_pointer_tail].y = y;
    g_pointer_queue[g_pointer_tail].down = down;
    g_pointer_tail = next;
}

static LRESULT CALLBACK platform_wndproc(HWND h, UINT m, WPARAM w, LPARAM l) {
    switch (m) {
    case WM_SYSCOMMAND:
        if ((w & 0xfff0u) == SC_CLOSE) {
            g_quit = 1;
            return 0;
        }
        break;
    case WM_CLOSE:
        g_quit = 1;
        return 0;
    case WM_DESTROY:
        g_quit = 1;
        PostQuitMessage(0);
        return 0;
    case WM_KEYDOWN: {
        int code = win32_to_android_key(w);
        if (code) {
            if (!(l & 0x40000000)) queue_key(code, 1); /* ignore auto-repeat */
            return 0;
        }
        if (w == VK_RETURN || w == VK_SPACE) g_key = (int)w;
        return 0;
    }
    case WM_KEYUP: {
        int code = win32_to_android_key(w);
        if (code) {
            queue_key(code, 0);
            return 0;
        }
        return 0;
    }
    case WM_LBUTTONDOWN:
        SetCapture(h);
        queue_pointer_event(h, l, 1);
        return 0;
    case WM_LBUTTONUP:
        queue_pointer_event(h, l, 0);
        ReleaseCapture();
        return 0;
    default:
        return DefWindowProcA(h, m, w, l);
    }
}

static HMODULE load_angle(void) {
    /*
     * Find a GLES2 backend.  The port talks to a real EGL/GLES2 implementation; ANGLE
     * (Google's GL ES 2.0 over Direct3D 11) is the one this project was developed against,
     * and every current Chromium build ships it.
     *
     * The search is: next to the executable, the tree's own third_party/angle, then the
     * usual places a Chromium-based browser keeps its ANGLE DLLs.
     */
    const char *env = getenv("FAST5_ANGLE_DIR");
    const char *dirs[] = {
        "third_party\\angle",
        ".",
        env,
        "C:\\Program Files\\Google\\Chrome\\Application",
        "C:\\Program Files (x86)\\Google\\Chrome\\Application",
        "C:\\Program Files\\Microsoft\\Edge\\Application",
        "C:\\Program Files (x86)\\Microsoft\\Edge\\Application",
        "C:\\Program Files\\BraveSoftware\\Brave-Browser\\Application",
        "C:\\Program Files (x86)\\BraveSoftware\\Brave-Browser\\Application",
        "C:\\Program Files\\Microsoft\\EdgeDev",
        "C:\\Users\\Baz\\AppData\\Local\\Programs\\Command Code",
    };
    /* Chromium keeps the DLLs in a versioned subdirectory; find it if the flat one is absent. */
    for (unsigned i = 0; i < sizeof dirs / sizeof dirs[0]; i++) {
        if (!dirs[i]) continue;
        char path[512];
        snprintf(path, sizeof path, "%s\\libEGL.dll", dirs[i]);
        SetDllDirectoryA(dirs[i]);
        HMODULE m = LoadLibraryA(path);
        if (m) {
            fprintf(stderr, "platform: ANGLE loaded from %s\n", path);
            return m;
        }
    }
    /* Try the versioned directories of a Chromium install. */
    for (unsigned i = 0; i < sizeof dirs / sizeof dirs[0]; i++) {
        if (!dirs[i]) continue;
        char base[512];
        snprintf(base, sizeof base, "%s", dirs[i]);
        for (int v = 0; v < 200; v++) {
            char path[600];
            snprintf(path, sizeof path, "%s\\%d.0.0.0\\libEGL.dll", base, v);
            if (GetFileAttributesA(path) == INVALID_FILE_ATTRIBUTES) continue;
            SetDllDirectoryA(path);
            HMODULE m = LoadLibraryA(path);
            if (m) {
                fprintf(stderr, "platform: ANGLE loaded from %s\n", path);
                return m;
            }
        }
    }
    fprintf(stderr, "platform: no GLES2 backend (libEGL.dll) found.\n");
    fprintf(stderr, "platform: install ANGLE, or drop libEGL.dll and libGLESv2.dll into\n");
    fprintf(stderr, "platform: third_party/angle/ - see third_party/angle/README.md.\n");
    return NULL;
}

static int host_dimension(const char *name, int fallback) {
    const char *value = getenv(name);
    if (value && *value) {
        long parsed = strtol(value, NULL, 10);
        if (parsed >= 320 && parsed <= 16384) return (int)parsed;
    }
    return fallback;
}

int platform_init(int width, int height, const char *title) {
    /* System-DPI aware: GetSystemMetrics/CreateWindow then use physical pixels. */
    SetProcessDPIAware();
    int want_width = host_dimension("FAST5_WIDTH", width);
    int want_height = host_dimension("FAST5_HEIGHT", height);
    int monitor_width = GetSystemMetrics(SM_CXSCREEN);
    int monitor_height = GetSystemMetrics(SM_CYSCREEN);
    if (monitor_width > 0 && want_width > monitor_width) want_width = monitor_width;
    if (monitor_height > 0 && want_height > monitor_height) want_height = monitor_height;
    g_width = want_width;
    g_height = want_height;
    /*
     * Borderless mode.
     *
     * A framed window needs extra pixels for the caption and the borders, so a client area of
     * the full screen size does not fit: Windows clamps the window and the client area ends up
     * smaller than requested (the engine then renders into a surface it cannot present). A
     * borderless window has no frame, so the client area is exactly the requested size.
     * It is used when the request does not fit the screen, or when FAST5_BORDERLESS=1 asks
     * for it explicitly.
     */
    int borderless = getenv("FAST5_BORDERLESS") != NULL;
    if (getenv("FAST5_WINDOWED")) borderless = 0;
    if (monitor_width > 0 && monitor_height > 0 &&
        (g_width >= monitor_width || g_height >= monitor_height)) {
        borderless = 1;
    }

    RECT frame = {0, 0, g_width, g_height};
    DWORD style = borderless ? (WS_POPUP | WS_CLIPSIBLINGS | WS_CLIPCHILDREN)
                             : (WS_OVERLAPPEDWINDOW | WS_CLIPSIBLINGS | WS_CLIPCHILDREN);
    if (!borderless) AdjustWindowRectEx(&frame, style, FALSE, WS_EX_APPWINDOW);
    int window_w = frame.right - frame.left;
    int window_h = frame.bottom - frame.top;
    int window_x = (monitor_width - window_w) / 2;
    int window_y = (monitor_height - window_h) / 2;
    if (window_x < 0) window_x = 0;
    if (window_y < 0) window_y = 0;

    g_egl_module = load_angle();
    if (!g_egl_module) {
        fprintf(stderr, "platform: could not load libEGL.dll\n");
        return 0;
    }

    PFN_eglGetDisplay get_display = (PFN_eglGetDisplay)GetProcAddress(g_egl_module, "eglGetDisplay");
    PFN_eglInitialize initialize = (PFN_eglInitialize)GetProcAddress(g_egl_module, "eglInitialize");
    PFN_eglBindAPI bind_api = (PFN_eglBindAPI)GetProcAddress(g_egl_module, "eglBindAPI");
    PFN_eglChooseConfig choose_config = (PFN_eglChooseConfig)GetProcAddress(g_egl_module, "eglChooseConfig");
    PFN_eglCreateWindowSurface create_surface = (PFN_eglCreateWindowSurface)GetProcAddress(g_egl_module, "eglCreateWindowSurface");
    PFN_eglQuerySurface query_surface = (PFN_eglQuerySurface)GetProcAddress(g_egl_module, "eglQuerySurface");
    PFN_eglCreateContext create_context = (PFN_eglCreateContext)GetProcAddress(g_egl_module, "eglCreateContext");
    PFN_eglMakeCurrent make_current = (PFN_eglMakeCurrent)GetProcAddress(g_egl_module, "eglMakeCurrent");
    g_swap = (PFN_eglSwapBuffers)GetProcAddress(g_egl_module, "eglSwapBuffers");
    g_terminate = (PFN_eglTerminate)GetProcAddress(g_egl_module, "eglTerminate");
    g_get_proc = (PFN_eglGetProcAddress)GetProcAddress(g_egl_module, "eglGetProcAddress");
    if (!get_display || !initialize || !bind_api || !choose_config || !create_surface ||
        !create_context || !make_current || !g_swap || !g_get_proc) {
        fprintf(stderr, "platform: missing EGL entry points\n");
        return 0;
    }

    WNDCLASSEXA wc;
    memset(&wc, 0, sizeof wc);
    wc.cbSize = sizeof wc;
    wc.style = CS_OWNDC | CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = platform_wndproc;
    wc.hInstance = GetModuleHandleA(NULL);
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.lpszClassName = "FastFiveHD";
    if (!RegisterClassExA(&wc)) return 0;

    g_hwnd = CreateWindowExA(WS_EX_APPWINDOW, wc.lpszClassName, title,
                             style | WS_VISIBLE,
                             window_x, window_y, window_w, window_h,
                             NULL, NULL, wc.hInstance, NULL);
    if (!g_hwnd) {
        fprintf(stderr, "platform: CreateWindow failed\n");
        return 0;
    }
    if (getenv("FAST5_TEST_MINIMIZED")) {
        /* Test runs render into a hidden window so they never cover the desktop. The EGL
           surface keeps the full client size; captures read the back buffer as usual. */
        ShowWindow(g_hwnd, SW_HIDE);
        SetWindowPos(g_hwnd, HWND_BOTTOM, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_HIDEWINDOW);
        fprintf(stderr, "platform: test window hidden (FAST5_TEST_MINIMIZED)\n");
    } else {
        ShowWindow(g_hwnd, SW_SHOW);
        SetForegroundWindow(g_hwnd);
        SetFocus(g_hwnd);
        UpdateWindow(g_hwnd);
    }
    {
        RECT wr, cr;
        GetWindowRect(g_hwnd, &wr);
        GetClientRect(g_hwnd, &cr);
        /* The framebuffer is the client area, so the presentation size follows it. A framed
         * window that does not fit the screen is clamped by Windows, and the client area then
         * is smaller than the requested size. */
        if (cr.right > 0 && cr.bottom > 0) {
            g_width = cr.right;
            g_height = cr.bottom;
        }
        fprintf(stderr,
                "platform: requested %dx%d; window %ldx%ld at %ld,%ld; client %ldx%ld%s\n",
                want_width, want_height, (long)(wr.right - wr.left), (long)(wr.bottom - wr.top),
                (long)wr.left, (long)wr.top, (long)cr.right, (long)cr.bottom,
                borderless ? " (borderless)" : "");
    }

    g_display = get_display((void *)(uintptr_t)EGL_DEFAULT_DISPLAY);
    EGLint major = 0, minor = 0;
    if (!g_display || !initialize(g_display, &major, &minor)) {
        fprintf(stderr, "platform: eglInitialize failed\n");
        return 0;
    }
    if (!bind_api(EGL_OPENGL_ES_API)) return 0;

    const EGLint cfg_attr[] = {
        EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
        EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
        EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8,
        EGL_DEPTH_SIZE, 24, EGL_STENCIL_SIZE, 8,
        EGL_NONE
    };
    EGLConfig cfg = NULL;
    EGLint ncfg = 0;
    if (!choose_config(g_display, cfg_attr, &cfg, 1, &ncfg) || ncfg < 1) return 0;
    g_surface = create_surface(g_display, cfg, g_hwnd, NULL);
    if (!g_surface) return 0;
    const EGLint ctx_attr[] = {EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE};
    g_context = create_context(g_display, cfg, NULL, ctx_attr);
    if (!g_context) return 0;
    if (!make_current(g_display, g_surface, g_surface, g_context)) return 0;
    if (query_surface) {
        EGLint surface_w = 0, surface_h = 0;
        query_surface(g_display, g_surface, EGL_WIDTH, &surface_w);
        query_surface(g_display, g_surface, EGL_HEIGHT, &surface_h);
        fprintf(stderr, "platform: EGL surface %dx%d\n", (int)surface_w, (int)surface_h);
    }

    fprintf(stderr, "platform: EGL %d.%d, window %dx%d\n",
            major, minor, g_width, g_height);
    return 1;
}

int platform_width(void) { return g_width; }

int platform_height(void) { return g_height; }

void platform_set_design_size(int width, int height) {
    if (width > 0) g_design_w = width;
    if (height > 0) g_design_h = height;
}

void platform_poll(void) {
    MSG msg;
    while (PeekMessageA(&msg, NULL, 0, 0, PM_REMOVE)) {
        TranslateMessage(&msg);
        DispatchMessageA(&msg);
    }
}

void platform_swap(void) {
    if (g_display && g_surface && g_swap) g_swap(g_display, g_surface);
}

void platform_shutdown(void) {
    if (g_display && g_context) {
        /* release the current context before terminating EGL */
        /* eglMakeCurrent(dpy, NO_SURFACE, NO_SURFACE, NO_CONTEXT) via proc */
    }
    if (g_display && g_terminate) g_terminate(g_display);
    if (g_hwnd) DestroyWindow(g_hwnd);
    g_hwnd = NULL;
    if (g_egl_module) FreeLibrary(g_egl_module);
    g_egl_module = NULL;
}

int platform_should_quit(void) { return g_quit; }

int platform_take_key(void) {
    int k = g_key;
    g_key = 0;
    return k;
}

int platform_take_game_key(int *code, int *down) {
    if (g_key_head == g_key_tail) return 0;
    if (code) *code = g_key_queue[g_key_head].code;
    if (down) *down = g_key_queue[g_key_head].down;
    g_key_head = (g_key_head + 1) % KEY_QUEUE_SIZE;
    return 1;
}

int platform_held_key(int code) {
    if (code <= 0 || code >= KEY_STATE_MAX) return 0;
    return g_key_down[code] != 0;
}

void platform_inject_key(int code, int down) { queue_key(code, down); }

int platform_take_pointer(int *x, int *y, int *down) {
    if (g_pointer_head == g_pointer_tail) return 0;
    if (x) *x = g_pointer_queue[g_pointer_head].x;
    if (y) *y = g_pointer_queue[g_pointer_head].y;
    if (down) *down = g_pointer_queue[g_pointer_head].down;
    g_pointer_head = (g_pointer_head + 1) % POINTER_QUEUE_SIZE;
    return 1;
}

void *platform_gl_proc(const char *name) {
    return g_get_proc ? g_get_proc(name) : NULL;
}
