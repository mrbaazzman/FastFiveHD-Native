/*
 * GLES2 import bridge for the native Windows build.
 *
 * Guest ARM code calls GL through its PLT; arguments arrive in r0-r3 and the guest stack with
 * softfp float bits. Each import is translated to the matching ANGLE GLES2 entry point. Guest
 * pointers are guest-memory offsets; they become host pointers into the same guest memory.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

#include "cpu.h"
#include "mem.h"
#include "platform.h"
#include "resolution.h"

extern uint32_t guest_mem_size;

typedef unsigned int GLenum;
typedef unsigned int GLuint;
typedef int GLint;
typedef int GLsizei;
typedef unsigned char GLboolean;
typedef unsigned int GLbitfield;
typedef float GLfloat;

#define GL_ARRAY_BUFFER 0x8892u
#define GL_ELEMENT_ARRAY_BUFFER 0x8893u
#define GL_RGB 0x1907u
#define GL_RGBA 0x1908u
#define GL_UNSIGNED_BYTE 0x1401u
#define GL_BGR_EXT 0x80E0u
#define GL_BGRA_EXT 0x80E1u
#define GL_COMPILE_STATUS 0x8B81u
#define GL_LINK_STATUS 0x8B82u
#define GL_INFO_LOG_LENGTH 0x8B84u

#define GL_FUNCS(X) \
    X(ActiveTexture) X(AttachShader) X(BindAttribLocation) X(BindBuffer) X(BindFramebuffer) X(BindRenderbuffer) \
    X(BindTexture) X(BlendColor) X(BlendEquation) X(BlendFunc) X(BufferData) X(BufferSubData) \
    X(CheckFramebufferStatus) X(Clear) X(ClearColor) X(ClearDepthf) X(ClearStencil) X(ColorMask) \
    X(CompileShader) X(CompressedTexImage2D) X(CompressedTexSubImage2D) X(CopyTexSubImage2D) \
    X(CreateProgram) X(CreateShader) X(CullFace) X(DeleteBuffers) X(DeleteFramebuffers) \
    X(DeleteProgram) X(DeleteRenderbuffers) X(DeleteShader) X(DeleteTextures) X(DepthFunc) \
    X(DepthMask) X(DepthRangef) X(Disable) X(DisableVertexAttribArray) X(DrawArrays) \
    X(DrawElements) X(Enable) X(EnableVertexAttribArray) X(Flush) X(FramebufferRenderbuffer) \
    X(FramebufferTexture2D) X(FrontFace) X(GenBuffers) X(GenFramebuffers) X(GenRenderbuffers) \
    X(GenTextures) X(GenerateMipmap) X(GetActiveAttrib) X(GetActiveUniform) X(GetAttribLocation) \
    X(GetError) X(GetFloatv) X(GetIntegerv) X(GetProgramInfoLog) X(GetProgramiv) \
    X(GetShaderInfoLog) X(GetShaderiv) X(GetString) X(GetUniformLocation) X(LineWidth) \
    X(LinkProgram) X(PixelStorei) X(PolygonOffset) X(ReadPixels) X(RenderbufferStorage) \
    X(SampleCoverage) X(Scissor) X(ShaderSource) X(StencilFunc) X(StencilMask) X(StencilOp) \
    X(TexImage2D) X(TexParameterf) X(TexParameteri) X(TexSubImage2D) X(Uniform1f) X(Uniform1fv) \
    X(Uniform1i) X(Uniform1iv) X(Uniform2fv) X(Uniform2iv) X(Uniform3fv) X(Uniform3iv) \
    X(Uniform4fv) X(Uniform4iv) X(UniformMatrix4fv) X(UseProgram) X(VertexAttrib4f) \
    X(VertexAttribPointer) X(Viewport)

#define DECL(n) static void *g_##n;
GL_FUNCS(DECL)
#undef DECL

static GLuint g_array_buffer;
static GLuint g_element_buffer;
static int g_compressed_logs;
static int g_shader_logs;
static int g_shader_seq;
static int g_fake_gpu;
static GLuint g_bound_fbo;
static int g_fbo_logs;
static int g_error_logs;
static int g_draw_logs;
static int g_skip_next_draw;

void gl_bridge_set_skip_draw(int skip) { g_skip_next_draw = skip; }
static int g_state_logs;
static GLuint g_current_program;
static GLenum g_active_texture = 0x84C0u;
static GLuint g_bound_texture[16];
static int g_scissor_test;
static int g_scissor_rect[4];
static int g_depth_test;
static int g_cull_face;
static int g_blend;
static int g_no_cull;
static int g_matrix_logs;
static int g_texture_logs;
static unsigned char g_texmap_seen[256];
static int g_texmap_logs;
static int g_tex_image_logs;
static int g_vertex_logs;
static int g_attrloc_logs;
static int g_attr_call_logs;
static int g_buffer_logs;
static int g_tex_dumps;
static int g_programiv_logs;
static int g_active_attrib_logs;
static int g_attr_state_logs;
static int g_trace;
/* Texture BMP dumping is a diagnostic, not part of normal play: it writes multi-megabyte files to
 * disk on first upload of every matching texture. FAST5_TEX_DUMP=1 turns it back on. */
static int g_tex_dump = -1;
#define MAX_GL_OBJECTS 1024
static unsigned char g_program_exists[MAX_GL_OBJECTS];
static const char *g_last_gl_call;
static uint32_t g_attr_ptr[16];
static int g_attr_stride[16];
static int g_attr_size[16];
static int g_attr_enabled[16];
static char *g_shader_src[MAX_GL_OBJECTS];
static GLuint g_prog_shaders[MAX_GL_OBJECTS][4];
static int g_prog_shader_count[MAX_GL_OBJECTS];

int gl_bridge_bound_fbo(void) { return (int)g_bound_fbo; }

static uint32_t g_pending_tex;
/*
 * Guest texture object -> GL texture id. Diagnostic only: the engine owns sampler binding and the
 * bridge never overrides it. The table feeds the texmap/name traces and render-target dumps.
 * g_gl_id is shared by every GL object kind, so it climbs far past 256; the table is 8192 entries.
 */
#define TEX_MAP_MAX 8192u
static uint32_t g_tex_ptr_map[TEX_MAP_MAX];
static char g_tex_name[TEX_MAP_MAX][48];  /* object name per GL id, for diagnostics/RTT dumps */
static uint32_t g_tex_map_high;      /* one past the highest id ever recorded */
static uint32_t g_tex_id_peak;       /* one past the highest texture id ever bound */
static unsigned char g_tex_upload_logged[256];
static int g_tex_audit;              /* FAST5_TEX_AUDIT=1: report map/identity inconsistencies */
static int g_tex_w[TEX_MAP_MAX], g_tex_h[TEX_MAP_MAX];
static GLuint g_tex_fbo[TEX_MAP_MAX];  /* FBO this texture is the colour attachment of */
static GLuint g_fbo_color_tex[256];    /* FBO -> its colour-attachment texture id */
static unsigned g_gl_frame;            /* frame number supplied by the harness */
static int g_rtt_dump = -1;            /* FAST5_RTT_DUMP=1: dump post-effect RTTs */
static unsigned char g_rtt_dumped[TEX_MAP_MAX];

/*
 * The size of the render target that is currently bound, in pixels.
 *
 * The 2D layer is authored in the game's design space (800x480). When the engine renders a
 * 2D screen into a render target (the menu's RTT screens), the target has its own size and
 * the content must be scaled to that target, not to the window.
 */
int gl_bridge_fbo_size(int fbo, int *w, int *h) {
    if (w) *w = 0;
    if (h) *h = 0;
    if (fbo <= 0 || fbo >= 256) return 0;
    unsigned tex = g_fbo_color_tex[fbo];
    if (!tex || tex >= TEX_MAP_MAX) return 0;
    if (g_tex_w[tex] <= 0 || g_tex_h[tex] <= 0) return 0;
    if (w) *w = g_tex_w[tex];
    if (h) *h = g_tex_h[tex];
    return 1;
}
static unsigned char g_fbo_dumped[256];

static unsigned g_fbo_bind_count;      /* framebuffer binds since the previous tick */
static GLuint g_fbo_bind_ids[16];
static unsigned g_fbo_bind_uniq;

void gl_bridge_frame_tick(unsigned frame) {
    if (g_rtt_dump < 0) {
        const char *e = getenv("FAST5_RTT_DUMP");
        g_rtt_dump = e && *e && strcmp(e, "0") != 0;
    }
    if (g_rtt_dump && frame && (frame % 300u) == 0u) {
        fprintf(stderr, "fbo_tick: frame=%u prev_binds=%u ids=", frame, g_fbo_bind_count);
        for (unsigned i = 0; i < g_fbo_bind_uniq; i++) fprintf(stderr, "%u,", g_fbo_bind_ids[i]);
        fprintf(stderr, "\n");
    }
    g_fbo_bind_count = 0;
    g_fbo_bind_uniq = 0;
    g_gl_frame = frame;
}

/* Highest texture id the engine has used so far + 1. Reported with each frame capture; it proves
   whether the id space outgrew the old 256-entry map. */
uint32_t gl_bridge_texture_id_peak(void) { return g_tex_id_peak; }

/* Forget everything recorded for one GL texture id.
 *
 * The engine deletes textures when a scene is torn down and ANGLE recycles the numeric name for
 * the next one. Leaving the old entry in place makes the map keep answering "this guest texture
 * lives at id N" long after id N became a different image — a wrong texture that lasts until the
 * map happens to be rewritten. */
static void tex_map_forget(GLuint id) {
    if (id >= TEX_MAP_MAX) return;
    g_tex_ptr_map[id] = 0;
    g_tex_name[id][0] = 0;
}

void gl_bridge_note_texture(uint32_t t) { g_pending_tex = t; }

static void rtt_dump_fbo(GLuint fbo);

static uint32_t arg(Cpu *c, uint8_t *mem, int i) {
    switch (i) {
    case 0: return c->r0;
    case 1: return c->r1;
    case 2: return c->r2;
    case 3: return c->r3;
    default: return (uint32_t)(mem ? *(uint32_t *)(mem + c->sp + (uint32_t)(i - 4) * 4u) : 0);
    }
}

static float farg(Cpu *c, uint8_t *mem, int i) {
    uint32_t b = arg(c, mem, i);
    float f;
    memcpy(&f, &b, 4);
    return f;
}

static void *gp(uint8_t *mem, uint32_t addr) {
    return addr ? (void *)(mem + addr) : NULL;
}

static uint32_t guest_str(uint8_t *mem, const char *s) {
    static uint32_t cursor = 0x9A5000u;
    size_t n = strlen(s) + 1;
    if (cursor + n >= 0x9B0000u) cursor = 0x9A5000u;
    memcpy(mem + cursor, s, n);
    uint32_t r = cursor;
    cursor += (uint32_t)n;
    return r;
}

static int texture_object_name(Cpu *c, uint8_t *mem, char *out, size_t cap) {
    if (!c->r5 || c->r5 + 12u >= guest_mem_size || cap < 2) return 0;
    uint32_t np = *(uint32_t *)(mem + c->r5 + 8u);
    if (!np || np >= guest_mem_size) return 0;
    for (size_t i = 0; i + 1 < cap; i++) {
        if (np + i >= guest_mem_size) return 0;
        out[i] = (char)mem[np + i];
        if (!out[i]) return i > 0;
    }
    out[cap - 1] = 0;
    return 0;
}

/* Assign attribute locations in declaration order. The shader source contains both branches of
 * some attribute #if blocks, so duplicates must not consume a second location. */
static void bind_declared_attributes(GLuint program, const char *src) {
    if (program >= MAX_GL_OBJECTS || !src) return;
    GLuint slot = 0;
    char seen[32][64];
    int nseen = 0;
    const char *p = src;
    while ((p = strstr(p, "attribute")) != NULL) {
        p += 9;
        const char *end = strchr(p, ';');
        if (!end) break;
        char name[64] = {0};
        const char *q = p;
        while (q < end) {
            while (q < end && !(isalnum((unsigned char)*q) || *q == '_')) q++;
            if (q >= end) break;
            const char *start = q;
            while (q < end && (isalnum((unsigned char)*q) || *q == '_')) q++;
            size_t n = (size_t)(q - start);
            if (n < sizeof name) {
                memcpy(name, start, n);
                name[n] = 0;
            }
        }
        int duplicate = 0;
        for (int i = 0; i < nseen; i++) {
            if (!strcmp(seen[i], name)) { duplicate = 1; break; }
        }
        if (name[0] && !duplicate && g_BindAttribLocation) {
            ((void (*)(GLuint, GLuint, const char *))g_BindAttribLocation)(program, slot, name);
            if (g_trace) fprintf(stderr, "gl: bindAttrib prog=%u loc=%u name=%s\n", program, slot, name);
            if (nseen < 32) {
                strncpy(seen[nseen], name, sizeof seen[nseen] - 1);
                seen[nseen][sizeof seen[nseen] - 1] = 0;
                nseen++;
            }
            slot++;
        }
        p = end;
    }
}

int gl_bridge_init(void) {
#define LOAD(n) g_##n = platform_gl_proc("gl" #n);
    GL_FUNCS(LOAD)
#undef LOAD
    {
        const char *e = getenv("FAST5_FAKE_GPU");
        g_fake_gpu = e && *e && strcmp(e, "0");
        if (g_fake_gpu) fprintf(stderr, "gl: compatibility GPU identity enabled\n");
        e = getenv("FAST5_NO_CULL");
        g_no_cull = e && *e && strcmp(e, "0");
        if (g_no_cull) fprintf(stderr, "gl: culling disabled for diagnosis\n");
        e = getenv("FAST5_GL_TRACE");
        g_trace = e && *e && strcmp(e, "0");
        e = getenv("FAST5_TEX_DUMP");
        g_tex_dump = e && *e && strcmp(e, "0");
        e = getenv("FAST5_TEX_AUDIT");
        g_tex_audit = e && *e && strcmp(e, "0");
        if (!g_trace) {
            g_state_logs = g_draw_logs = g_texture_logs = g_matrix_logs = 10000;
            g_attr_call_logs = g_buffer_logs = g_vertex_logs = g_attrloc_logs = 10000;
            g_fbo_logs = g_error_logs = g_shader_logs = g_compressed_logs = 10000;
            g_tex_image_logs = 10000;
        }
    }
    return g_Clear && g_ClearColor && g_CreateShader && g_ShaderSource && g_CompileShader &&
           g_CreateProgram && g_LinkProgram && g_UseProgram;
}

static void write_rgb_bmp(const char *path, int w, int h, int channels,
                          const unsigned char *data) {
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
    uint32_t raw = (uint32_t)row * (uint32_t)h; memcpy(hdr + 34, &raw, 4);
    FILE *f = fopen(path, "wb");
    if (!f) return;
    fwrite(hdr, 1, sizeof hdr, f);
    unsigned char *line = (unsigned char *)calloc(1, (size_t)row);
    for (int y = h - 1; y >= 0; y--) {
        memset(line, 0, (size_t)row);
        for (int x = 0; x < w; x++) {
            const unsigned char *p = data + ((size_t)y * (size_t)w + (size_t)x) * (size_t)channels;
            line[x * 3 + 0] = p[2];
            line[x * 3 + 1] = p[1];
            line[x * 3 + 2] = p[0];
        }
        fwrite(line, 1, (size_t)row, f);
    }
    free(line);
    fclose(f);
}

/* Dump the colour attachment of `fbo` at capture frames. Called when the engine switches away from
 * a render target, i.e. after the scene for that frame has been drawn into it. */
static void rtt_dump_fbo(GLuint fbo) {
    if (g_rtt_dump < 0) {
        const char *e = getenv("FAST5_RTT_DUMP");
        g_rtt_dump = e && *e && strcmp(e, "0") != 0;
    }
    if (!g_rtt_dump || fbo == 0 || fbo >= 256) return;
    if (!g_gl_frame || (g_gl_frame % 300u) != 0u) return;
    if (g_fbo_dumped[fbo] == (unsigned char)(g_gl_frame & 0xffu)) return;
    GLuint texid = g_fbo_color_tex[fbo];
    if (!texid || texid >= TEX_MAP_MAX) return;
    int w = g_tex_w[texid], h = g_tex_h[texid];
    if (w <= 0 || h <= 0 || w > 4096 || h > 4096) return;
    if (!g_ReadPixels || !g_BindFramebuffer) return;
    unsigned char *px = (unsigned char *)malloc((size_t)w * (size_t)h * 4u);
    if (!px) return;
    ((void (*)(GLenum, GLuint))g_BindFramebuffer)(0x8D40u, fbo);
    ((void (*)(GLint, GLint, GLsizei, GLsizei, GLenum, GLenum, void *))g_ReadPixels)
        (0, 0, (GLsizei)w, (GLsizei)h, GL_RGBA, GL_UNSIGNED_BYTE, px);
    g_fbo_dumped[fbo] = (unsigned char)(g_gl_frame & 0xffu);
    char path[160];
    snprintf(path, sizeof path, "build/fbo%u_tex%u_f%u_%dx%d.bmp", fbo, texid, g_gl_frame, w, h);
    write_rgb_bmp(path, w, h, 4, px);
    free(px);
    fprintf(stderr, "fbodump: fbo=%u tex=%u '%s' %dx%d -> %s\n",
            fbo, texid, g_tex_name[texid][0] ? g_tex_name[texid] : "?", w, h, path);
}

/* PVRTC is not provided by ANGLE/D3D. Until the real decoder is wired, upload a deterministic
 * placeholder RGBA image so geometry and layout remain visible. */
static void upload_compressed(uint8_t *mem, uint32_t target, uint32_t level, uint32_t format,
                              uint32_t w, uint32_t h, uint32_t size, uint32_t data) {
    if (!w || !h || w > 4096 || h > 4096) return;
    size_t n = (size_t)w * (size_t)h * 4u;
    unsigned char *rgba = (unsigned char *)malloc(n);
    if (!rgba) return;
    const unsigned char *src = (const unsigned char *)gp(mem, data);
    unsigned seed = (format << 24) ^ (level * 2654435761u) ^ (src ? src[0] : 0);
    for (uint32_t y = 0; y < h; y++) {
        for (uint32_t x = 0; x < w; x++) {
            unsigned char *p = rgba + ((size_t)y * w + x) * 4u;
            p[0] = (unsigned char)(((x * 7u + seed) & 0xFF) | 0x30);
            p[1] = (unsigned char)(((y * 11u + seed * 3u) & 0xFF) | 0x30);
            p[2] = (unsigned char)(((x * 13u + y * 5u + seed) & 0xFF) | 0x30);
            p[3] = 255;
        }
    }
    ((void (*)(GLenum, GLint, GLint, GLsizei, GLsizei, GLint, GLenum, GLenum, const void *))g_TexImage2D)
        (target, (GLint)level, GL_RGBA, (GLsizei)w, (GLsizei)h, 0, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
    if (g_compressed_logs < 8) {
        fprintf(stderr, "gl: PVRTC placeholder fmt=0x%x %ux%u size=%u\n", format, w, h, size);
        g_compressed_logs++;
    }
    free(rgba);
}

/* The shipped shader templates contain a stray ']' on some preprocessor lines
 * (for example "#endif]"), which GLES2 compilers reject but the original PowerVR compiler
 * tolerated. Strip ']' only on lines whose first non-space character is '#'. */
static char *combine_and_clean_shader(const char *const *src, int count, const GLint *lp,
                                      size_t *out_len, size_t *removed) {
    size_t total = 0;
    for (int i = 0; i < count; i++) {
        int ll = lp ? lp[i] : -1;
        total += (ll >= 0) ? (size_t)ll : strlen(src[i]);
    }
    char *raw = (char *)malloc(total + 1);
    char *out = (char *)malloc(total + 1);
    if (!raw || !out) { free(raw); free(out); return NULL; }
    size_t n = 0;
    for (int i = 0; i < count; i++) {
        int ll = lp ? lp[i] : -1;
        size_t len = (ll >= 0) ? (size_t)ll : strlen(src[i]);
        memcpy(raw + n, src[i], len);
        n += len;
    }
    raw[n] = 0;

    size_t o = 0, cut = 0;
    int line_start = 1, directive = 0, saw_nonspace = 0;
    for (size_t i = 0; i < n; i++) {
        char ch = raw[i];
        if (line_start) {
            if (ch == ' ' || ch == '\t') {
                /* still at the start of the line */
            } else {
                line_start = 0;
                saw_nonspace = 1;
                directive = (ch == '#');
            }
        }
        if (directive && ch == ']') {
            cut++;
            continue;
        }
        out[o++] = ch;
        if (ch == '\n') {
            line_start = 1;
            directive = 0;
            saw_nonspace = 0;
        }
    }
    out[o] = 0;
    free(raw);
    *out_len = o;
    *removed = cut;
    return out;
}

static void log_draw_attributes(uint8_t *mem) {
    static unsigned char seen[1024];
    static int early;
    if (!g_trace) return;
    int show = early < 6;
    if (!show && g_current_program < 1024) {
        if (!seen[g_current_program]) {
            seen[g_current_program] = 1;
            show = 1;
        }
    }
    if (!show) return;
    early++;
    fprintf(stderr, "gl: attrs prog=%u fbo=%u vbo=%u ebo=%u\n",
            g_current_program, g_bound_fbo, g_array_buffer, g_element_buffer);
    for (int i = 0; i < 4; i++) {
        fprintf(stderr, "gl:   attr%d en=%d size=%d stride=%d ptr=0x%x",
                i, g_attr_enabled[i], g_attr_size[i], g_attr_stride[i], g_attr_ptr[i]);
        if (g_attr_enabled[i] && g_array_buffer == 0 && g_attr_ptr[i] &&
            g_attr_ptr[i] + 12u < guest_mem_size) {
            const float *fp = (const float *)(mem + g_attr_ptr[i]);
            fprintf(stderr, " first=%.3f %.3f %.3f", fp[0], fp[1], fp[2]);
        }
        fprintf(stderr, "\n");
    }
}

int gl_bridge_call(Cpu *c, uint8_t *mem, const char *n) {
#define A(i) arg(c, mem, i)
#define F(i) farg(c, mem, i)
#define P(i) gp(mem, A(i))
    g_last_gl_call = n;
    if (!strcmp(n, "glClearColor")) {
        if (g_state_logs < 20) {
            fprintf(stderr, "gl: clearColor %.3f %.3f %.3f %.3f\n", F(0), F(1), F(2), F(3));
            g_state_logs++;
        }
        ((void (*)(GLfloat, GLfloat, GLfloat, GLfloat))g_ClearColor)(F(0), F(1), F(2), F(3));
        return 1;
    }
    if (!strcmp(n, "glClear")) {
        if (g_state_logs < 20) {
            fprintf(stderr, "gl: clear mask=0x%x\n", A(0));
            g_state_logs++;
        }
        ((void (*)(GLbitfield))g_Clear)((GLbitfield)A(0));
        return 1;
    }
    if (!strcmp(n, "glViewport")) {
        GLint vx = (GLint)A(0), vy = (GLint)A(1);
        GLsizei vw = (GLsizei)A(2), vh = (GLsizei)A(3);
        /*
         * The guest renders the screen at the design resolution, but the window can be any
         * size. When the engine asks for the whole screen area on the default framebuffer,
         * the host presents it in the whole framebuffer instead; render targets keep their
         * own size so off-screen passes stay inside their attachment.
         */
        if (!g_resolution.engine_native && g_bound_fbo == 0 &&
            g_resolution.frame_w != g_resolution.design_w &&
            vx == 0 && vy == 0 && vw == g_resolution.design_w && vh == g_resolution.design_h) {
            vx = 0;
            vy = 0;
            vw = g_resolution.frame_w;
            vh = g_resolution.frame_h;
        }
        if (g_state_logs < 20) {
            fprintf(stderr, "gl: viewport %d %d %d %d\n", (int)vx, (int)vy, (int)vw, (int)vh);
            g_state_logs++;
        }
        ((void (*)(GLint, GLint, GLsizei, GLsizei))g_Viewport)(vx, vy, vw, vh);
        return 1;
    }
    if (!strcmp(n, "glScissor")) {
        GLint sx = (GLint)A(0), sy = (GLint)A(1);
        GLsizei sw = (GLsizei)A(2), sh = (GLsizei)A(3);
        /* In design mode the scissor rect is in design space, like everything else the 2D layer
         * draws; in native mode the engine already works in window pixels. */
        if (!g_resolution.engine_native && g_resolution.frame_w != g_resolution.design_w) {
            double x0, y0, x1, y1;
            fast5_resolution_to_design(sx, sy, &x0, &y0);
            fast5_resolution_to_design(sx + sw, sy + sh, &x1, &y1);
            sx = (GLint)(x0 < 0 ? 0 : x0);
            sy = (GLint)(y0 < 0 ? 0 : y0);
            sw = (GLsizei)(x1 - x0);
            sh = (GLsizei)(y1 - y0);
        }
        g_scissor_rect[0] = (int)sx; g_scissor_rect[1] = (int)sy;
        g_scissor_rect[2] = (int)sw; g_scissor_rect[3] = (int)sh;
        if (g_state_logs < 120) {
            fprintf(stderr, "gl: scissor %d %d %d %d\n", (int)sx, (int)sy, (int)sw, (int)sh);
            g_state_logs++;
        }
        ((void (*)(GLint, GLint, GLsizei, GLsizei))g_Scissor)(sx, sy, sw, sh);
        return 1;
    }
    if (!strcmp(n, "glEnable")) {
        if (A(0) == 0x0C11u) g_scissor_test = 1;
        if (A(0) == 0x0B71u) g_depth_test = 1;
        if (A(0) == 0x0B44u) {
            g_cull_face = 1;
            if (g_no_cull) { fprintf(stderr, "gl: cull enable suppressed\n"); return 1; }
        }
        if (A(0) == 0x0BE2u) g_blend = 1;
        if (g_state_logs < 120) { fprintf(stderr, "gl: enable 0x%x\n", A(0)); g_state_logs++; }
        ((void (*)(GLenum))g_Enable)((GLenum)A(0));
        return 1;
    }
    if (!strcmp(n, "glDisable")) {
        if (A(0) == 0x0C11u) g_scissor_test = 0;
        if (A(0) == 0x0B71u) g_depth_test = 0;
        if (A(0) == 0x0B44u) g_cull_face = 0;
        if (A(0) == 0x0BE2u) g_blend = 0;
        if (g_state_logs < 120) { fprintf(stderr, "gl: disable 0x%x\n", A(0)); g_state_logs++; }
        ((void (*)(GLenum))g_Disable)((GLenum)A(0));
        return 1;
    }
    if (!strcmp(n, "glCullFace")) {
        if (g_state_logs < 120) { fprintf(stderr, "gl: cullFace 0x%x\n", A(0)); g_state_logs++; }
        ((void (*)(GLenum))g_CullFace)((GLenum)A(0));
        return 1;
    }
    if (!strcmp(n, "glFrontFace")) {
        if (g_state_logs < 120) { fprintf(stderr, "gl: frontFace 0x%x\n", A(0)); g_state_logs++; }
        ((void (*)(GLenum))g_FrontFace)((GLenum)A(0));
        return 1;
    }
    if (!strcmp(n, "glBlendFunc")) { ((void (*)(GLenum, GLenum))g_BlendFunc)((GLenum)A(0), (GLenum)A(1)); return 1; }
    if (!strcmp(n, "glBlendEquation")) { ((void (*)(GLenum))g_BlendEquation)((GLenum)A(0)); return 1; }
    if (!strcmp(n, "glBlendColor")) { ((void (*)(GLfloat, GLfloat, GLfloat, GLfloat))g_BlendColor)(F(0), F(1), F(2), F(3)); return 1; }
    if (!strcmp(n, "glDepthFunc")) { ((void (*)(GLenum))g_DepthFunc)((GLenum)A(0)); return 1; }
    if (!strcmp(n, "glDepthMask")) { ((void (*)(GLboolean))g_DepthMask)((GLboolean)A(0)); return 1; }
    if (!strcmp(n, "glDepthRangef")) { ((void (*)(GLfloat, GLfloat))g_DepthRangef)(F(0), F(1)); return 1; }
    if (!strcmp(n, "glClearDepthf")) { ((void (*)(GLfloat))g_ClearDepthf)(F(0)); return 1; }
    if (!strcmp(n, "glClearStencil")) { ((void (*)(GLint))g_ClearStencil)((GLint)A(0)); return 1; }
    if (!strcmp(n, "glColorMask")) { ((void (*)(GLboolean, GLboolean, GLboolean, GLboolean))g_ColorMask)((GLboolean)A(0), (GLboolean)A(1), (GLboolean)A(2), (GLboolean)A(3)); return 1; }
    if (!strcmp(n, "glCullFace")) { ((void (*)(GLenum))g_CullFace)((GLenum)A(0)); return 1; }
    if (!strcmp(n, "glFrontFace")) { ((void (*)(GLenum))g_FrontFace)((GLenum)A(0)); return 1; }
    if (!strcmp(n, "glLineWidth")) { ((void (*)(GLfloat))g_LineWidth)(F(0)); return 1; }
    if (!strcmp(n, "glPolygonOffset")) { ((void (*)(GLfloat, GLfloat))g_PolygonOffset)(F(0), F(1)); return 1; }
    if (!strcmp(n, "glPixelStorei")) { ((void (*)(GLenum, GLint))g_PixelStorei)((GLenum)A(0), (GLint)A(1)); return 1; }
    if (!strcmp(n, "glSampleCoverage")) { ((void (*)(GLfloat, GLboolean))g_SampleCoverage)(F(0), (GLboolean)A(1)); return 1; }
    if (!strcmp(n, "glStencilFunc")) { ((void (*)(GLenum, GLint, GLuint))g_StencilFunc)((GLenum)A(0), (GLint)A(1), (GLuint)A(2)); return 1; }
    if (!strcmp(n, "glStencilOp")) { ((void (*)(GLenum, GLenum, GLenum))g_StencilOp)((GLenum)A(0), (GLenum)A(1), (GLenum)A(2)); return 1; }
    if (!strcmp(n, "glStencilMask")) { ((void (*)(GLuint))g_StencilMask)((GLuint)A(0)); return 1; }
    if (!strcmp(n, "glFlush")) { ((void (*)(void))g_Flush)(); return 1; }
    if (!strcmp(n, "glGetError")) {
        GLenum e = ((GLenum (*)(void))g_GetError)();
        if (e && g_error_logs < 20) {
            fprintf(stderr, "gl: error 0x%x after %s\n", e, g_last_gl_call ? g_last_gl_call : "?");
            g_error_logs++;
        }
        c->r0 = e;
        return 1;
    }

    if (!strcmp(n, "glGetString")) {
        if (g_fake_gpu) {
            const char *fake = NULL;
            switch (A(0)) {
            case 0x1F00: fake = "Imagination Technologies"; break;
            case 0x1F01: fake = "PowerVR SGX 540"; break;
            case 0x1F02: fake = "OpenGL ES 2.0"; break;
            case 0x1F03: {
                const char *real = (const char *)((const unsigned char *(*)(GLenum))g_GetString)(0x1F03u);
                char ext[8192];
                snprintf(ext, sizeof ext, "%s", real ? real : "");
                c->r0 = guest_str(mem, ext);
                return 1;
            }
            default: break;
            }
            if (fake) { c->r0 = guest_str(mem, fake); return 1; }
        }
        const unsigned char *s = ((const unsigned char *(*)(GLenum))g_GetString)((GLenum)A(0));
        if (!s) { c->r0 = 0; return 1; }
        char buf[8192];
        snprintf(buf, sizeof buf, "%s", (const char *)s);
        c->r0 = guest_str(mem, buf);
        return 1;
    }
    if (!strcmp(n, "glGetIntegerv")) {
        ((void (*)(GLenum, GLint *))g_GetIntegerv)((GLenum)A(0), (GLint *)P(1));
        return 1;
    }
    if (!strcmp(n, "glGetFloatv")) { ((void (*)(GLenum, GLfloat *))g_GetFloatv)((GLenum)A(0), (GLfloat *)P(1)); return 1; }
    if (!strcmp(n, "glReadPixels")) { ((void (*)(GLint, GLint, GLsizei, GLsizei, GLenum, GLenum, void *))g_ReadPixels)((GLint)A(0), (GLint)A(1), (GLsizei)A(2), (GLsizei)A(3), (GLenum)A(4), (GLenum)A(5), P(6)); return 1; }

    if (!strcmp(n, "glGenTextures")) { ((void (*)(GLsizei, GLuint *))g_GenTextures)((GLsizei)A(0), (GLuint *)P(1)); return 1; }
    if (!strcmp(n, "glDeleteTextures")) {
        GLsizei cnt = (GLsizei)A(0);
        const GLuint *ids = (const GLuint *)P(1);
        if (ids && cnt > 0) {
            for (GLsizei i = 0; i < cnt; i++) {
                if (g_tex_audit && ids[i] && ids[i] < TEX_MAP_MAX &&
                    (g_tex_ptr_map[ids[i]] || g_tex_name[ids[i]][0])) {
                    static int del_logs;
                    if (del_logs < 200) {
                        fprintf(stderr, "texAudit: delete id=%u name='%s' ptr=0x%08x\n",
                                ids[i], g_tex_name[ids[i]], g_tex_ptr_map[ids[i]]);
                        del_logs++;
                    }
                }
                tex_map_forget(ids[i]);
            }
        }
        ((void (*)(GLsizei, const GLuint *))g_DeleteTextures)(cnt, ids);
        return 1;
    }
    if (!strcmp(n, "glBindTexture")) {
        GLenum bt = (GLenum)A(0);
        GLuint bid = (GLuint)A(1);
        char tname[128] = {0};
        int have_name = texture_object_name(c, mem, tname, sizeof tname);
        if (bt == 0x0DE1u) {
            if (bid + 1u > g_tex_id_peak) g_tex_id_peak = bid + 1u;
            if (bid < TEX_MAP_MAX && have_name && tname[0]) {
                snprintf(g_tex_name[bid], sizeof g_tex_name[bid], "%s", tname);
            }
            if (g_pending_tex) {
                if (bid < TEX_MAP_MAX) {
                    if (g_tex_audit) {
                        /* Two ways the map rots: the id already described a different texture
                           object, or this texture object is already recorded under another id. */
                        if (g_tex_ptr_map[bid] && g_tex_ptr_map[bid] != g_pending_tex) {
                            static int rb_logs;
                            if (rb_logs < 200) {
                                fprintf(stderr, "texAudit: REBIND id=%u was=0x%08x now=0x%08x\n",
                                        bid, g_tex_ptr_map[bid], g_pending_tex);
                                rb_logs++;
                            }
                        }
                        for (uint32_t i = 1; i < g_tex_map_high && i < TEX_MAP_MAX; i++) {
                            if (i != bid && g_tex_ptr_map[i] == g_pending_tex) {
                                static int al_logs;
                                if (al_logs < 200) {
                                    fprintf(stderr, "texAudit: ALIAS ptr=0x%08x ids %u and %u\n",
                                            g_pending_tex, i, bid);
                                    al_logs++;
                                }
                                break;
                            }
                        }
                    }
                    g_tex_ptr_map[bid] = g_pending_tex;
                    if (bid + 1u > g_tex_map_high) {
                        g_tex_map_high = bid + 1u;
                        /* One-time proof that the old 256-entry table was too small. */
                        static int crossed;
                        if (!crossed && g_tex_map_high > 257u) {
                            crossed = 1;
                            fprintf(stderr, "texmap: gl id space passed 256 (now %u)\n",
                                    g_tex_map_high);
                        }
                    }
                } else {
                    static int over;
                    if (!over) {
                        over = 1;
                        fprintf(stderr, "texmap: gl id %u exceeds table size %u\n",
                                bid, TEX_MAP_MAX);
                    }
                }
            }
            g_pending_tex = 0;
            if (bid < sizeof g_texmap_seen && !g_texmap_seen[bid] && g_texmap_logs < 64) {
                g_texmap_seen[bid] = 1;
                g_texmap_logs++;
                char mname[160] = {0};
                if (g_tex_ptr_map[bid] && g_tex_ptr_map[bid] + 12u < guest_mem_size) {
                    uint32_t np = *(uint32_t *)(mem + g_tex_ptr_map[bid] + 8u);
                    if (np && np < guest_mem_size) {
                        for (size_t i = 0; i + 1 < sizeof mname; i++) {
                            if (np + i >= guest_mem_size) break;
                            mname[i] = (char)mem[np + i];
                            if (!mname[i]) break;
                        }
                    }
                }
                fprintf(stderr, "texmap: id=%u ptr=0x%08x name=%s\n", bid, g_tex_ptr_map[bid], mname);
            }
            unsigned slot = g_active_texture >= 0x84C0u ? g_active_texture - 0x84C0u : 0;
            if (slot < 16) g_bound_texture[slot] = bid;
        }
        if (g_texture_logs < 30) { fprintf(stderr, "gl: bindTexture target=0x%x id=%u\n", bt, bid); g_texture_logs++; }
        ((void (*)(GLenum, GLuint))g_BindTexture)(bt, bid);
        return 1;
    }
    if (!strcmp(n, "glActiveTexture")) {
        g_active_texture = (GLenum)A(0);
        if (g_texture_logs < 30) { fprintf(stderr, "gl: activeTexture 0x%x\n", A(0)); g_texture_logs++; }
        ((void (*)(GLenum))g_ActiveTexture)((GLenum)A(0));
        return 1;
    }
    if (!strcmp(n, "glTexParameteri")) { ((void (*)(GLenum, GLenum, GLint))g_TexParameteri)((GLenum)A(0), (GLenum)A(1), (GLint)A(2)); return 1; }
    if (!strcmp(n, "glTexParameterf")) { ((void (*)(GLenum, GLenum, GLfloat))g_TexParameterf)((GLenum)A(0), (GLenum)A(1), F(2)); return 1; }
    if (!strcmp(n, "glGenerateMipmap")) { ((void (*)(GLenum))g_GenerateMipmap)((GLenum)A(0)); return 1; }
    if (!strcmp(n, "glTexImage2D")) {
        GLenum target = (GLenum)A(0);
        GLint level = (GLint)A(1);
        GLint internal = (GLint)A(2);
        GLsizei width = (GLsizei)A(3);
        GLsizei height = (GLsizei)A(4);
        GLint border = (GLint)A(5);
        GLenum format = (GLenum)A(6);
        GLenum type = (GLenum)A(7);
        uint32_t src = A(8);
        const void *data = P(8);
        void *converted = NULL;
        unsigned slot = g_active_texture >= 0x84C0u ? g_active_texture - 0x84C0u : 0;
        GLuint tex_id = slot < 16 ? g_bound_texture[slot] : 0;
        if (tex_id && tex_id < TEX_MAP_MAX && width > 0 && height > 0) {
            g_tex_w[tex_id] = (int)width;
            g_tex_h[tex_id] = (int)height;
        }
        if ((format == GL_BGRA_EXT || format == GL_BGR_EXT) && type == GL_UNSIGNED_BYTE &&
            width > 0 && height > 0 && width <= 4096 && height <= 4096) {
            size_t bytes = (size_t)width * (size_t)height * (format == GL_BGRA_EXT ? 4u : 3u);
            if (src && src + bytes <= guest_mem_size) {
                unsigned char *dst = (unsigned char *)malloc(bytes);
                if (dst) {
                    const unsigned char *s = (const unsigned char *)data;
                    for (size_t i = 0; i + 2 < bytes; i += (format == GL_BGRA_EXT ? 4u : 3u)) {
                        dst[i] = s[i + 2];
                        dst[i + 1] = s[i + 1];
                        dst[i + 2] = s[i];
                        if (format == GL_BGRA_EXT) dst[i + 3] = s[i + 3];
                    }
                    converted = dst;
                    data = dst;
                    internal = format == GL_BGRA_EXT ? (GLint)GL_RGBA : (GLint)GL_RGB;
                    format = (GLenum)internal;
                }
            }
        }
        if (g_tex_image_logs < 240) {
            unsigned slot = g_active_texture >= 0x84C0u ? g_active_texture - 0x84C0u : 0;
            GLuint id = slot < 16 ? g_bound_texture[slot] : 0;
            fprintf(stderr, "gl: texImage2D id=%u level=%d internal=0x%x %dx%d format=0x%x type=0x%x data=0x%x%s\n",
                    id, (int)level, (unsigned)internal, (int)width, (int)height, (unsigned)format,
                    (unsigned)type, src, converted ? " converted-bgra" : "");
            g_tex_image_logs++;
        }
        {
            unsigned slot = g_active_texture >= 0x84C0u ? g_active_texture - 0x84C0u : 0;
            GLuint id = slot < 16 ? g_bound_texture[slot] : 0;
            if (id < 256u && !g_tex_upload_logged[id] && g_tex_ptr_map[id]) {
                g_tex_upload_logged[id] = 1;
                char uname[160] = {0};
                uint32_t tp = g_tex_ptr_map[id];
                if (tp + 12u < guest_mem_size) {
                    uint32_t np = *(uint32_t *)(mem + tp + 8u);
                    if (np && np < guest_mem_size) {
                        for (size_t i = 0; i + 1 < sizeof uname; i++) {
                            if (np + i >= guest_mem_size) break;
                            uname[i] = (char)mem[np + i];
                            if (!uname[i]) break;
                        }
                    }
                }
                if (g_tex_dump) fprintf(stderr, "texupload: id=%u ptr=0x%08x name=%s\n", id, tp, uname);
                if (g_tex_dump && (strstr(uname, "indow") || strstr(uname, "INDOW") || strstr(uname, "ightmap") || strstr(uname, "IGHTMAP") || strstr(uname, "Glow") || strstr(uname, "GLOW") || strstr(uname, "eflect") || strstr(uname, "EFLECT") || strstr(uname, "_Body") || strstr(uname, "_BODY") || strstr(uname, "_Mask") || strstr(uname, "_MASK") || strstr(uname, "Car_Decals")) &&
                    data && width > 0 && height > 0 && width <= 2048 && height <= 2048 &&
                    type == GL_UNSIGNED_BYTE && (format == GL_RGB || format == GL_RGBA)) {
                    char path[160];
                    char safe[160];
                    snprintf(safe, sizeof safe, "%s", uname);
                    /* Sanitise only the texture name: doing it after the "build/" prefix was
                       joined turned the separator into '_' and wrote the dump to the repo root. */
                    for (char *q = safe; *q; q++) {
                        if (*q == '/' || *q == '\\' || *q == ':') *q = '_';
                    }
                    snprintf(path, sizeof path, "build/tex_%s.bmp", safe);
                    write_rgb_bmp(path, (int)width, (int)height, format == GL_RGBA ? 4 : 3,
                                  (const unsigned char *)data);
                    fprintf(stderr, "gl: dumped window tex %s %dx%d fmt=0x%x\n", path, (int)width, (int)height, (unsigned)format);
                    if (format == GL_RGBA) {
                        size_t npx = (size_t)width * (size_t)height;
                        unsigned char *a = (unsigned char *)malloc(npx * 3u);
                        if (a) {
                            const unsigned char *s = (const unsigned char *)data;
                            for (size_t k = 0; k < npx; k++) {
                                a[k * 3u] = s[k * 4u + 3u];
                                a[k * 3u + 1u] = s[k * 4u + 3u];
                                a[k * 3u + 2u] = s[k * 4u + 3u];
                            }
                            snprintf(path, sizeof path, "build/alpha_%s.bmp", safe);
                            write_rgb_bmp(path, (int)width, (int)height, 3, a);
                            fprintf(stderr, "gl: dumped window alpha %s\n", path);
                            free(a);
                        }
                    }
                }
            }
        }
        if (g_tex_dump && data && width > 0 && height > 0 && width <= 2048 && height <= 2048 &&
            type == GL_UNSIGNED_BYTE &&
            (format == GL_RGB || format == GL_RGBA)) {
            unsigned slot = g_active_texture >= 0x84C0u ? g_active_texture - 0x84C0u : 0;
            GLuint id = slot < 16 ? g_bound_texture[slot] : 0;
            if (id == 8u || id == 10u) {
                char path[64];
                snprintf(path, sizeof path, "build/tex_id_%u_%dx%d.bmp", id, (int)width, (int)height);
                write_rgb_bmp(path, (int)width, (int)height, format == GL_RGBA ? 4 : 3,
                              (const unsigned char *)data);
                fprintf(stderr, "gl: dumped %s\n", path);
            } else if (g_tex_dumps < 3) {
                char path[64];
                snprintf(path, sizeof path, "build/tex_%d_%dx%d.bmp", g_tex_dumps, (int)width, (int)height);
                write_rgb_bmp(path, (int)width, (int)height, format == GL_RGBA ? 4 : 3,
                              (const unsigned char *)data);
                fprintf(stderr, "gl: dumped %s\n", path);
                g_tex_dumps++;
            }
        }
        ((void (*)(GLenum, GLint, GLint, GLsizei, GLsizei, GLint, GLenum, GLenum, const void *))g_TexImage2D)
            (target, level, internal, width, height, border, format, type, data);
        free(converted);
        return 1;
    }
    if (!strcmp(n, "glTexSubImage2D")) {
        GLenum target = (GLenum)A(0);
        GLint level = (GLint)A(1);
        GLint xoff = (GLint)A(2);
        GLint yoff = (GLint)A(3);
        GLsizei width = (GLsizei)A(4);
        GLsizei height = (GLsizei)A(5);
        GLenum format = (GLenum)A(6);
        GLenum type = (GLenum)A(7);
        uint32_t src = A(8);
        const void *data = P(8);
        void *converted = NULL;
        if ((format == GL_BGRA_EXT || format == GL_BGR_EXT) && type == GL_UNSIGNED_BYTE &&
            width > 0 && height > 0 && width <= 4096 && height <= 4096) {
            size_t bytes = (size_t)width * (size_t)height * (format == GL_BGRA_EXT ? 4u : 3u);
            if (src && src + bytes <= guest_mem_size) {
                unsigned char *dst = (unsigned char *)malloc(bytes);
                if (dst) {
                    const unsigned char *s = (const unsigned char *)data;
                    for (size_t i = 0; i + 2 < bytes; i += (format == GL_BGRA_EXT ? 4u : 3u)) {
                        dst[i] = s[i + 2];
                        dst[i + 1] = s[i + 1];
                        dst[i + 2] = s[i];
                        if (format == GL_BGRA_EXT) dst[i + 3] = s[i + 3];
                    }
                    converted = dst;
                    data = dst;
                    format = format == GL_BGRA_EXT ? (GLenum)GL_RGBA : (GLenum)GL_RGB;
                }
            }
        }
        ((void (*)(GLenum, GLint, GLint, GLint, GLsizei, GLsizei, GLenum, GLenum, const void *))g_TexSubImage2D)
            (target, level, xoff, yoff, width, height, format, type, data);
        free(converted);
        return 1;
    }
    if (!strcmp(n, "glCompressedTexImage2D")) {
        uint32_t fmt = A(2), w = A(3), h = A(4);
        uint32_t size = A(6), data = A(7);
        if (fmt == 0x8C00u || fmt == 0x8C01u || fmt == 0x8C02u || fmt == 0x8C03u) {
            upload_compressed(mem, A(0), A(1), fmt, w, h, size, data);
        } else {
            ((void (*)(GLenum, GLint, GLenum, GLsizei, GLsizei, GLint, GLsizei, const void *))g_CompressedTexImage2D)
                ((GLenum)A(0), (GLint)A(1), (GLenum)fmt, (GLsizei)w, (GLsizei)h, (GLint)A(5), (GLsizei)size, P(7));
        }
        return 1;
    }
    if (!strcmp(n, "glCompressedTexSubImage2D")) { return 1; }
    if (!strcmp(n, "glCopyTexSubImage2D")) {
        ((void (*)(GLenum, GLint, GLint, GLint, GLint, GLint, GLsizei, GLsizei))g_CopyTexSubImage2D)
            ((GLenum)A(0), (GLint)A(1), (GLint)A(2), (GLint)A(3), (GLint)A(4), (GLint)A(5), (GLsizei)A(6), (GLsizei)A(7));
        return 1;
    }

    if (!strcmp(n, "glGenBuffers")) { ((void (*)(GLsizei, GLuint *))g_GenBuffers)((GLsizei)A(0), (GLuint *)P(1)); return 1; }
    if (!strcmp(n, "glDeleteBuffers")) { ((void (*)(GLsizei, const GLuint *))g_DeleteBuffers)((GLsizei)A(0), (const GLuint *)P(1)); return 1; }
    if (!strcmp(n, "glBindBuffer")) {
        GLenum t = (GLenum)A(0); GLuint b = (GLuint)A(1);
        if (t == GL_ARRAY_BUFFER) g_array_buffer = b;
        else if (t == GL_ELEMENT_ARRAY_BUFFER) g_element_buffer = b;
        if (g_state_logs < 120) {
            fprintf(stderr, "gl: bindBuffer target=0x%x id=%u\n", t, b);
            g_state_logs++;
        }
        ((void (*)(GLenum, GLuint))g_BindBuffer)(t, b);
        return 1;
    }
    if (!strcmp(n, "glBufferData")) {
        if (g_buffer_logs < 12) {
            const float *fp = (const float *)(mem + A(2));
            fprintf(stderr, "gl: bufferData prog=%u target=0x%x size=%u data=0x%x usage=0x%x first=%.3f %.3f %.3f\n",
                    g_current_program, A(0), A(1), A(2), A(3),
                    A(2) ? fp[0] : 0.0f, A(2) ? fp[1] : 0.0f, A(2) ? fp[2] : 0.0f);
            g_buffer_logs++;
        }
        ((void (*)(GLenum, intptr_t, const void *, GLenum))g_BufferData)
            ((GLenum)A(0), (intptr_t)(uint32_t)A(1), P(2), (GLenum)A(3));
        return 1;
    }
    if (!strcmp(n, "glBufferSubData")) { ((void (*)(GLenum, intptr_t, intptr_t, const void *))g_BufferSubData)((GLenum)A(0), (intptr_t)(uint32_t)A(1), (intptr_t)(uint32_t)A(2), P(3)); return 1; }
    if (!strcmp(n, "glVertexAttribPointer")) {
        uint32_t ptr = A(5);
        void *p = g_array_buffer ? (void *)(uintptr_t)ptr : gp(mem, ptr);
        if (A(0) < 16) {
            g_attr_ptr[A(0)] = ptr;
            g_attr_stride[A(0)] = (int)A(4);
            g_attr_size[A(0)] = (int)A(1);
        }
        if (g_vertex_logs < 8 && A(0) == 0 && g_array_buffer == 0 && A(2) == 0x1406u) {
            const float *fp = (const float *)(mem + ptr);
            fprintf(stderr, "gl: verts0 %.3f %.3f %.3f  %.3f %.3f %.3f  %.3f %.3f %.3f\n",
                    fp[0], fp[1], fp[2], fp[3], fp[4], fp[5], fp[6], fp[7], fp[8]);
            g_vertex_logs++;
        }
        if (g_trace && g_attr_call_logs < 120) {
            fprintf(stderr, "gl: attrCall prog=%u idx=%u size=%d type=0x%x stride=%d vbo=%u ptr=0x%x\n",
                    g_current_program, A(0), (int)A(1), A(2), (int)A(4), g_array_buffer, ptr);
            g_attr_call_logs++;
        }
        if (g_state_logs < 120) {
            fprintf(stderr, "gl: attribPointer idx=%u size=%d type=0x%x stride=%d vbo=%u ptr=0x%x\n",
                    A(0), (int)A(1), A(2), (int)A(4), g_array_buffer, ptr);
            g_state_logs++;
        }
        ((void (*)(GLuint, GLint, GLenum, GLboolean, GLsizei, const void *))g_VertexAttribPointer)
            ((GLuint)A(0), (GLint)A(1), (GLenum)A(2), (GLboolean)A(3), (GLsizei)A(4), p);
        return 1;
    }
    if (!strcmp(n, "glEnableVertexAttribArray")) {
        if (A(0) < 16) g_attr_enabled[A(0)] = 1;
        if (g_trace && g_attr_state_logs < 80) {
            fprintf(stderr, "gl: enableAttrib %u\n", A(0));
            g_attr_state_logs++;
        }
        ((void (*)(GLuint))g_EnableVertexAttribArray)((GLuint)A(0));
        return 1;
    }
    if (!strcmp(n, "glDisableVertexAttribArray")) {
        if (A(0) < 16) g_attr_enabled[A(0)] = 0;
        if (g_trace && g_attr_state_logs < 80) {
            fprintf(stderr, "gl: disableAttrib %u\n", A(0));
            g_attr_state_logs++;
        }
        ((void (*)(GLuint))g_DisableVertexAttribArray)((GLuint)A(0));
        return 1;
    }
    if (!strcmp(n, "glVertexAttrib4f")) { ((void (*)(GLuint, GLfloat, GLfloat, GLfloat, GLfloat))g_VertexAttrib4f)((GLuint)A(0), F(1), F(2), F(3), F(4)); return 1; }
    if (!strcmp(n, "glDrawArrays")) {
        log_draw_attributes(mem);
        if (g_draw_logs < 20) {
            fprintf(stderr, "gl: drawArrays mode=0x%x first=%d count=%d fbo=%u prog=%u depth=%d cull=%d blend=%d scissor=%d [%d,%d,%d,%d] tex0=%u tex1=%u\n",
                    A(0), (int)A(1), (int)A(2), g_bound_fbo, g_current_program,
                    g_depth_test, g_cull_face, g_blend, g_scissor_test,
                    g_scissor_rect[0], g_scissor_rect[1], g_scissor_rect[2], g_scissor_rect[3],
                    g_bound_texture[0], g_bound_texture[1]);
            g_draw_logs++;
        }
        ((void (*)(GLenum, GLint, GLsizei))g_DrawArrays)((GLenum)A(0), (GLint)A(1), (GLsizei)A(2));
        {
            if (g_trace) {
                GLenum e = ((GLenum (*)(void))g_GetError)();
                if (e) fprintf(stderr, "gl: drawArrays error 0x%x\n", e);
            }
        }
        return 1;
    }
    if (!strcmp(n, "glDrawElements")) {
        if (g_skip_next_draw) {
            g_skip_next_draw = 0;
            return 1;
        }
        uint32_t idx = A(3);
        void *p = g_element_buffer ? (void *)(uintptr_t)idx : gp(mem, idx);
        log_draw_attributes(mem);
        if (g_draw_logs < 1000) {
            fprintf(stderr, "gl: drawElements mode=0x%x count=%d type=0x%x fbo=%u prog=%u vbo=%u ebo=%u depth=%d cull=%d blend=%d scissor=%d [%d,%d,%d,%d] tex0=%u tex1=%u\n",
                    A(0), A(1), A(2), g_bound_fbo, g_current_program, g_array_buffer, g_element_buffer,
                    g_depth_test, g_cull_face, g_blend, g_scissor_test,
                    g_scissor_rect[0], g_scissor_rect[1], g_scissor_rect[2], g_scissor_rect[3],
                    g_bound_texture[0], g_bound_texture[1]);
            if (g_array_buffer == 0 && g_element_buffer == 0 && g_attr_size[0] >= 2 &&
                A(2) == 0x1403u && A(1) >= 3) {
                const uint16_t *ind = (const uint16_t *)(mem + idx);
                for (int k = 0; k < 3; k++) {
                    uint32_t vi = ind[k];
                    const float *vp = (const float *)(mem + g_attr_ptr[0] + vi * (uint32_t)g_attr_stride[0]);
                    fprintf(stderr, "gl:   vert[%u] %.3f %.3f %.3f\n", vi, vp[0], vp[1], vp[2]);
                }
            }
            g_draw_logs++;
        }
        ((void (*)(GLenum, GLsizei, GLenum, const void *))g_DrawElements)((GLenum)A(0), (GLsizei)A(1), (GLenum)A(2), p);
        {
            if (g_trace) {
                GLenum e = ((GLenum (*)(void))g_GetError)();
                if (e) fprintf(stderr, "gl: drawElements error 0x%x\n", e);
            }
        }
        return 1;
    }

    if (!strcmp(n, "glCreateShader")) { c->r0 = ((GLuint (*)(GLenum))g_CreateShader)((GLenum)A(0)); return 1; }
    if (!strcmp(n, "glDeleteShader")) {
        if (A(0) < MAX_GL_OBJECTS) { free(g_shader_src[A(0)]); g_shader_src[A(0)] = NULL; }
        ((void (*)(GLuint))g_DeleteShader)((GLuint)A(0));
        return 1;
    }
    if (!strcmp(n, "glShaderSource")) {
        GLuint sh = (GLuint)A(0); GLsizei count = (GLsizei)A(1);
        uint32_t arr = A(2), lens = A(3);
        const char *src[64];
        if (count > 64) count = 64;
        for (int i = 0; i < count; i++) {
            uint32_t p = *(uint32_t *)(mem + arr + (uint32_t)i * 4u);
            src[i] = p ? (const char *)(mem + p) : "";
        }
        const GLint *lp = lens ? (const GLint *)(mem + lens) : NULL;
        size_t len = 0, removed = 0;
        char *clean = combine_and_clean_shader(src, count, lp, &len, &removed);
        if (!clean) return 1;
        const char *one = clean;
        ((void (*)(GLuint, GLsizei, const char *const *, const GLint *))g_ShaderSource)
            (sh, 1, &one, NULL);
        if (g_shader_logs < 200) {
            char path[80];
            snprintf(path, sizeof path, "build/shader_%u_%d.txt", sh, g_shader_seq++);
            FILE *sf = fopen(path, "wb");
            if (sf) { fwrite(clean, 1, len, sf); fclose(sf); }
            fprintf(stderr, "gl: source %s shader=%u parts=%d len=%zu removed=%zu\n",
                    path, sh, count, len, removed);
            g_shader_logs++;
        }
        if (sh < MAX_GL_OBJECTS) {
            free(g_shader_src[sh]);
            g_shader_src[sh] = (char *)malloc(len + 1);
            if (g_shader_src[sh]) memcpy(g_shader_src[sh], clean, len + 1);
        }
        free(clean);
        return 1;
    }
    if (!strcmp(n, "glCompileShader")) { ((void (*)(GLuint))g_CompileShader)((GLuint)A(0)); return 1; }
    if (!strcmp(n, "glGetShaderiv")) {
        ((void (*)(GLuint, GLenum, GLint *))g_GetShaderiv)((GLuint)A(0), (GLenum)A(1), (GLint *)P(2));
        if (A(1) == GL_COMPILE_STATUS && P(2) && *(GLint *)P(2) == 0) {
            fprintf(stderr, "gl: shader %u compile failed\n", A(0));
        }
        return 1;
    }
    if (!strcmp(n, "glGetShaderInfoLog")) {
        ((void (*)(GLuint, GLsizei, GLsizei *, char *))g_GetShaderInfoLog)((GLuint)A(0), (GLsizei)A(1), (GLsizei *)P(2), (char *)P(3));
        if (P(3) && ((char *)P(3))[0]) fprintf(stderr, "gl shader log: %s\n", (char *)P(3));
        return 1;
    }
    if (!strcmp(n, "glCreateProgram")) {
        GLuint prog = ((GLuint (*)(void))g_CreateProgram)();
        if (prog < MAX_GL_OBJECTS) g_program_exists[prog] = 1;
        if (g_trace) fprintf(stderr, "gl: createProgram -> %u\n", prog);
        c->r0 = prog;
        return 1;
    }
    if (!strcmp(n, "glDeleteProgram")) {
        if (A(0) < MAX_GL_OBJECTS) {
            g_program_exists[A(0)] = 0;
            g_prog_shader_count[A(0)] = 0;
        }
        if (g_trace) fprintf(stderr, "gl: deleteProgram %u\n", A(0));
        ((void (*)(GLuint))g_DeleteProgram)((GLuint)A(0));
        return 1;
    }
    if (!strcmp(n, "glAttachShader")) {
        GLuint prog = (GLuint)A(0), sh = (GLuint)A(1);
        if (prog < MAX_GL_OBJECTS && g_prog_shader_count[prog] < 4) {
            g_prog_shaders[prog][g_prog_shader_count[prog]++] = sh;
        }
        ((void (*)(GLuint, GLuint))g_AttachShader)(prog, sh);
        return 1;
    }
    if (!strcmp(n, "glLinkProgram")) {
        GLuint prog = (GLuint)A(0);
        if (prog < MAX_GL_OBJECTS) {
            for (int i = 0; i < g_prog_shader_count[prog]; i++) {
                GLuint sh = g_prog_shaders[prog][i];
                if (sh < MAX_GL_OBJECTS) bind_declared_attributes(prog, g_shader_src[sh]);
            }
        }
        ((void (*)(GLuint))g_LinkProgram)(prog);
        return 1;
    }
    if (!strcmp(n, "glUseProgram")) {
        g_current_program = (GLuint)A(0);
        if (g_state_logs < 120) { fprintf(stderr, "gl: useProgram %u\n", g_current_program); g_state_logs++; }
        ((void (*)(GLuint))g_UseProgram)((GLuint)A(0));
        return 1;
    }
    if (!strcmp(n, "glGetProgramiv")) {
        ((void (*)(GLuint, GLenum, GLint *))g_GetProgramiv)((GLuint)A(0), (GLenum)A(1), (GLint *)P(2));
        if (g_trace && P(2) && g_programiv_logs < 60 &&
            (A(1) == 0x8b82u || A(1) == 0x8b86u || A(1) == 0x8b87u ||
             A(1) == 0x8b89u || A(1) == 0x8b8au)) {
            fprintf(stderr, "gl: programiv prog=%u pname=0x%x -> %d\n",
                    A(0), A(1), *(GLint *)P(2));
            g_programiv_logs++;
        }
        if (A(1) == GL_LINK_STATUS && P(2) && *(GLint *)P(2) == 0) {
            fprintf(stderr, "gl: program %u link failed\n", A(0));
        }
        return 1;
    }
    if (!strcmp(n, "glGetProgramInfoLog")) {
        GLuint prog = (GLuint)A(0);
        if (prog >= MAX_GL_OBJECTS || !g_program_exists[prog]) {
            if (P(2)) *(GLsizei *)P(2) = 0;
            if (P(3)) ((char *)P(3))[0] = 0;
            if (g_trace) fprintf(stderr, "gl: ignored programInfoLog on non-program %u\n", prog);
            return 1;
        }
        GLenum pre = g_trace ? ((GLenum (*)(void))g_GetError)() : 0;
        ((void (*)(GLuint, GLsizei, GLsizei *, char *))g_GetProgramInfoLog)
            (prog, (GLsizei)A(1), (GLsizei *)P(2), (char *)P(3));
        GLenum post = g_trace ? ((GLenum (*)(void))g_GetError)() : 0;
        if (g_trace && (pre || post)) {
            fprintf(stderr, "gl: programInfoLog prog=%u exists=%d pre=0x%x post=0x%x last=%s\n",
                    prog, prog < MAX_GL_OBJECTS ? g_program_exists[prog] : -1,
                    pre, post, g_last_gl_call ? g_last_gl_call : "?");
        }
        if (P(3) && ((char *)P(3))[0]) fprintf(stderr, "gl program log: %s\n", (char *)P(3));
        return 1;
    }
    if (!strcmp(n, "glGetAttribLocation")) {
        GLint loc = ((GLint (*)(GLuint, const char *))g_GetAttribLocation)((GLuint)A(0), (const char *)P(1));
        if (g_attrloc_logs < 50) {
            fprintf(stderr, "gl: attribLocation prog=%u name=%s -> %d\n", A(0), (const char *)P(1), loc);
            g_attrloc_logs++;
        }
        c->r0 = (uint32_t)loc;
        return 1;
    }
    if (!strcmp(n, "glGetUniformLocation")) {
        GLint loc = ((GLint (*)(GLuint, const char *))g_GetUniformLocation)((GLuint)A(0), (const char *)P(1));
        if (g_state_logs < 120) {
            fprintf(stderr, "gl: uniformLocation prog=%u name=%s -> %d\n", A(0), (const char *)P(1), loc);
            g_state_logs++;
        }
        c->r0 = (uint32_t)loc;
        return 1;
    }
    if (!strcmp(n, "glGetActiveAttrib")) {
        GLuint prog = (GLuint)A(0), index = (GLuint)A(1);
        GLsizei buf = (GLsizei)A(2);
        GLsizei *len = (GLsizei *)P(3);
        GLint *size = (GLint *)P(4);
        GLenum *type = (GLenum *)P(5);
        char *name = (char *)P(6);
        ((void (*)(GLuint, GLuint, GLsizei, GLsizei *, GLint *, GLenum *, char *))g_GetActiveAttrib)
            (prog, index, buf, len, size, type, name);
        if (g_trace && g_active_attrib_logs < 40) {
            fprintf(stderr, "gl: activeAttrib prog=%u idx=%u buf=%d name=%s size=%d type=0x%x\n",
                    prog, index, (int)buf, name ? name : "(null)",
                    size ? (int)*size : -1, type ? (unsigned)*type : 0u);
            g_active_attrib_logs++;
        }
        return 1;
    }
    if (!strcmp(n, "glGetActiveUniform")) { ((void (*)(GLuint, GLuint, GLsizei, GLsizei *, GLint *, GLenum *, char *))g_GetActiveUniform)((GLuint)A(0), (GLuint)A(1), (GLsizei)A(2), (GLsizei *)P(3), (GLint *)P(4), (GLenum *)P(5), (char *)P(6)); return 1; }

    if (!strcmp(n, "glUniform1f")) { ((void (*)(GLint, GLfloat))g_Uniform1f)((GLint)A(0), F(1)); return 1; }
    if (!strcmp(n, "glUniform1i")) {
        if (g_texture_logs < 30) {
            fprintf(stderr, "gl: uniform1i prog=%u loc=%d value=%d\n",
                    g_current_program, (int)A(0), (int)A(1));
            g_texture_logs++;
        }
        ((void (*)(GLint, GLint))g_Uniform1i)((GLint)A(0), (GLint)A(1));
        return 1;
    }
    if (!strcmp(n, "glUniform1fv")) { ((void (*)(GLint, GLsizei, const GLfloat *))g_Uniform1fv)((GLint)A(0), (GLsizei)A(1), (const GLfloat *)P(2)); return 1; }
    if (!strcmp(n, "glUniform2fv")) { ((void (*)(GLint, GLsizei, const GLfloat *))g_Uniform2fv)((GLint)A(0), (GLsizei)A(1), (const GLfloat *)P(2)); return 1; }
    if (!strcmp(n, "glUniform3fv")) { ((void (*)(GLint, GLsizei, const GLfloat *))g_Uniform3fv)((GLint)A(0), (GLsizei)A(1), (const GLfloat *)P(2)); return 1; }
    if (!strcmp(n, "glUniform4fv")) { ((void (*)(GLint, GLsizei, const GLfloat *))g_Uniform4fv)((GLint)A(0), (GLsizei)A(1), (const GLfloat *)P(2)); return 1; }
    if (!strcmp(n, "glUniform1iv")) { ((void (*)(GLint, GLsizei, const GLint *))g_Uniform1iv)((GLint)A(0), (GLsizei)A(1), (const GLint *)P(2)); return 1; }
    if (!strcmp(n, "glUniform2iv")) { ((void (*)(GLint, GLsizei, const GLint *))g_Uniform2iv)((GLint)A(0), (GLsizei)A(1), (const GLint *)P(2)); return 1; }
    if (!strcmp(n, "glUniform3iv")) { ((void (*)(GLint, GLsizei, const GLint *))g_Uniform3iv)((GLint)A(0), (GLsizei)A(1), (const GLint *)P(2)); return 1; }
    if (!strcmp(n, "glUniform4iv")) { ((void (*)(GLint, GLsizei, const GLint *))g_Uniform4iv)((GLint)A(0), (GLsizei)A(1), (const GLint *)P(2)); return 1; }
    if (!strcmp(n, "glUniformMatrix4fv")) {
        if (g_matrix_logs < 30) {
            const GLfloat *m = (const GLfloat *)P(3);
            fprintf(stderr, "gl: matrix loc=%d count=%d transpose=%d m=%.3f %.3f %.3f %.3f / %.3f %.3f %.3f %.3f\n",
                    (int)A(0), (int)A(1), (int)A(2), m ? m[0] : 0.0f, m ? m[1] : 0.0f,
                    m ? m[2] : 0.0f, m ? m[3] : 0.0f, m ? m[4] : 0.0f, m ? m[5] : 0.0f,
                    m ? m[6] : 0.0f, m ? m[7] : 0.0f);
            g_matrix_logs++;
        }
        ((void (*)(GLint, GLsizei, GLboolean, const GLfloat *))g_UniformMatrix4fv)
            ((GLint)A(0), (GLsizei)A(1), (GLboolean)A(2), (const GLfloat *)P(3));
        return 1;
    }

    if (!strcmp(n, "glGenFramebuffers")) { ((void (*)(GLsizei, GLuint *))g_GenFramebuffers)((GLsizei)A(0), (GLuint *)P(1)); return 1; }
    if (!strcmp(n, "glDeleteFramebuffers")) { ((void (*)(GLsizei, const GLuint *))g_DeleteFramebuffers)((GLsizei)A(0), (const GLuint *)P(1)); return 1; }
    if (!strcmp(n, "glBindFramebuffer")) {
        GLenum t = (GLenum)A(0); GLuint b = (GLuint)A(1);
        if (t == 0x8D40u) {
            if (b != g_bound_fbo) rtt_dump_fbo(g_bound_fbo);
            g_fbo_bind_count++;
            unsigned known = 0;
            for (unsigned i = 0; i < g_fbo_bind_uniq; i++) {
                if (g_fbo_bind_ids[i] == b) { known = 1; break; }
            }
            if (!known && g_fbo_bind_uniq < 16) g_fbo_bind_ids[g_fbo_bind_uniq++] = b;
            g_bound_fbo = b;
            if (g_fbo_logs < 12) {
                fprintf(stderr, "gl: bind framebuffer %u\n", b);
                g_fbo_logs++;
            }
        }
        ((void (*)(GLenum, GLuint))g_BindFramebuffer)(t, b);
        return 1;
    }
    if (!strcmp(n, "glFramebufferTexture2D")) {
        GLenum attachment = (GLenum)A(1);
        GLuint texid = (GLuint)A(3);
        if (attachment == 0x8CE0u && texid && texid < TEX_MAP_MAX) {
            g_tex_fbo[texid] = g_bound_fbo;
            if (g_bound_fbo < 256) g_fbo_color_tex[g_bound_fbo] = texid;
        }
        ((void (*)(GLenum, GLenum, GLenum, GLuint, GLint))g_FramebufferTexture2D)
            ((GLenum)A(0), attachment, (GLenum)A(2), texid, (GLint)A(4));
        return 1;
    }
    if (!strcmp(n, "glFramebufferRenderbuffer")) { ((void (*)(GLenum, GLenum, GLenum, GLuint))g_FramebufferRenderbuffer)((GLenum)A(0), (GLenum)A(1), (GLenum)A(2), (GLuint)A(3)); return 1; }
    if (!strcmp(n, "glCheckFramebufferStatus")) {
        GLenum st = ((GLenum (*)(GLenum))g_CheckFramebufferStatus)((GLenum)A(0));
        if (st != 0x8CD5u && g_fbo_logs < 20) {
            fprintf(stderr, "gl: framebuffer status 0x%x\n", st);
            g_fbo_logs++;
        }
        c->r0 = st;
        return 1;
    }
    if (!strcmp(n, "glGenRenderbuffers")) { ((void (*)(GLsizei, GLuint *))g_GenRenderbuffers)((GLsizei)A(0), (GLuint *)P(1)); return 1; }
    if (!strcmp(n, "glDeleteRenderbuffers")) { ((void (*)(GLsizei, const GLuint *))g_DeleteRenderbuffers)((GLsizei)A(0), (const GLuint *)P(1)); return 1; }
    if (!strcmp(n, "glBindRenderbuffer")) { ((void (*)(GLenum, GLuint))g_BindRenderbuffer)((GLenum)A(0), (GLuint)A(1)); return 1; }
    if (!strcmp(n, "glRenderbufferStorage")) { ((void (*)(GLenum, GLenum, GLsizei, GLsizei))g_RenderbufferStorage)((GLenum)A(0), (GLenum)A(1), (GLsizei)A(2), (GLsizei)A(3)); return 1; }

    return 0;
#undef A
#undef F
#undef P
}
