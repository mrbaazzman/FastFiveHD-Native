/*
 * Host overrides for translated guest functions.
 *
 * Generated code under build/link/ is disposable: never patch it. If a guest function must be
 * replaced or accelerated on the host, handle its ELF address here and return 1. Returning 0 lets
 * the generated table dispatch to the translated body.
 *
 * This file is intentionally linked outside build/link/ so regeneration never deletes overrides.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include "cpu.h"
#include <ctype.h>
#include "mem.h"
#include "resolution.h"

#ifdef _WIN32
#include <windows.h>
#include <mmsystem.h>
#include "platform.h"
#endif

extern void fn_68d008(Cpu *c, uint8_t *mem); /* vox::DriverCallbackInterface::FillBuffer */
extern void fn_68d04c(Cpu *c, uint8_t *mem); /* vox::DriverCallbackInterface::_FillBuffer */
extern void fn_29eaec(Cpu *c, uint8_t *mem); /* Application::Init */
extern void fn_631da0(Cpu *c, uint8_t *mem); /* CCommonGLDriver::draw2DRectangle */
extern void fn_29d504(Cpu *c, uint8_t *mem); /* the engine's application re-init helper */

/*
 * The live Application instance.
 *
 * The engine destroys and re-creates the application during the boot (the Universal logo
 * screen does that), so the instance cannot be cached across the boot.  The engine keeps
 * its own pointer to it: `Application::s_pInstance`, ELF 0x7f8758
 * (`_ZN11Application11s_pInstanceE`).  Reading it is authoritative — it is the instance
 * the engine itself is using.
 */
uint32_t g_application_ptr;

#define APPLICATION_INSTANCE_ADDR 0x7f8758u

/*
 * 1 = let the engine keep its own 2D logical size and screen-size globals
 * (diagnostic switch: shows what the engine does on its own).
 */
int g_ui_native_2d;

static uint32_t live_application(const uint8_t *mem) {
    uint32_t app = *(const uint32_t *)(mem + APPLICATION_INSTANCE_ADDR);
    if (app && app + 0x258u < guest_mem_size) return app;
    return 0;
}

extern void call_target(Cpu *c, uint8_t *mem, uint32_t target);
extern int gl_bridge_bound_fbo(void);
extern int gl_bridge_fbo_size(int fbo, int *w, int *h);

uint32_t g_sm_playsnd, g_sm_play2d, g_sm_playevent, g_sm_play3d, g_sm_update;
uint32_t g_vox_valid_data, g_vox_create_emitter, g_vox_play, g_vox_update,
         g_vox_load_ds, g_vox_isready;
uint32_t g_vox_upd_src, g_vox_upd_emit;

/*
 * Host audio sink — replaces the Java AudioTrack path.
 *
 * vox::DriverAndroid::_InitAT performs ALL of its setup, including
 * `pthread_create(UpdateThreadedAT)`, inside `if (s_javaVM != NULL)`. The port has no JavaVM, so
 * the mixer thread never starts at all and the game is silent (not "audio plays but is dropped" —
 * nothing ever runs). The host supplies the sink instead:
 *   - _InitAT   -> open a Win32 waveOut device and start the shipped UpdateThreadedAT thread
 *   - DoCallbackAT -> let the shipped DriverCallbackInterface::_FillBuffer produce the PCM into a
 *                  guest scratch buffer, then send it to waveOut instead of AudioTrack
 * The engine's own mixer, decoders and timing thread are used unchanged.
 */
#define ELF_FILL_BUFFER     0x68d04cu  /* vox::DriverCallbackInterface::_FillBuffer   */
#define ELF_UPDATE_THREADED 0x698d28u  /* vox::DriverAndroid::UpdateThreadedAT        */
#define AUDIO_SCRATCH       0x02000000u/* free guest region: above thread stacks, below heap */
#define AUDIO_FRAMES        1024u      /* stereo frames per callback (matches the shipped cap) */
#define AUDIO_BUFFERS       4

#ifdef _WIN32
static HWAVEOUT g_wave;
static WAVEHDR g_wave_hdr[AUDIO_BUFFERS];
static short *g_wave_data[AUDIO_BUFFERS];
static int g_wave_next;
static int g_wave_ready;
static uint32_t g_audio_driver;
static uint32_t g_audio_writes;
static volatile LONG g_audio_running;
static uint8_t *g_audio_mem;
static HANDLE g_audio_thread;

/*
 * Audio pump. Runs on a host thread and calls the shipped mixer
 * `vox::DriverCallbackInterface::_FillBuffer(driver, pcm, frames)` on a private guest stack, then
 * queues the block to waveOut. waveOut's own buffer queue provides the pacing: when all buffers are
 * in flight the thread waits, which throttles it to real time.
 */
#define AUDIO_STACK_TOP 0x02080000u

static int audio_trace(void) {
    static int on = -1;
    if (on < 0) on = getenv("FAST5_AUDIO_TRACE") != NULL;
    return on;
}

/* --trace accounting: is the signal continuous, or a handful of blips in a sea of silence? */
static uint32_t g_dbg_blocks, g_dbg_loud, g_dbg_maxpeak;
static int g_dbg_write_err, g_dbg_prep_err, g_dbg_open_rc = -1;

/* FAST5_AUDIO_DUMP=1: capture exactly the bytes handed to waveOut, so the mix can be checked on
   its own (independent of any playback device) by playing the resulting WAV. */
static FILE *g_dump_file;
static uint32_t g_dump_bytes;

static void audio_dump_open(void) {
    if (!getenv("FAST5_AUDIO_DUMP")) return;
    g_dump_file = fopen("build/audio_dump.wav", "wb");
    if (!g_dump_file) return;
    uint8_t hdr[44] = {0};
    memcpy(hdr, "RIFF", 4);
    memcpy(hdr + 8, "WAVEfmt ", 8);
    hdr[16] = 16;                 /* fmt chunk size */
    hdr[20] = 1;                  /* PCM */
    hdr[22] = 2;                  /* channels */
    hdr[24] = 0x44; hdr[25] = 0xAC;      /* 44100 */
    hdr[28] = 0x10; hdr[29] = 0xB1; hdr[30] = 0x02; /* 176400 bytes/s */
    hdr[32] = 4;                  /* block align */
    hdr[34] = 16;                 /* bits */
    memcpy(hdr + 36, "data", 4);
    fwrite(hdr, 1, sizeof hdr, g_dump_file);
}

static void audio_dump_write(const void *data, uint32_t bytes) {
    if (!g_dump_file) return;
    fwrite(data, 1, bytes, g_dump_file);
    g_dump_bytes += bytes;
    /* Patch RIFF/data sizes every block so a killed process still leaves a playable file. */
    uint32_t riff_sz = 36u + g_dump_bytes, data_sz = g_dump_bytes;
    fseek(g_dump_file, 4, SEEK_SET);  fwrite(&riff_sz, 4, 1, g_dump_file);
    fseek(g_dump_file, 40, SEEK_SET); fwrite(&data_sz, 4, 1, g_dump_file);
    fseek(g_dump_file, 0, SEEK_END);
    fflush(g_dump_file);
}

static DWORD WINAPI audio_pump(LPVOID unused) {
    (void)unused;
    Cpu *cpu = (Cpu *)calloc(1, sizeof *cpu);
    if (!cpu || !g_audio_mem) return 0;
    while (g_audio_running) {
        int slot = g_wave_next % AUDIO_BUFFERS;
        WAVEHDR *h = &g_wave_hdr[slot];
        if (h->dwFlags & WHDR_INQUEUE) {
            Sleep(2);                       /* device busy: this is the pacing */
            continue;
        }
        cpu->r0 = g_audio_driver;
        cpu->r1 = AUDIO_SCRATCH;
        cpu->r2 = AUDIO_FRAMES;
        cpu->sp = AUDIO_STACK_TOP;
        cpu->lr = 0xDEADBEE0u;
        /*
         * Call `FillBuffer`, NOT `_FillBuffer`: FillBuffer is the designed entry point — it locks
         * the driver mutex at driver+4 and checks the "started" byte at driver+8 before calling
         * _FillBuffer. It also silently does nothing unless driver+8 is set.
         */
        fn_68d008(cpu, g_audio_mem);        /* mixer -> AUDIO_SCRATCH */

        memcpy(g_wave_data[slot], g_audio_mem + AUDIO_SCRATCH, (size_t)AUDIO_FRAMES * 4u);
        h->dwBufferLength = AUDIO_FRAMES * 4u;
        if (getenv("FAST5_AUDIO_TESTTONE")) {
            /* Sink self-test: a 440 Hz tone written where the mixer's output goes. If this is
               audible the waveOut path is sound and any remaining problem is the mix itself. */
            static uint32_t phase;
            for (uint32_t i = 0; i < AUDIO_FRAMES; i++) {
                int v = (int)(8000.0 * sin(2.0 * 3.14159265358979 * 440.0 * (double)phase / 44100.0));
                phase = (phase + 1u) % 44100u;
                g_wave_data[slot][i * 2] = (short)v;
                g_wave_data[slot][i * 2 + 1] = (short)v;
            }
        }
        audio_dump_write(g_wave_data[slot], AUDIO_FRAMES * 4u);
        {
            MMRESULT wr = waveOutWrite(g_wave, h, sizeof *h);
            if (wr != MMSYSERR_NOERROR && g_dbg_write_err++ == 0) {
                fprintf(stderr, "audio: waveOutWrite failed rc=%u\n", (unsigned)wr);
            }
        }
        g_wave_next = slot + 1;
        if (audio_trace()) {
            /* Accumulate over EVERY block: a high peak on a few blocks is not "audio", it is a
               blip. g_dbg_loud/g_dbg_blocks is the honest measure of whether sound is continuous. */
            int peak = 0;
            const short *pcm = (const short *)(g_audio_mem + AUDIO_SCRATCH);
            for (uint32_t i = 0; i < AUDIO_FRAMES * 2u; i++) {
                int v = pcm[i] < 0 ? -pcm[i] : pcm[i];
                if (v > peak) peak = v;
            }
            g_dbg_blocks++;
            if (peak) g_dbg_loud++;
            if ((uint32_t)peak > g_dbg_maxpeak) g_dbg_maxpeak = (uint32_t)peak;
            if ((g_audio_writes % 200u) == 0) {
                uint32_t head = *(uint32_t *)(g_audio_mem + g_audio_driver + 0x10u);
                fprintf(stderr,
                        "audio: blk %u last=%d max=%u loud=%u/%u srcList=%s wrErr=%d "
                        "prepErr=%d openRc=%d\n",
                        g_audio_writes, peak, g_dbg_maxpeak, g_dbg_loud, g_dbg_blocks,
                        (head == g_audio_driver + 0x10u) ? "EMPTY" : "non-empty",
                        g_dbg_write_err, g_dbg_prep_err, g_dbg_open_rc);
            }
        }
        g_audio_writes++;
    }
    free(cpu);
    return 0;
}

static int audio_host_open(void) {
    if (g_wave_ready) return 1;
    WAVEFORMATEX wf;
    memset(&wf, 0, sizeof wf);
    wf.wFormatTag = WAVE_FORMAT_PCM;
    wf.nChannels = 2;
    wf.nSamplesPerSec = 44100;
    wf.wBitsPerSample = 16;
    wf.nBlockAlign = 4;
    wf.nAvgBytesPerSec = 44100u * 4u;
    MMRESULT rc = waveOutOpen(&g_wave, WAVE_MAPPER, &wf, 0, 0, CALLBACK_NULL);
    g_dbg_open_rc = (int)rc;
    if (rc != MMSYSERR_NOERROR) {
        fprintf(stderr, "audio: waveOutOpen failed rc=%u\n", (unsigned)rc);
        return 0;
    }
    for (int i = 0; i < AUDIO_BUFFERS; i++) {
        g_wave_data[i] = (short *)calloc(AUDIO_FRAMES, 4);
        if (!g_wave_data[i]) return 0;
        memset(&g_wave_hdr[i], 0, sizeof g_wave_hdr[i]);
        g_wave_hdr[i].lpData = (LPSTR)g_wave_data[i];
        g_wave_hdr[i].dwBufferLength = AUDIO_FRAMES * 4u;
        MMRESULT pr = waveOutPrepareHeader(g_wave, &g_wave_hdr[i], sizeof g_wave_hdr[i]);
        if (pr != MMSYSERR_NOERROR) g_dbg_prep_err++;
    }
    g_wave_ready = 1;
    audio_dump_open();
    if (audio_trace()) {
        /* Which device did WAVE_MAPPER actually select? If this is not the device the user is
           listening on, the mixer can be perfect and nothing is heard. */
        UINT devs = waveOutGetNumDevs();
        WAVEOUTCAPSW caps;
        memset(&caps, 0, sizeof caps);
        if (devs && waveOutGetDevCapsW(WAVE_MAPPER, &caps, sizeof caps) == MMSYSERR_NOERROR) {
            fprintf(stderr, "audio: waveOut %d device(s), mapper='%ls', %d x %u frames, "
                            "prepErr=%d\n", (int)devs, caps.szPname, AUDIO_BUFFERS,
                    AUDIO_FRAMES, g_dbg_prep_err);
        } else {
            fprintf(stderr, "audio: waveOut %d device(s), caps unavailable, %d x %u frames\n",
                    (int)devs, AUDIO_BUFFERS, AUDIO_FRAMES);
        }
    }
    return 1;
}
#endif

extern void fn_50b9f4(Cpu *c, uint8_t *mem);
extern void fn_68d04c(Cpu *c, uint8_t *mem); /* vox::DriverCallbackInterface::_FillBuffer */
extern void call_target(Cpu *c, uint8_t *mem, uint32_t target);
extern int thread_win32_create(uint8_t *mem, uint32_t mem_size, uint32_t thread_ptr,
                               uint32_t start, uint32_t arg);
extern void fn_34f6e4(Cpu *c, uint8_t *mem); /* CarControl::UpdateAccelValues */
extern void fn_350b04(Cpu *c, uint8_t *mem); /* CarControl::UpdateSteeringAccelerometer */
extern void fn_350f54(Cpu *c, uint8_t *mem); /* CarControl::CalculateWheelAngle */
extern void fn_35078c(Cpu *c, uint8_t *mem); /* CarControl::SetManualInputFlags */
extern void fn_34f208(Cpu *c, uint8_t *mem); /* CarControl::UpdateCarControl */
extern void fn_30a7b0(Cpu *c, uint8_t *mem); /* GamePadManager::IsGamePadButtonActive */
extern void fn_454858(Cpu *c, uint8_t *mem); /* GP_RaceNormal::HandleNitro */
extern void fn_4538a8(Cpu *c, uint8_t *mem); /* GP_RaceNormal::UpdateCameraMode */
extern void fn_34f980(Cpu *c, uint8_t *mem); /* CarControl::UpdateSteering */
extern void fn_30a34c(Cpu *c, uint8_t *mem); /* Accelerometer::moved */
extern void fn_534fcc(Cpu *c, uint8_t *mem); /* CTextureManager::getPlaceHolder */
extern void fn_62abd8(Cpu *c, uint8_t *mem); /* CCommonGLDriver::setTexture */
extern void fn_637368(Cpu *c, uint8_t *mem); /* CTexture::bindImpl */
extern void fn_629760(Cpu *c, uint8_t *mem); /* CGLSLShader::linkProgram */
extern void fn_633e84(Cpu *c, uint8_t *mem); /* unused */
extern void fn_634e84(Cpu *c, uint8_t *mem); /* CCommonGLDriver::commitCurrentMaterialImpl */
extern void fn_62b770(Cpu *c, uint8_t *mem); /* commitCurrentMaterialIndirectParameters */
extern void fn_62e300(Cpu *c, uint8_t *mem); /* commitCurrentMaterialParametersAux<CMaterial> */
extern void fn_2b8080(Cpu *c, uint8_t *mem); /* glitch::core::quaternion::fromAngleAxis */
extern void fn_4dc320(Cpu *c, uint8_t *mem); /* ISceneNode::setRotation */
extern void fn_2b19ac(Cpu *c, uint8_t *mem); /* StateStack::PushState */
extern void fn_2b1a34(Cpu *c, uint8_t *mem); /* StateStack::PopState */
extern void fn_2b1af8(Cpu *c, uint8_t *mem); /* StateStack::PopAllStates */
extern void fn_2b18b8(Cpu *c, uint8_t *mem); /* StateStack::CloseToMenu */
extern void fn_2b18e0(Cpu *c, uint8_t *mem); /* StateStack::DoStateChange */
extern void fast5_log_note(const char *fmt, ...);
extern void fn_51ac28(Cpu *c, uint8_t *mem); /* IMaterialParameters<CMaterial>::setParameter<ITexture> */
extern void fn_51c260(Cpu *c, uint8_t *mem); /* IMaterialParameters<CMaterial>::setParameter<ITexture> (2) */
extern void fn_523f04(Cpu *c, uint8_t *mem); /* IMaterialParameters<CMaterialRenderer>::setParameter<ITexture> */
extern void fn_525048(Cpu *c, uint8_t *mem); /* IMaterialParameters<CMaterialRenderer>::setParameter<ITexture> (2) */
extern int gl_bridge_bound_fbo(void);
extern void gl_bridge_note_texture(uint32_t tex);
extern uint32_t guest_mem_size;

static int guest_cstr(uint8_t *mem, uint32_t ptr, char *out, size_t cap) {
    if (!ptr || ptr >= guest_mem_size || cap < 2) return 0;
    for (size_t i = 0; i + 1 < cap; i++) {
        if (ptr + i >= guest_mem_size) return 0;
        out[i] = (char)mem[ptr + i];
        if (!out[i]) return i > 0;
    }
    out[cap - 1] = 0;
    return 0;
}

static int texture_alias(const char *name, char *out, size_t cap) {
    const char *file = strrchr(name, '/');
    file = file ? file + 1 : name;
    const char *suffix = strstr(file, ".PVRTC2.tga");
    int pvrtc4 = 0;
    if (!suffix) {
        suffix = strstr(file, ".PVRTC4.tga");
        pvrtc4 = suffix != NULL;
    }
    if (!suffix) return 0;
    if (pvrtc4 && strncmp(file, "mm_", 3)) return 0;

    if (!strncmp(file, "mm_", 3)) file += 3;
    suffix = strstr(file, ".PVRTC2.tga");
    if (!suffix) suffix = strstr(file, ".PVRTC4.tga");
    if (!suffix) return 0;

    size_t stem = (size_t)(suffix - file);
    const char *ext = ".LINEAR.pvr";
    if (stem == 4 && !strncmp(file, "rock", 4)) ext = ".pvr";
    else if (stem == 5 && !strncmp(file, "grass", 5)) ext = ".pvr";
    else if (stem == 9 && !strncmp(file, "cement_00", 9)) ext = ".pvr";

    const char *prefix = "Track/Favelas/";
    if (strlen(prefix) + stem + strlen(ext) + 1u > cap) return 0;
    memcpy(out, prefix, strlen(prefix));
    memcpy(out + strlen(prefix), file, stem);
    memcpy(out + strlen(prefix) + stem, ext, strlen(ext) + 1u);

    /*
     * Only resolve to a variant that actually exists. The engine's suffix list has no
     * ".LINEAR.tga"; the installed data stores the Favelas/menu payloads under that name. An
     * unconditional rewrite sent other tracks (e.g. DESERT_*) to a Favelas path that cannot
     * exist and suppressed the engine's own fallbacks. Probe the derived overlay, then the
     * immutable tree. If neither has it, leave the name untouched.
     */
    char probe[512];
    snprintf(probe, sizeof probe, "build/overlay/%s", out);
    FILE *pf = fopen(probe, "rb");
    if (!pf) {
        snprintf(probe, sizeof probe, "GloftF5HP/res/%s", out);
        pf = fopen(probe, "rb");
    }
    if (!pf) return 0;
    fclose(pf);
    return 1;
}

static int alias_guest_name(Cpu *c, uint8_t *mem, const char *where) {
    static int alias_on = -1;
    if (alias_on < 0) alias_on = getenv("FAST5_NO_TEXTURE_ALIAS") == NULL;
    if (!alias_on) return 0;
    char name[512];
    char alias[256];
    if (!guest_cstr(mem, c->r1, name, sizeof name)) return 0;
    if (!texture_alias(name, alias, sizeof alias)) return 0;
    if (strlen(alias) > strlen(name)) return 0;
    fprintf(stderr, "texture fallback (%s): %s -> %s\n", where, name, alias);
    memcpy(mem + c->r1, alias, strlen(alias) + 1u);
    return 1;
}

/* Wheel scene-node pointers taken from the three menu cars (car+0x1140..0x115c). */
static uint32_t g_wheel_nodes[24];
static int g_wheel_node_count = 0;

/*
 * call_override runs on every indirect call in the game, so the long if-chain below is a real cost
 * (it is entered ~60 times per dispatch when nothing matches). Reject quickly against a bitmap of
 * the addresses that actually have a hook, then fall through to the chain only for those.
 */
static uint32_t g_hook_bitmap[0x10000u / 32u];      /* one bit per 64 KiB of guest address space */
static uint8_t g_hook_bitmap_ready;

static void hook_bitmap_init(void) {
    static const uint32_t hooked[] = {
        0x2ca704u, 0x2caddcu, 0x2cde4cu, 0x2cecf0u, 0x2d3280u, 0x2d36c8u, 0x2dd140u,
        0x534fccu, 0x521524u, 0x517668u, 0x420e0cu, 0x420ea8u, 0x420f64u,
        0x42182cu, 0x2b19acu, 0x2b1a34u, 0x2b1af8u, 0x2b18b8u, 0x4c0b70u, 0x2b8080u,
        0x4dc320u, 0x2ca50cu, 0x2caff8u, 0x483accu, 0x2cb430u, 0x2ca6f0u,
        0x2d1c88u, 0x52add8u, 0x58cc48u, 0x58c0b8u, 0x58ec70u, 0x4a00e4u, 0x62abd8u,
        0x504764u, 0x619238u, 0x619170u, 0x61a260u, 0x29a0b4u, 0x29a0e4u, 0x29be34u,
        0x29a6b4u, 0x29a590u, 0x29a704u, 0x29a75cu, 0x29beacu, 0x29bedcu,
        0x29a7e4u,
        0x29a7b4u, 0x29a22cu, 0x520a20u, 0x5204a0u, 0x637368u, 0x629760u, 0x62e300u,
        0x62b770u, 0x51c260u, 0x523f04u, 0x525048u,
        0x30a34cu, 0x34f6e4u, 0x34f980u, 0x350b04u, 0x350f54u, 0x35078cu, 0x34f208u, 0x30a7b0u,
        0x454858u, 0x4538a8u, 0x68963cu, 0x34f980u, 0x393978u,
        0x391e84u, 0x391f58u, 0x392874u, 0x3927dcu, 0x392898u,
        0x35d058u, 0x35d190u, 0x35cef8u, 0x35d268u, 0x35eef8u,
        0x65e868u, 0x65e928u, 0x65eb74u, 0x65e4b0u, 0x65e4bcu, 0x65e840u,
        0x65f530u, 0x65f56cu,
        0x34c7a4u,
        0x4d8228u, /* CCameraSceneNode::setAspectRatio */
        0x631da0u, /* CCommonGLDriver::draw2DRectangle */
        0x3e6f80u, /* CMenuManager::GetRayFromScreenCoordinates */
        0x4460f0u, /* GS_UniversalLogo::StateRender */
        0x3c7748u, /* CFont::DrawString */
        0x29eaecu, /* Application::Init */
        0x29d504u, /* application re-init helper */
    };
    for (unsigned i = 0; i < sizeof hooked / sizeof hooked[0]; i++) {
        uint32_t page = hooked[i] >> 16;
        if (page < 0x10000u) g_hook_bitmap[page >> 5] |= 1u << (page & 31u);
    }
    g_hook_bitmap_ready = 1;
}

int call_override(Cpu *c, uint8_t *mem, uint32_t target) {
    if (!g_hook_bitmap_ready) hook_bitmap_init();
    {
        uint32_t page = target >> 16;
        if (page >= 0x10000u || !(g_hook_bitmap[page >> 5] & (1u << (page & 31u)))) {
            return 0;
        }
    }
    /* Sound-path counters: did the game ever ASK to play anything? Sounds load fine (LoadSound
       logs "OK"), but the mixer's source list stays empty, so the question is whether the play
       entry points are reached at all. */
    {
        extern uint32_t g_sm_playsnd, g_sm_play2d, g_sm_playevent, g_sm_play3d, g_sm_update;
        switch (target) {
        case 0x35d058u: g_sm_playsnd++; return 0;
        case 0x35d190u: g_sm_play2d++; return 0;
        case 0x35cef8u: g_sm_playevent++; return 0;
        case 0x35d268u: g_sm_play3d++; return 0;
        case 0x35eef8u: g_sm_update++; return 0;
        /* vox::VoxEngine::* thin wrappers (Ghidra - 0x10000). These are the real engine gates:
           Play3DInternal bails out unless VoxEngine::IsValid(DataHandle) is true, so counting
           these separates "request never reached the engine" from "engine rejected it". */
        case 0x65e868u: g_vox_valid_data++; return 0;    /* IsValid(DataHandle)      */
        case 0x65e928u: g_vox_create_emitter++; return 0; /* CreateEmitterAsync      */
        case 0x65eb74u: g_vox_play++; return 0;          /* Play                    */
        case 0x65e4b0u: g_vox_update++; return 0;        /* Update                  */
        case 0x65e4bcu: g_vox_load_ds++; return 0;       /* LoadDataSource          */
        case 0x65e840u: g_vox_isready++; return 0;       /* IsReady                 */
        /* VoxEngine::Initialize creates two VoxThreads that run these. They are what promote a
           queued emitter into a driver source; if they never fire, the driver list stays empty. */
        case 0x65f530u: g_vox_upd_src++; return 0;       /* UpdateSourcesThreaded   */
        case 0x65f56cu: g_vox_upd_emit++; return 0;      /* UpdateEmittersThreaded  */
        default: break;
        }
    }
    static int tex_trace = -1;
    if (tex_trace < 0) tex_trace = getenv("FAST5_TEXTURE_TRACE") != NULL;
    static int wheel_trace = -1;
    if (wheel_trace < 0) wheel_trace = getenv("FAST5_WHEEL_TRACE") != NULL;
    static int race_trace = -1;
    if (race_trace < 0) race_trace = getenv("FAST5_RACE_TRACE") != NULL;
    static int steer_trace = -1;
    if (steer_trace < 0) steer_trace = getenv("FAST5_STEER_TRACE") != NULL;
    if (steer_trace && target == 0x30a34cu) {
        /* Accelerometer::moved(this, x, y, z) (Ghidra 0x31a34c): the entry point for tilt input.
           Call the original, then read the angle cache _calcAngles produced (+0x38/+0x40/+0x48).
           Measuring the angles is what sizes the deadzone and gain for keyboard steering. */
        float x, y, z;
        memcpy(&x, &c->r1, 4);
        memcpy(&y, &c->r2, 4);
        memcpy(&z, &c->r3, 4);
        uint32_t obj = c->r0;
        fn_30a34c(c, mem);
        static int moved_logs;
        if (obj && obj + 0x4cu < guest_mem_size) {
            float a0, a1, a2;
            memcpy(&a0, mem + obj + 0x38u, 4);
            memcpy(&a1, mem + obj + 0x40u, 4);
            memcpy(&a2, mem + obj + 0x48u, 4);
            /* Log densely while tilt is non-zero so the input->angle mapping is unambiguous. */
            int interesting = (x != 0.0f || y != 0.0f);
            static int pressed_logs;
            if (moved_logs < 12 || (interesting && pressed_logs < 16)) {
                if (interesting) pressed_logs++;
                fprintf(stderr,
                        "accelMoved n=%d in=(%.3f %.3f %.3f) ang=(%.4f %.4f %.4f)\n",
                        moved_logs + 1, x, y, z, a0, a1, a2);
            }
        }
        moved_logs++;
        return 1;
    }
    if (steer_trace && target == 0x34f6e4u) {
        /*
         * CarControl::UpdateAccelValues (Ghidra 0x35f6e4): reads the accelerometer object at
         * +4 (x) and +0xc (z), low-passes them 0.7/0.3, and stores the steering value at
         * carControl+0. Logging it proves whether tilt actually reaches the steering model.
         */
        fn_34f6e4(c, mem);
        uint32_t cc = c->r0;
        static int acc_logs;
        if (cc && cc + 0x30u < guest_mem_size) {
            float steer, raw0, raw1, raw2, s28, s2c;
            memcpy(&steer, mem + cc + 0u, 4);
            memcpy(&raw0, mem + cc + 0x10u, 4);
            memcpy(&raw1, mem + cc + 0x14u, 4);
            memcpy(&raw2, mem + cc + 0x18u, 4);
            memcpy(&s28, mem + cc + 0x28u, 4);
            memcpy(&s2c, mem + cc + 0x2cu, 4);
            if (acc_logs < 40 || (acc_logs % 120) == 0) {
                fprintf(stderr,
                        "accelVals n=%d steer=%.4f filt=(%.3f %.3f %.3f) s28=%.4f s2c=%.3f\n",
                        acc_logs + 1, steer, raw0, raw1, raw2, s28, s2c);
            }
        }
        acc_logs++;
        return 1;
    }
    if (steer_trace && target == 0x350b04u) {
        /*
         * CarControl::UpdateSteeringAccelerometer(int): the accelerometer steering mode.
         * this+0 is the tilt angle, this+8 is the car, car+0x40c is the wheel angle it drives.
         * Measuring in/out here is what proves keyboard->tilt actually reaches the car.
         */
        uint32_t cc = c->r0;
        float in_angle = 0.0f, wheel_before = 0.0f;
        uint32_t car = 0;
        if (cc && cc + 0xcu < guest_mem_size) {
            memcpy(&in_angle, mem + cc + 0u, 4);
            car = *(uint32_t *)(mem + cc + 8u);
        }
        if (car && car + 0x410u < guest_mem_size) {
            memcpy(&wheel_before, mem + car + 0x40cu, 4);
        }
        fn_350b04(c, mem);
        float wheel_after = wheel_before;
        if (car && car + 0x410u < guest_mem_size) {
            memcpy(&wheel_after, mem + car + 0x40cu, 4);
        }
        static int sa_logs;
        /* Log densely while the angle is non-zero, so the press is unambiguously captured. */
        if (sa_logs < 6 || (in_angle != 0.0f && sa_logs < 400)) {
            fprintf(stderr,
                    "steerAccel n=%d cc=0x%08x angle=%.4f (%.2f deg) wheel %.4f -> %.4f\n",
                    sa_logs + 1, cc, in_angle, in_angle * 57.29578, wheel_before, wheel_after);
        }
        sa_logs++;
        return 1;
    }
    if (target == 0x34f208u) {
        /*
         * CarControl::UpdateCarControl(int) — per-car update, runs before UpdateSteering.
         *
         * IMPORTANT: this is called for the player car AND every CPU car. `car+0x400` is the
         * per-car control mode (0 = human / global gamepad, non-zero = AI reading its own button
         * mask at carControl+0xb4). Only the player car already sits at 0; forcing 0 on every car
         * switched the AI onto the global gamepad, which made the keyboard drive *all* cars.
         * So: never change the mode, and only override the scheme for the car that is already 0.
         */
        uint32_t cc = c->r0;
        if (cc && cc + 0x94u < guest_mem_size) {
            uint32_t car = *(uint32_t *)(mem + cc + 8u);
            uint32_t mode = (car && car + 0x404u < guest_mem_size)
                            ? *(uint32_t *)(mem + car + 0x400u) : 1u;
            if (mode == 0u && *(uint32_t *)(mem + cc + 0x90u) == 1u) {
                *(uint32_t *)(mem + cc + 0x90u) = 0u;   /* player: onscreen-buttons scheme */
            }
        }
        fn_34f208(c, mem);
        return 1;
    }
    if (target == 0x35078cu) {
        /*
         * CarControl::SetManualInputFlags() (Ghidra 0x36078c) — rebuilds the per-frame input
         * bitmask at CarControl+0xc. Bit 4 = steer left, bit 8 = steer right; the touch onscreen
         * buttons set them through IsGamePadDown(this, 4/8). The host keyboard drives those two
         * bits here so the shipped UpdateSteeringOnscreenButtons consumes them exactly as touch
         * input would.
         */
        uint32_t cc = c->r0;            /* capture before the call: it may clobber r0 */
        fn_35078c(c, mem);
        if (cc && cc + 0x10u < guest_mem_size) {
            extern int platform_held_key(int code);
            /*
             * Only the player car: car+0x400 == 0 means human input. CPU cars use their own mask
             * and must be left alone, otherwise the keyboard drives every car on the track.
             */
            uint32_t car = *(uint32_t *)(mem + cc + 8u);
            uint32_t mode = (car && car + 0x404u < guest_mem_size)
                            ? *(uint32_t *)(mem + car + 0x400u) : 1u;
            if (mode == 0u) {
            uint32_t flags = *(uint32_t *)(mem + cc + 0xcu);
            uint32_t before = flags;
            /*
             * CarControl input bitmask, from the consumers inside UpdateCarControl:
             *   0x01 throttle, 0x02 brake, 0x04 steer left, 0x08 steer right.
             * The shipped SetManualInputFlags sets 0x01 unconditionally (auto-throttle), which is
             * why the car drives itself. Clearing it and re-setting it only while UP is held gives
             * manual acceleration; CTRL drives the brake bit.
             */
            flags &= ~(0x1u | 0x2u | 0x4u | 0x8u);
            if (platform_held_key(19) || platform_held_key(51)) flags |= 0x1u;   /* UP / W */
            if (platform_held_key(113) || platform_held_key(114)) flags |= 0x2u; /* CTRL */
            if (platform_held_key(21) || platform_held_key(29)) flags |= 0x4u;   /* LEFT / A */
            if (platform_held_key(22) || platform_held_key(32)) flags |= 0x8u;   /* RIGHT / D */
            *(uint32_t *)(mem + cc + 0xcu) = flags;
            if (steer_trace) {
                static int sf_logs;
                static uint32_t last_flags;
                if (flags != last_flags) {
                    fprintf(stderr, "setFlags CHANGED cc=0x%08x before=0x%x after=0x%x\n",
                            cc, before, flags);
                    last_flags = flags;
                } else if ((flags & 0xcu) != 0u && sf_logs < 200) {
                    fprintf(stderr, "setFlags HELD cc=0x%08x flags=0x%x\n", cc, flags);
                    sf_logs++;
                }
            }
            }   /* mode == 0: player car only */
        }
        return 1;
    }
    if (target == 0x350f54u) {
        /*
         * CarControl::CalculateWheelAngle(int) — the steering dispatcher.
         *
         * The shipped build picks a scheme from CarControl::m_settings.steeringMode (this+0x90):
         *   1 -> accelerometer, 2 -> onscreen wheel, anything else -> onscreen buttons.
         * We want the touch/button scheme on a desktop, so this hook rewrites the setting to 0 and
         * hands control back to the shipped UpdateSteeringOnscreenButtons, which reads the input
         * flags we drive from the keyboard. That keeps the engine's own steering model, rate limit
         * and clamping -- nothing about the physics is bypassed, and the camera does not move
         * because the accelerometer path (which feeds the tilt/lean behaviour) is never entered.
         */
        uint32_t cc = c->r0;
        /*
         * Player car only. car+0x400 == 0 is the human/global-gamepad mode; CPU cars sit at 1 and
         * read their own button mask. Rewriting the mode (as an earlier revision did) switched the
         * AI onto the global gamepad and made the keyboard drive every car on the track, so the
         * mode is never touched here.
         */
        if (cc && cc + 0x94u < guest_mem_size) {
            uint32_t car = *(uint32_t *)(mem + cc + 8u);
            uint32_t mode = (car && car + 0x404u < guest_mem_size)
                            ? *(uint32_t *)(mem + car + 0x400u) : 1u;
            if (mode == 0u && *(uint32_t *)(mem + cc + 0x90u) != 0u) {
                *(uint32_t *)(mem + cc + 0x90u) = 0u;   /* player: onscreen-buttons scheme */
            }
        }
        fn_350f54(c, mem);
        if (steer_trace) {
            uint32_t car2 = (cc && cc + 0xcu < guest_mem_size)
                            ? *(uint32_t *)(mem + cc + 8u) : 0;
            if (car2 && car2 + 0x410u < guest_mem_size) {
                uint32_t flags = *(uint32_t *)(mem + cc + 0xcu);
                float wheel, out;
                memcpy(&wheel, mem + car2 + 0x40cu, 4);
                memcpy(&out, &c->r0, 4);
                static int wheel_logs;
                if ((wheel_logs < 10 || (flags & 0xcu) != 0) && wheel_logs < 400) {
                    fprintf(stderr, "touchSteer n=%d flags=0x%x wheel=%.4f ret=%.4f\n",
                            wheel_logs + 1, flags, wheel, out);
                }
                wheel_logs++;
            }
        }
        return 1;
    }
    if (target == 0x34f980u) {
        /*
         * CarControl::UpdateSteering(int) — called every frame for the player AND every CPU car.
         * Logging only; the scheme/mode are switched for the player car in the
         * UpdateCarControl/CalculateWheelAngle hooks. Nothing is written to the car here so the AI
         * keeps its own physics mode.
         */
        uint32_t cc = c->r0;
        if (steer_trace) {
            static int us_logs;
            if (us_logs < 400) {
                uint32_t car = (cc && cc + 0xcu < guest_mem_size)
                               ? *(uint32_t *)(mem + cc + 8u) : 0;
                uint32_t m = (car && car + 0x404u < guest_mem_size)
                             ? *(uint32_t *)(mem + car + 0x400u) : 0xdeadbeefu;
                uint32_t s = (cc && cc + 0x94u < guest_mem_size)
                             ? *(uint32_t *)(mem + cc + 0x90u) : 0xdeadbeefu;
                fprintf(stderr, "updSteer n=%d cc=0x%08x car=0x%08x mode400=%u scheme90=%u\n",
                        us_logs + 1, cc, car, m, s);
            }
            us_logs++;
        }
        {
            uint32_t car = (cc && cc + 0xcu < guest_mem_size)
                           ? *(uint32_t *)(mem + cc + 8u) : 0;
            uint32_t flags_before = (cc && cc + 0x10u < guest_mem_size)
                                    ? *(uint32_t *)(mem + cc + 0xcu) : 0;
            float wheel_before = 0.0f;
            if (car && car + 0x410u < guest_mem_size) {
                memcpy(&wheel_before, mem + car + 0x40cu, 4);
            }
            fn_34f980(c, mem);
            float wheel_after = wheel_before;
            if (car && car + 0x410u < guest_mem_size) {
                memcpy(&wheel_after, mem + car + 0x40cu, 4);
            }
            if (steer_trace && (flags_before & 0xcu) != 0u) {
                static int wl;
                if (wl < 60) {
                    fprintf(stderr, "wheelResp n=%d cc=0x%08x flags=0x%x wheel %.4f -> %.4f\n",
                            wl + 1, cc, flags_before, wheel_before, wheel_after);
                    wl++;
                }
            }
        }
        return 1;
    }
    if (target == 0x68963cu) {
        /* vox::DriverAndroid::_InitAT(void*) — replaced. The shipped body performs everything,
           including `pthread_create(UpdateThreadedAT)`, inside `if (s_javaVM != NULL)`; with no
           JavaVM the mixer thread never starts and the game is completely silent. Open waveOut and
           run the shipped mixer from a host pump thread instead. */
        uint32_t drv = c->r0;
        if (!drv || drv + 0x6cu >= guest_mem_size) return 0;
        /*
         * The shipped _InitAT calls SetDriverSampleRate(44100) BEFORE the JavaVM check, so on
         * Android this always ran. It was skipped here, and every source derives its resample step
         * from it (`step = doppler * (pos * pitch >> 14) >> 14`); with a zero rate the step is
         * nonsense and FillBufferStereo16 walks the sample data with the wrong stride, which
         * produced a DC ramp instead of audio. Do it first, exactly as shipped.
         */
        {
            Cpu s = *c;
            s.r0 = 44100u;
            s.r1 = 0; s.r2 = 0; s.r3 = 0;
            s.sp = c->sp;
            call_target(&s, mem, 0x68ca80u);   /* vox::DriverCallbackSourceInterface::SetDriverSampleRate */
        }
        *(uint32_t *)(mem + drv + 0x54u) = 1u;
        *(uint32_t *)(mem + drv + 0x58u) = AUDIO_FRAMES;
        *(uint32_t *)(mem + drv + 0x5cu) = AUDIO_FRAMES;
        mem[drv + 0x60u] = 0;
        /*
         * +0x08 is the driver's "started" byte. The shipped `UpdateThreadedAT` sets it to 1 only
         * after the AudioTrack has been constructed, i.e. once the driver can actually deliver
         * audio. We skipped it, and the vox engine never created a single source on this driver
         * (the mixer's list at driver+0x10 stayed empty for the whole run while the engine happily
         * created 480 emitters). Set it, because our waveOut device is open at this point.
         */
        mem[drv + 0x08u] = 1;
        g_audio_driver = drv;
#ifdef _WIN32
        g_audio_mem = mem;
        if (!audio_host_open()) return 1;
        if (!g_audio_thread) {
            g_audio_running = 1;
            g_audio_thread = CreateThread(NULL, 0, audio_pump, NULL, 0, NULL);
            if (audio_trace()) {
                fprintf(stderr, "audio: pump thread started (driver=0x%08x)\n", drv);
            }
        }
#endif
        return 1;
    }
    if (target == 0x34c7a4u) {
        /*
         * FormatMoney(int value, char *dst, int signMode) — probe. The cash counter renders blank
         * even though the profile holds a non-zero value, so log what this actually produces.
         */
        extern void fn_34c7a4(Cpu *c, uint8_t *mem);
        static int logs, text_trace = -1;
        if (text_trace < 0) text_trace = getenv("FAST5_TEXT_TRACE") != NULL;
        uint32_t val = c->r0, dst = c->r1, mode = c->r2;
        Cpu k = *c;
        fn_34c7a4(&k, mem);
        if (text_trace && logs < 40 && dst + 64u < guest_mem_size) {
            fprintf(stderr, "fmtMoney: val=%d mode=%u -> '%.48s'\n", (int32_t)val, mode,
                    (const char *)(mem + dst));
            logs++;
        }
        c->r0 = k.r0;
        return 1;
    }
    {
        /*
         * Native reflection path counters (FAST5_POSTFX_TRACE=1). These are the shipped
         * PostEffects functions themselves, called by the engine's own frame/update code:
         *   0x391e84 PreDraw, 0x391f58 PostDraw, 0x392874 GetPrevRT, 0x3927dc ActivateEffect,
         *   0x392898 ChangeStateEffect. Counting them proves whether the live path is native.
         */
        static uint32_t pre_draw, post_draw, prev_rt;
        static int postfx_trace = -1;
        if (postfx_trace < 0) postfx_trace = getenv("FAST5_POSTFX_TRACE") != NULL;
        if (target == 0x391e84u) {
            pre_draw++;
            if (postfx_trace && (pre_draw % 300u) == 1u) {
                fprintf(stderr, "postfx: frames=%u preDraw=%u postDraw=%u getPrevRT=%u\n",
                        pre_draw, pre_draw, post_draw, prev_rt);
            }
            return 0;
        }
        if (target == 0x391f58u) { post_draw++; return 0; }
        if (target == 0x392874u) { prev_rt++; return 0; }
        if (target == 0x3927dcu) {
            if (postfx_trace) fprintf(stderr, "postfx: ActivateEffect id=%u\n", c->r1);
            return 0;
        }
        if (target == 0x392898u) {
            if (postfx_trace) fprintf(stderr, "postfx: ChangeStateEffect lastRendered=0x12\n");
            return 0;
        }
    }
    if (target == 0x393978u) {
        /*
         * FUN_003a3978: PostEffects' "is this GS_Race?" gate. PreDraw/PostDraw skip the whole
         * off-screen pass while the active effect id equals the last-rendered one, which freezes
         * RTT_1/RTT_2 for the rest of the race. Every car batch samples GetPrevRT() as its
         * ReflSampler, so that frozen buffer is the roof/hood artifact. The engine created these
         * targets precisely to be sampled; refreshing them each frame is the coherent behaviour.
         * Enabled by default; FAST5_LIVE_REFLECTION=0 restores the frozen shipped gate for A/B.
         */
        static int live_reflection = -1;
        if (live_reflection < 0) {
            const char *e = getenv("FAST5_LIVE_REFLECTION");
            live_reflection = !(e && e[0] == '0' && e[1] == 0);
        }
        if (live_reflection) {
            c->r0 = 0;
            return 1;
        }
        return 0;
    }
    if (target == 0x4538a8u) {
        /*
         * GP_RaceNormal::UpdateCameraMode() — the camera is NOT a gamepad action; it is driven by
         * two bytes on GP_RaceNormal that the touch HUD sets in ExecuteTouch:
         *   gp+0x1c  one-shot "cycle camera" request, consumed here (set to 0 after use)
         *   gp+0x1d  held "look back" flag, copied to camera+0x47 every frame
         * The keyboard drives them here, before the original runs, so the engine's own camera
         * table, sounds and mode filtering all still apply.
         */
        extern int platform_held_key(int code);
        uint32_t gp = c->r0;
        int cam_trace = getenv("FAST5_CAM_TRACE") != NULL;
        int requested = 0;
        if (gp && gp + 0x1eu < guest_mem_size) {
            static int cam_was_down;
            int down = platform_held_key(31);            /* C */
            if (down && !cam_was_down) { mem[gp + 0x1cu] = 1; requested = 1; }
            cam_was_down = down;
            mem[gp + 0x1du] = (uint8_t)(platform_held_key(54) ? 1 : 0);  /* Z = look back */
        }
        fn_4538a8(c, mem);
        /* The original clears gp+0x1c once it accepts the cycle (subject to its own 1000 ms
           cooldown). Watching it go 1 -> 0 proves the camera really advanced. */
        if (cam_trace && requested && gp && gp + 0x1eu < guest_mem_size) {
            fprintf(stderr, "camCycle gp=0x%08x consumed=%d\n", gp,
                    mem[gp + 0x1cu] == 0 ? 1 : 0);
        }
        return 1;
    }
    if (target == 0x454858u) {
        /*
         * GP_RaceNormal::HandleNitro(int) — runs every frame while racing.
         *
         * Nitro is ONE TAP, not a hold: the touch HUD sets the "boost active" byte at
         * GP_RaceNormal+0x17 once, and HandleNitro's own timer (accumulated in +0x18, ended at
         * 5999) turns it off again.
         *
         * Critically, starting a boost also CONSUMES A CHARGE. The shipped touch handler
         * (GP_RaceNormal::ExecuteTouch) does that before setting the flag:
         *     player = Game::GetPlayer(-1);
         *     if (player->charges (player+0x16d4) < 1) -> refuse (play "empty" sound)
         *     else { player->charges--; gp+0x17 = 1; player->boosting (player+0x16d8) = 1; }
         * Skipping the decrement is what allowed an endless boost, so it is reproduced here.
         */
        extern int platform_held_key(int code);
        extern void fn_2a8b54(Cpu *c, uint8_t *mem);   /* Game::GetPlayer */
        uint32_t gp = c->r0;
        if (gp && gp + 0x18u < guest_mem_size) {
            static int nitro_was_down;
            int down = platform_held_key(62);            /* SPACE */
            if (down && !nitro_was_down && mem[gp + 0x17u] == 0) {
                /* player = Game::GetPlayer(-1) */
                Cpu q = *c;
                q.r0 = 0xFFFFFFFFu;
                fn_2a8b54(&q, mem);
                uint32_t player = q.r0;
                if (player && player + 0x16dcu < guest_mem_size) {
                    int32_t charges;
                    memcpy(&charges, mem + player + 0x16d4u, 4);
                    if (charges >= 1) {
                        charges -= 1;
                        memcpy(mem + player + 0x16d4u, &charges, 4);
                        mem[gp + 0x17u] = 1;             /* start the boost */
                        mem[player + 0x16d8u] = 1;
                        if (getenv("FAST5_PAD_TRACE")) {
                            fprintf(stderr, "nitroTAP gp=0x%08x player=0x%08x chargesLeft=%d\n",
                                    gp, player, charges);
                        }
                    } else if (getenv("FAST5_PAD_TRACE")) {
                        fprintf(stderr, "nitroTAP refused: no charges (player=0x%08x)\n", player);
                    }
                }
            }
            nitro_was_down = down;
        }
        fn_454858(c, mem);
        return 1;
    }
    if (target == 0x30a7b0u) {
        /*
         * GamePadManager::IsGamePadButtonActive(action, mask, pad) — the query every non-steering
         * control goes through. GP_RaceNormal::ExecuteGamePad reads:
         *   action 2, mask 0x200 -> nitro      action 2, mask 0x40 -> camera cycle
         *   action 2, mask 0x80  -> pause
         * The action index selects which event window is read (ProcessGamePads fills +4 with
         * "pressed this frame", +8 with "held", +0xc with "released this frame"), so:
         *   action 0 / 2 are EDGE queries and action 1 is a LEVEL query.
         * The host answers the mapped ones directly and defers everything else to the shipped code.
         */
        static int pad_trace = -1;
        if (pad_trace < 0) pad_trace = getenv("FAST5_PAD_TRACE") != NULL;
        {
            extern int platform_held_key(int code);
            uint32_t action = c->r1, mask = c->r2;
            /*
             * Keyboard -> gamepad-button map, taken from the shipped consumers:
             *   mask 0x200 -> nitro  (GP_RaceNormal::ExecuteGamePad, action 2)
             *   mask 0x40  -> camera (GP_RaceNormal::ExecuteGamePad, action 2)
             *
             * Deliberately NOT mapped: mask 0x800. It sets CarControl bit 0x400, which acts as an
             * extra brake input (there is no handbrake in this game — drift is brake + turn). That
             * is why binding Z to it made the car drift whenever Z was held while steering. */
            int key = 0;
            if (mask == 0x200u) key = 62;        /* nitro -> SPACE */
            else if (mask == 0x40u) key = 31;    /* camera -> C */

            if (key) {
                /* Edge query: report true only on the transition into "down". */
                static uint32_t edge_mask[64];
                static uint32_t edge_key[64];
                static int edge_down[64];
                static int nedge;
                int slot = -1;
                for (int i = 0; i < nedge; i++) {
                    if (edge_mask[i] == mask && edge_key[i] == (uint32_t)key) { slot = i; break; }
                }
                if (slot < 0 && nedge < 64) {
                    slot = nedge++;
                    edge_mask[slot] = mask;
                    edge_key[slot] = (uint32_t)key;
                    edge_down[slot] = 0;
                }
                if (slot >= 0) {
                    int down = platform_held_key(key);
                    int rising = down && !edge_down[slot];
                    edge_down[slot] = down;
                    if (rising && pad_trace) {
                        fprintf(stderr, "padInject action=%u mask=0x%x key=%d\n",
                                action, mask, key);
                    }
                    c->r0 = (uint32_t)rising;
                    return 1;
                }
            }
        }
        if (pad_trace) {
            uint32_t action = c->r1, mask = c->r2, pad = c->r3;
            static uint32_t seen_a[64], seen_m[64];
            static int nseen;
            int known = 0;
            for (int i = 0; i < nseen; i++) {
                if (seen_a[i] == action && seen_m[i] == mask) { known = 1; break; }
            }
            if (!known && nseen < 64) {
                seen_a[nseen] = action;
                seen_m[nseen] = mask;
                nseen++;
                fprintf(stderr, "padAction NEW action=%u mask=0x%x pad=%u\n", action, mask, pad);
            }
        }
        fn_30a7b0(c, mem);
        return 1;
    }
    if (target == 0x2ca704u) {
        /* MenuScene::LoadCars(slot) */
        if (tex_trace) {
            static int lc_logs = 0;
            if (lc_logs < 20) {
                fprintf(stderr, "loadCars slot=%d scene=0x%08x lr=0x%08x\n", (int32_t)c->r1, c->r0, c->lr);
                lc_logs++;
            }
        }
        return 0;
    }
    if (target == 0x2caddcu) {
        /* MenuScene::LoadCar */
        if (tex_trace) {
            static int lc2_logs = 0;
            if (lc2_logs < 20) {
                fprintf(stderr, "loadCar(one) scene=0x%08x a1=%d lr=0x%08x\n", c->r0, (int32_t)c->r1, c->lr);
                lc2_logs++;
            }
        }
        return 0;
    }
    if (target == 0x2cde4cu || target == 0x2cecf0u) {
        /* RaceCar::RaceCar(this, carIndex, options*, tint) */
        if (tex_trace) {
            static int rc_logs = 0;
            if (rc_logs < 20) {
                uint32_t opt = c->r2;
                uint32_t o0 = (opt && opt + 12u < guest_mem_size) ? *(uint32_t *)(mem + opt) : 0;
                uint32_t o4 = (opt && opt + 16u < guest_mem_size) ? *(uint32_t *)(mem + opt + 4u) : 0;
                uint32_t o8 = (opt && opt + 20u < guest_mem_size) ? *(uint32_t *)(mem + opt + 8u) : 0;
                fprintf(stderr, "raceCarCtor this=0x%08x carIdx=%d opt=0x%08x [%08x %08x %08x] tint=%d\n",
                        c->r0, (int32_t)c->r1, opt, o0, o4, o8, (int32_t)c->r3);
                rc_logs++;
            }
        }
        return 0;
    }
    if (target == 0x2d3280u) {
        /* RaceCar::SetCarColor */
        if (tex_trace) {
            static int cc_logs = 0;
            if (cc_logs < 20) {
                fprintf(stderr, "setCarColor this=0x%08x a1=%d a2=%d a3=%d\n",
                        c->r0, (int32_t)c->r1, (int32_t)c->r2, (int32_t)c->r3);
                cc_logs++;
            }
        }
        return 0;
    }
    if (target == 0x2d36c8u) {
        /* RaceCar::InitBodyTexture: trace the driver-type virtual it queries (0x6c). The shipped
           COpenGLES2Driver::getDriverType() returns 8; the engine only tests "!= 0", so the path
           is taken unmodified. */
        uint32_t game = (0x7f980cu + 4u < guest_mem_size) ? *(uint32_t *)(mem + 0x7f980cu) : 0;
        uint32_t cfg = (game && game + 0x110u < guest_mem_size) ? *(uint32_t *)(mem + game + 0x10cu) : 0;
        uint32_t obj = (cfg && cfg + 0x14u < guest_mem_size) ? *(uint32_t *)(mem + cfg + 0x10u) : 0;
        uint32_t vt = (obj && obj + 4u < guest_mem_size) ? *(uint32_t *)(mem + obj) : 0;
        uint32_t fn = (vt && vt + 0x70u < guest_mem_size) ? *(uint32_t *)(mem + vt + 0x6cu) : 0;
        if (tex_trace) {
            static int ib_logs = 0;
            if (ib_logs < 10) {
                fprintf(stderr, "initBodyTexture car=0x%08x lr=0x%08x game=0x%08x cfg=0x%08x obj=0x%08x vt=0x%08x driverTypeFn=0x%08x\n",
                        c->r0, c->lr, game, cfg, obj, vt, fn);
                ib_logs++;
            }
        }
        return 0;
    }
    if (target == 0x2dd140u) {
        /* RaceCar::SetPVRBodyMatParams(mesh, mask, color1, color2) */
        if (tex_trace) {
            static int pb_logs = 0;
            if (pb_logs < 30) {
                fprintf(stderr, "setPVRBodyMat car=0x%08x meshptr=0x%08x maskptr=0x%08x c1=0x%08x c2=0x%08x\n",
                        c->r0, c->r1, c->r2, c->r3, *(uint32_t *)(mem + c->sp));
                pb_logs++;
            }
        }
        return 0;
    }
    if (target == 0x534fccu) {
        if (tex_trace) {
            static int ph_logs = 0;
            static int ph_skipped = 0;
            if (ph_logs < 100) {
                fprintf(stderr, "getPlaceHolder: kind=%u type=%u\n", c->r1, c->r2 & 0xffffu);
                ph_logs++;
            } else {
                ph_skipped++;
            }
        }
        fn_534fcc(c, mem);
        return 1;
    }
    if (target == 0x521524u) {
        /* CMaterialRenderer::getParameterID(type, start) - log the lookup arguments and the
         * renderer's def type fields so the miss cause is measurable. */
        if (tex_trace) {
            static int gp_logs = 0;
            if (gp_logs < 120) {
                uint32_t ren = c->r0, want = c->r1, start = c->r2;
                uint32_t dc = (ren && ren + 0x24u < guest_mem_size) ? *(uint16_t *)(mem + ren + 0xeu) : 0;
                uint32_t defs = (ren && ren + 0x24u < guest_mem_size) ? *(uint32_t *)(mem + ren + 0x20u) : 0;
                fprintf(stderr, "getParamID lr=0x%06x ren=0x%08x want=%u start=%u defCnt=%u defTypes:", c->lr, ren, want, start & 0xffffu, dc);
                for (uint32_t k = 0; k < dc && k < 10 && defs; k++) {
                    uint32_t dp = defs + k * 0x10u;
                    if (dp + 0x10u >= guest_mem_size) break;
                    fprintf(stderr, "{%u,%u}", *(uint16_t *)(mem + dp + 4u), mem[dp + 6u]);
                }
                fprintf(stderr, "\n");
                gp_logs++;
            }
        }
        return 0;
    }
    if (target == 0x517668u) {
        if (tex_trace) {
            static int ma_logs = 0;
            if (ma_logs < 120) {
                uint32_t ren = c->r1;
                uint32_t dc = (ren && ren + 0x24u < guest_mem_size) ? *(uint16_t *)(mem + ren + 0xeu) : 0;
                uint32_t defs = (ren && ren + 0x24u < guest_mem_size) ? *(uint32_t *)(mem + ren + 0x20u) : 0;
                fprintf(stderr, "matAlloc ren=0x%08x defCnt=%u defs:", ren, dc);
                for (uint32_t k = 0; k < dc && k < 6 && defs; k++) {
                    uint32_t sd = *(uint32_t *)(mem + defs + k * 0x10u);
                    char dnb[64] = {0};
                    if (sd && sd + 8u < guest_mem_size) {
                        const char *dn = (const char *)(mem + sd + 4u);
                        uint32_t m = 0;
                        while (m + 1 < sizeof dnb && dn[m]) { dnb[m] = dn[m]; m++; }
                    }
                    fprintf(stderr, " [%u:'%s']", k, dnb);
                }
                fprintf(stderr, "\n");
                ma_logs++;
            }
        }
        return 0;
    }
    if (target == 0x420e0cu) {
        /* BaseState::StateCreate() */
        uint32_t st = c->r0;
        uint32_t vt = (st && st + 4u < guest_mem_size) ? *(uint32_t *)(mem + st) : 0u;
        fast5_log_note("state create  this=0x%08x vt=0x%08x", st, vt);
        return 0;
    }
    if (target == 0x420ea8u) {
        /* BaseState::StateDestroy() */
        uint32_t st = c->r0;
        uint32_t vt = (st && st + 4u < guest_mem_size) ? *(uint32_t *)(mem + st) : 0u;
        fast5_log_note("state destroy this=0x%08x vt=0x%08x", st, vt);
        return 0;
    }
    if (target == 0x420f64u) {
        /* BaseState::OnMenuReset(): reached when a state returns to the menu. */
        uint32_t st = c->r0;
        uint32_t vt = (st && st + 4u < guest_mem_size) ? *(uint32_t *)(mem + st) : 0u;
        fast5_log_note("state menuReset this=0x%08x vt=0x%08x lr=0x%08x", st, vt, c->lr);
        return 0;
    }
    if (target == 0x42182cu) {
        /* BaseState::Unload3D(): the scene teardown that runs on the way back to the menu. */
        uint32_t st = c->r0;
        uint32_t vt = (st && st + 4u < guest_mem_size) ? *(uint32_t *)(mem + st) : 0u;
        fast5_log_note("state unload3D this=0x%08x vt=0x%08x lr=0x%08x", st, vt, c->lr);
        return 0;
    }
    if (target == 0x2b19acu) {
        /* StateStack::PushState(stack, BaseState*) */
        uint32_t stack = c->r0, state = c->r1;
        fn_2b19ac(c, mem);
        fast5_log_note("STATE push stack=0x%08x state=0x%08x vt=0x%08x lr=0x%08x",
                       stack, state,
                       (state && state + 4u < guest_mem_size) ? *(uint32_t *)(mem + state) : 0u,
                       c->lr);
        return 1;
    }
    if (target == 0x2b1a34u) {
        /* StateStack::PopState(stack, bool) */
        uint32_t stack = c->r0, pop_all = c->r1;
        uint32_t cur = 0;
        if (stack && stack + 0x2cu < guest_mem_size) cur = *(uint32_t *)(mem + stack + 0x28u);
        fn_2b1a34(c, mem);
        fast5_log_note("STATE pop  stack=0x%08x all=%u end=0x%08x ret=%u lr=0x%08x",
                       stack, pop_all, cur, c->r0, c->lr);
        return 1;
    }
    if (target == 0x2b1af8u) {
        uint32_t stack = c->r0;
        fn_2b1af8(c, mem);
        fast5_log_note("STATE pop_all stack=0x%08x lr=0x%08x", stack, c->lr);
        return 1;
    }
    if (target == 0x2b18b8u) {
        uint32_t stack = c->r0;
        fn_2b18b8(c, mem);
        fast5_log_note("STATE closeToMenu stack=0x%08x lr=0x%08x", stack, c->lr);
        return 1;
    }
    if (target == 0x4c0b70u) {
        /* CCinematicCommand::gogo(this, a1, a2, a3): dispatch by command name.
           The cinematic XML attributes feed the object index; a failed parse leaves it at -1. */
        if (race_trace) {
            static int cg_logs = 0;
            if (cg_logs < 400) {
                uint32_t cmd = c->r0;
                uint32_t namep = (cmd && cmd + 8u < guest_mem_size) ? *(uint32_t *)(mem + cmd + 4u) : 0;
                char nm[64] = {0};
                if (namep) guest_cstr(mem, namep, nm, sizeof nm);
                fprintf(stderr, "cineCmd n=%d name='%s' objIdx=%d a2=%d a3=%d\n",
                        cg_logs + 1, nm, (int32_t)c->r1, (int32_t)c->r2, (int32_t)c->r3);
                cg_logs++;
            }
        }
        return 0;
    }
    if (target == 0x2b8080u) {
        /* glitch::core::quaternion::fromAngleAxis(out, angle, axis): RaceCar::UpdateMeshes
           feeds the accumulated wheel angles car+0x48/+0x4c through it. */
        uint32_t out = c->r0, axis = c->r2, lr = c->lr;
        float angle;
        memcpy(&angle, &c->r1, 4);
        fn_2b8080(c, mem);
        if (!wheel_trace) return 1;
        uint32_t lr_elf = lr >= 0x10000u ? lr - 0x10000u : lr;
        if (lr_elf >= 0x2d29e0u && lr_elf < 0x2d2a60u) {
            static int fa_logs = 0;
            if (fa_logs < 40 || (fa_logs % 200) == 0) {
                float ax = 0, ay = 0, az = 0;
                if (axis && axis + 12u < guest_mem_size) {
                    ax = *(float *)(mem + axis);
                    ay = *(float *)(mem + axis + 4u);
                    az = *(float *)(mem + axis + 8u);
                }
                float q0 = 0, q1 = 0, q2 = 0, q3 = 0;
                if (out && out + 16u < guest_mem_size) {
                    q0 = *(float *)(mem + out);
                    q1 = *(float *)(mem + out + 4u);
                    q2 = *(float *)(mem + out + 8u);
                    q3 = *(float *)(mem + out + 12u);
                }
                fprintf(stderr, "fromAngleAxis n=%d lr=0x%06x out=0x%08x ang=%.5f axis=(%.2f %.2f %.2f) q=(%.4f %.4f %.4f %.4f)\n",
                        fa_logs + 1, lr_elf, out, angle, ax, ay, az, q0, q1, q2, q3);
            }
            fa_logs++;
        }
        return 1;
    }
    if (target == 0x4dc320u) {
        /* ISceneNode::setRotation: record the quaternion actually applied to a wheel node. */
        uint32_t node = c->r0, qp = c->r1;
        fn_4dc320(c, mem);
        if (!wheel_trace) return 1;
        if (g_wheel_node_count > 8 && node == g_wheel_nodes[8]) {
            static int sample_logs = 0;
            sample_logs++;
            if (sample_logs <= 5 || (sample_logs % 20) == 0) {
                float q0 = 0, q1 = 0, q2 = 0, q3 = 0;
                if (qp && qp + 16u < guest_mem_size) {
                    q0 = *(float *)(mem + qp);
                    q1 = *(float *)(mem + qp + 4u);
                    q2 = *(float *)(mem + qp + 8u);
                    q3 = *(float *)(mem + qp + 12u);
                }
                fprintf(stderr, "wheelSetRot sample=%d node=0x%08x q=(%.4f %.4f %.4f %.4f)\n",
                        sample_logs, node, q0, q1, q2, q3);
            }
        }
        return 1;
    }
    if (target == 0x2ca50cu) {
        /* MenuScene::Update(delta) - the per-frame driver that ends with MenuScene::UpdateCars.
           The wheel fields live in each RaceCar (scene+0x60..0x68), not in the scene itself. */
        if (!wheel_trace) return 0;
        static int mu_total = 0;
        mu_total++;
        if (mu_total <= 60 || (mu_total % 600) == 0) {
            uint32_t sc = c->r0;
            fprintf(stderr, "menuUpdate n=%d this=0x%08x lr=0x%06x", mu_total, sc, c->lr);
            if (sc && sc + 0x130u < guest_mem_size) {
                fprintf(stderr, " f54=%.3f f58=%.3f u5c=%u",
                        *(float *)(mem + sc + 0x54u), *(float *)(mem + sc + 0x58u),
                        *(uint32_t *)(mem + sc + 0x5cu));
                if (g_wheel_node_count == 0) {
                    for (int k = 0; k < 3; k++) {
                        uint32_t car = *(uint32_t *)(mem + sc + 0x60u + (uint32_t)k * 4u);
                        if (!car || car + 0x1160u >= guest_mem_size) continue;
                        for (int j = 0; j < 8; j++) {
                            uint32_t n = *(uint32_t *)(mem + car + 0x1140u + (uint32_t)j * 4u);
                            if (n && n < guest_mem_size && g_wheel_node_count < 24) {
                                g_wheel_nodes[g_wheel_node_count++] = n;
                            }
                        }
                    }
                    fprintf(stderr, " wheelNodes=%d", g_wheel_node_count);
                }
                for (int k = 0; k < 3; k++) {
                    uint32_t car = *(uint32_t *)(mem + sc + 0x60u + (uint32_t)k * 4u);
                    if (car && car + 0x60u < guest_mem_size) {
                        fprintf(stderr, " car%d=0x%08x rate=(%.2f,%.2f) ang=(%.5f,%.5f) lod=%d vis=%u",
                                k, car, *(float *)(mem + car + 0x40u), *(float *)(mem + car + 0x44u),
                                *(float *)(mem + car + 0x48u), *(float *)(mem + car + 0x4cu),
                                (int32_t)*(uint32_t *)(mem + car + 0x58u), mem[car + 0x60u]);
                    } else {
                        fprintf(stderr, " car%d=0x%08x", k, car);
                    }
                }
            }
            fprintf(stderr, "\n");
        }
        return 0;
    }
    if (target == 0x2caff8u) {
        /* MenuScene::UpdateCars(delta): log each car pointer and its LOD (car+0x58). */
        if (!wheel_trace) return 0;
        static int uc_logs = 0;
        if (uc_logs < 160) {
            uint32_t scene = c->r0;
            if (scene && scene + 0x70u < guest_mem_size) {
                uint32_t c0 = *(uint32_t *)(mem + scene + 0x60u);
                uint32_t c1 = *(uint32_t *)(mem + scene + 0x64u);
                uint32_t c2 = *(uint32_t *)(mem + scene + 0x68u);
                int l0 = (c0 && c0 + 0x60u < guest_mem_size) ? (int) * (uint32_t *)(mem + c0 + 0x58u) : -999;
                int l1 = (c1 && c1 + 0x60u < guest_mem_size) ? (int) * (uint32_t *)(mem + c1 + 0x58u) : -999;
                int l2 = (c2 && c2 + 0x60u < guest_mem_size) ? (int) * (uint32_t *)(mem + c2 + 0x58u) : -999;
                fprintf(stderr, "updateCars n=%d delta=%d cars=[0x%08x l%dd,0x%08x l%dd,0x%08x l%dd]\n",
                        uc_logs + 1, (int32_t)c->r1, c0, l0, c1, l1, c2, l2);
                uc_logs++;
            }
        }
        return 0;
    }
    if (target == 0x483accu) {
        /* BaseCar::UpdateCar: accumulates the visual wheel angles car+0x48/+0x4c from the
           wheel rates car+0x44/+0x40, gated on Game::s_pInstance+0x1b3 == 0. */
        if (!wheel_trace) return 0;
        static int bc_total = 0;
        bc_total++;
        if (bc_total <= 40 || (bc_total % 600) == 0) {
            uint32_t car = c->r0;
            uint32_t game = (0x7f980cu + 4u < guest_mem_size) ? *(uint32_t *)(mem + 0x7f980cu) : 0;
            uint32_t gate = (game && game + 0x1b4u < guest_mem_size) ? mem[game + 0x1b3u] : 0xdeadbeefu;
            fprintf(stderr, "baseCarUpdate n=%d car=0x%08x delta=%d gate1b3=%u", bc_total, car,
                    (int32_t)c->r1, gate);
            if (car && car + 0x60u < guest_mem_size) {
                fprintf(stderr, " rate=(%.2f,%.2f) ang=(%.5f,%.5f) lod=%d",
                        *(float *)(mem + car + 0x40u), *(float *)(mem + car + 0x44u),
                        *(float *)(mem + car + 0x48u), *(float *)(mem + car + 0x4cu),
                        (int32_t)*(uint32_t *)(mem + car + 0x58u));
            }
            fprintf(stderr, "\n");
        }
        return 0;
    }
    if (target == 0x2cb430u) {
        /* MenuScene::UpdateCarsWheelSpeed(idx): 20.0f wheel rate only when menuScene+0x5c != 0. */
        if (tex_trace) {
            static int ws_logs = 0;
            if (ws_logs < 60) {
                uint32_t scene = c->r0;
                uint32_t idx = c->r1;
                uint32_t flag = (scene && scene + 0x60u < guest_mem_size) ? (uint32_t)mem[scene + 0x5cu] : 0xdeadbeefu;
                fprintf(stderr, "wheelSpeed idx=%d scene=0x%08x flag5c=%u\n", (int32_t)idx, scene, flag);
                ws_logs++;
            }
        }
        return 0;
    }
    if (target == 0x2ca6f0u) {
        if (tex_trace) {
            static int ar_logs = 0;
            if (ar_logs < 20) {
                fprintf(stderr, "animateRollingRoad scene=0x%08x on=%d\n", c->r0, (int32_t)c->r1);
                ar_logs++;
            }
        }
        return 0;
    }
    if (target == 0x2d1c88u) {
        /* RaceCar::UpdateFakeWheelRotation(delta) */
        if (tex_trace) {
            static int wr_logs = 0;
            if (wr_logs < 30) {
                fprintf(stderr, "wheelRot car=0x%08x delta=%d\n", c->r0, (int32_t)c->r1);
                wr_logs++;
            }
        }
        return 0;
    }
    if (target == 0x52add8u) {
        if (tex_trace) {
            static int bmr_logs = 0;
            char mname[128] = {0};
            if (c->r1) guest_cstr(mem, c->r1, mname, sizeof mname);
            if (strncmp(mname, "00000000000000000000", 20) == 0) {
                static int z_logs2 = 0;
                if (z_logs2 < 40) {
                    fprintf(stderr, "beginMatRenderZERO lr=0x%06x name='%s' r1=0x%08x\n", c->lr, mname, c->r1);
                    z_logs2++;
                }
            } else if (bmr_logs < 300) {
                fprintf(stderr, "beginMatRender name='%s' r2=%u\n", mname, c->r2);
                bmr_logs++;
            }
        }
        return 0;
    }
    if (target == 0x58cc48u) {
        if (tex_trace) {
            static int fct_logs = 0;
            if (fct_logs < 6) {
                fprintf(stderr, "factoryMat r0=%08x r1=%08x r2=%08x r3=%08x sp=%08x stk:",
                        c->r0, c->r1, c->r2, c->r3, c->sp);
                for (int k = 0; k < 6; k++) {
                    uint32_t a = c->sp + (uint32_t)k * 4u;
                    if (a + 4u < guest_mem_size) {
                        uint32_t v = *(uint32_t *)(mem + a);
                        char s[48] = {0};
                        if (v) guest_cstr(mem, v, s, sizeof s);
                        fprintf(stderr, " %08x(%s)", v, s);
                    }
                }
                fprintf(stderr, "\n");
                fct_logs++;
            }
        }
        return 0;
    }
    if (target == 0x58c0b8u) {
        if (tex_trace) {
            static int cmat_logs = 0;
            if (cmat_logs < 80) {
                /* createMaterial(out, db, driver, renderer, [sp]SMaterial*, [sp+4]scene) */
                uint32_t smat = (c->sp + 8u < guest_mem_size) ? *(uint32_t *)(mem + c->sp) : 0;
                uint32_t pcount = (smat && smat + 0x18u < guest_mem_size) ? *(uint32_t *)(mem + smat + 0x10u) : 0;
                uint32_t parr = (smat && smat + 0x18u < guest_mem_size) ? *(uint32_t *)(mem + smat + 0x14u) : 0;
                fprintf(stderr, "createMaterial smat=0x%08x pcount=%u parr=0x%08x:", smat, pcount, parr);
                for (uint32_t k = 0; k < pcount && k < 4; k++) {
                    uint32_t pr = parr + k * 0x18u;
                    if (pr + 0x18u >= guest_mem_size) break;
                    fprintf(stderr, " rec[%u]={%08x %08x %08x %08x %08x %08x}", k,
                            *(uint32_t *)(mem + pr), *(uint32_t *)(mem + pr + 4u),
                            *(uint32_t *)(mem + pr + 8u), *(uint32_t *)(mem + pr + 12u),
                            *(uint32_t *)(mem + pr + 16u), *(uint32_t *)(mem + pr + 20u));
                }
                fprintf(stderr, "\n");
                cmat_logs++;
            }
        }
        return 0;
    }
    if (target == 0x58ec70u) {
        if (tex_trace) {
            static int cm_logs = 0;
            if (cm_logs < 40) {
                char mname[128] = {0};
                if (c->r3) guest_cstr(mem, c->r3, mname, sizeof mname);
                uint32_t list = c->r4; /* SEffectList* is the 4th arg (r3 was name; list in r4? logged below) */
                fprintf(stderr, "matRender name=%s r0=%08x r1=%08x r2=%08x r3=%08x r4=%08x r5=%08x\n",
                        mname, c->r0, c->r1, c->r2, c->r3, c->r4, c->r5);
                /* SEffectList arg order: (out, db, driver, name, effects, scene) -> effects in r4 */
                if (list && list + 0x10u < guest_mem_size) {
                    for (uint32_t e = *(uint32_t *)(mem + list), n = 0; e && e != list && n < 8;
                         e = *(uint32_t *)(mem + e), n++) {
                        uint32_t eff = *(uint32_t *)(mem + e + 0x10u);
                        if (!eff || eff + 0x30u >= guest_mem_size) break;
                        fprintf(stderr, "  effect[%u] techCnt=%u techArr=0x%08x paramCnt=%u paramArr=0x%08x\n", n,
                                *(uint32_t *)(mem + eff + 0x20u), *(uint32_t *)(mem + eff + 0x24u),
                                *(uint32_t *)(mem + eff + 0x28u), *(uint32_t *)(mem + eff + 0x2cu));
                    }
                }
                cm_logs++;
            }
        }
        return 0;
    }
    if (target == 0x4a00e4u) {
        if (tex_trace) {
            static int ap_logs = 0;
            if (ap_logs < 300) {
                uint32_t pidx = c->r1;
                uint32_t precrec = c->r2;
                char pname[96] = {0};
                uint32_t nptr = (precrec && precrec + 0x18u < guest_mem_size) ? *(uint32_t *)(mem + precrec) : 0;
                if (nptr) guest_cstr(mem, nptr, pname, sizeof pname);
                uint32_t ptype = (precrec && precrec + 8u < guest_mem_size) ? mem[precrec + 4u] : 0;
                uint32_t cnt = 0;
                if (precrec && precrec + 0x14u < guest_mem_size) {
                    uint32_t cp = *(uint32_t *)(mem + precrec + 0x10u);
                    if (cp && cp + 4u < guest_mem_size) cnt = *(uint32_t *)(mem + cp);
                }
                fprintf(stderr, "applyParam: idx=%u scene('%s') type=%u count=%u rec=0x%08x\n",
                        pidx, pname, ptype, cnt, precrec);
                ap_logs++;
            }
        }
        return 0;
    }
    if (target == 0x62abd8u) {
        uint32_t tex = c->r2;
        char name[160] = {0};
        if (tex && tex + 12u < guest_mem_size) {
            uint32_t np = *(uint32_t *)(mem + tex + 8u);
            guest_cstr(mem, np, name, sizeof name);
        }
        static const char *watch = NULL;
        static int watch_init;
        if (!watch_init) { watch = getenv("FAST5_WATCH_TEX"); watch_init = 1; }
        int watched = watch && *watch && name[0] && strstr(name, watch) != NULL;
        uint32_t gl_before = (tex && tex + 0x44u < guest_mem_size)
                             ? *(uint32_t *)(mem + tex + 0x40u) : 0xdeadbeefu;
        gl_bridge_note_texture(tex);
        if (tex_trace) {
            static int st_logs = 0;
            if (st_logs < 500) {
                uint32_t drv = c->r0;
                uint32_t curmat = (drv && drv + 0xc8u < guest_mem_size) ? *(uint32_t *)(mem + drv + 0xc8u) : 0;
                fprintf(stderr, "setTexture: unit=%u tex=0x%08x type=%u lr=0x%06x mat=0x%08x name=%s\n",
                        c->r1, tex, c->r3 & 3u, c->lr, curmat, name);
                st_logs++;
            }
        }
        fn_62abd8(c, mem);
        if (watched) {
            uint32_t gl_after = (tex && tex + 0x44u < guest_mem_size)
                                ? *(uint32_t *)(mem + tex + 0x40u) : 0xdeadbeefu;
            fprintf(stderr,
                    "watchSetTexture: tex=0x%08x name=%s unit=%u type=%u lr=0x%06x gl=%u->%u\n",
                    tex, name, c->r1, c->r3 & 3u, c->lr, gl_before, gl_after);
        }
        return 1;
    }
    if (target == 0x504764u) {
        static int trace = -1;
        if (trace < 0) trace = getenv("FAST5_TEXTURE_TRACE") != NULL;
        if (trace) {
            char name[512];
            char prefix[128];
            if (guest_cstr(mem, c->r1, name, sizeof name)) {
                uint32_t p = c->r0 + 0x30u;
                uint32_t rep = p < guest_mem_size ? *(uint32_t *)(mem + p) : 0;
                prefix[0] = 0;
                if (rep && rep + 12u < guest_mem_size) {
                    guest_cstr(mem, rep + 12u, prefix, sizeof prefix);
                }
                fprintf(stderr, "openFile: prefix=%s name=%s\n", prefix, name);
            }
        }
        return 0;
    }
    if (target == 0x619238u || target == 0x619170u || target == 0x61a260u) {
        static int trace = -1;
        if (trace < 0) trace = getenv("FAST5_TEXTURE_TRACE") != NULL;
        if (trace) {
            char name[512];
            if (guest_cstr(mem, c->r1, name, sizeof name)) {
                fprintf(stderr, "file open: %s\n", name);
            }
        }
        alias_guest_name(c, mem, target == 0x619238u ? "open" :
                         target == 0x61a260u ? "exists" : "archives");
        return 0;
    }
    if (target == 0x29a0b4u) {
        /* nativeCanAndroidPhonesUsingMenu3D -> the port renders 3D menus. */
        c->r0 = 1;
        return 1;
    }
    /* Java callbacks. Each of these ends in NVThreadGetCurrentJNIEnv() and then a JNIEnv vtable
     * call. The port has no live JavaVM (U11), so the uninitialized env reads the ELF header
     * (0x464c457f) and the next call faults at 0x464c47xx. They are answered on the host with the
     * value the Java side produced on the reference device (or a no-op where the behaviour is
     * purely Java-side). Verified by call site:
     *   - nativeGC (0x29a0e4): System.gc(), reached from TrackScene teardown when leaving a race.
     *   - AndroidOS_IsFinishLoadBackground (0x29be34): background loader never pending -> finished.
     *   - nativeSendAppToBackground (0x29a6b4), nativeIsNotCallSendApp (0x29a7b4): no-op / false.
     *   - nativeOpenGLLive (0x29a590), nativeOpenIGP (0x29a704), nativeNotifyTrophy (0x29a75c),
     *     AndroidOS_LoadSoundGroup (0x29be64), nativeResumeSoundEngine (0x29beac),
     *     nativesetfxMusicRes (0x29bedc), nativeGetHostName (0x29a22c): Java/network-only. */
    if (target == 0x29a0e4u || target == 0x29be34u) {
        c->r0 = 1; /* GC done / load finished */
        return 1;
    }
    if (target == 0x29a7e4u) {
        /* nativeExit(): the game asks Java to close the activity. There is no Java side on
         * the host, so the call is a no-op — but the exported function must not run, because
         * it dereferences the (uninitialised) JNIEnv and faults at 0x464c47xx. */
        return 1;
    }
    if (target == 0x29a6b4u || target == 0x29a590u || target == 0x29a704u ||
        target == 0x29a75cu || target == 0x29beacu ||
        target == 0x29bedcu) {
        return 1; /* no-op Java calls */
    }
    if (target == 0x29a7b4u) {
        c->r0 = 0; /* nativeIsNotCallSendApp() -> false */
        return 1;
    }
    if (target == 0x29a22cu) {
        /* nativeGetHostName() returns a malloc'd string; give it a valid empty one so callers
           never receive NULL. */
        c->r0 = 0x9A0600u;
        mem[0x9A0600u] = 0;
        return 1;
    }
    if ((target == 0x520a20u || target == 0x5204a0u) && c->sp + 28u < guest_mem_size) {
        static int cmr_logs2 = 0;
        if (tex_trace) {
            static int cmr_logs = 0;
            if (cmr_logs < 12) {
                fprintf(stderr,
                        "cmr-ctor(%s) r0=%08x r1=%08x r2=%08x r3=%08x stack: "
                        "%08x %08x %08x %08x %08x %08x %08x\n",
                        target == 0x520a20u ? "a" : "b",
                        c->r0, c->r1, c->r2, c->r3,
                        *(uint32_t *)(mem + c->sp + 0u), *(uint32_t *)(mem + c->sp + 4u),
                        *(uint32_t *)(mem + c->sp + 8u), *(uint32_t *)(mem + c->sp + 12u),
                        *(uint32_t *)(mem + c->sp + 16u), *(uint32_t *)(mem + c->sp + 20u),
                        *(uint32_t *)(mem + c->sp + 24u));
                cmr_logs++;
            }
        }
        uint32_t list = *(uint32_t *)(mem + c->sp);
        uint32_t count = *(uint32_t *)(mem + c->sp + 20u) & 0xffffu;
        uint32_t ids = *(uint32_t *)(mem + c->sp + 24u);
        if (tex_trace && cmr_logs2 < 80 && ids && ids + 64u < guest_mem_size && count) {
            fprintf(stderr, "cmr-raw(%s)[%u] count=%u ids=0x%08x:", target == 0x520a20u ? "a" : "b",
                    cmr_logs2, count, ids);
            for (uint32_t k = 0; k < 24u && k < count; k++) {
                fprintf(stderr, " %04x", *(uint16_t *)(mem + ids + k * 2u));
            }
            fprintf(stderr, "\n");
            cmr_logs2++;
        }
        if (tex_trace && cmr_logs2 <= 14) {
            uint32_t def_cnt = *(uint32_t *)(mem + c->sp + 8u) & 0xffffu;
            uint32_t def_arr = *(uint32_t *)(mem + c->sp + 12u);
            if (def_cnt && def_arr && def_arr + def_cnt * 4u < guest_mem_size) {
                fprintf(stderr, "cmr-defs[%u]: n=%u:", cmr_logs2, def_cnt);
                for (uint32_t k = 0; k < def_cnt && k < 10; k++) {
                    uint32_t dp = *(uint32_t *)(mem + def_arr + k * 4u);
                    if (!dp || dp + 8u >= guest_mem_size) { fprintf(stderr, " [bad]"); break; }
                    fprintf(stderr, " {cls=%04x type=%02x}", *(uint16_t *)(mem + dp + 4u),
                            mem[dp + 6u]);
                }
                fprintf(stderr, "\n");
            }
        }
    }
    if (target == 0x637368u) {
        gl_bridge_note_texture(c->r0);
        fn_637368(c, mem);
        return 1;
    }
    if (target == 0x629760u) {
        uint32_t sh = c->r0;
        fn_629760(c, mem);
        if (tex_trace && sh && sh + 0x40u < guest_mem_size) {
            static int lp_logs = 0;
            static int lp_rec_logs = 0;
            uint32_t auto_cnt = *(uint16_t *)(mem + sh + 0x18u);
            uint32_t total = *(uint16_t *)(mem + sh + 0x1au);
            uint32_t fs_start = *(uint16_t *)(mem + sh + 0x20u);
            uint32_t fs_end = *(uint16_t *)(mem + sh + 0x22u);
            int interesting = total >= 6u || fs_end != 0u;
            if (lp_logs < 400 && (interesting || lp_logs < 10)) {
                fprintf(stderr, "linkProgram: sh=0x%08x auto=%u total=%u fragRange=[%u,%u)\n",
                        sh, auto_cnt, total, fs_start, fs_end);
                lp_logs++;
                uint32_t arr = *(uint32_t *)(mem + sh + 0x14u);
                if (interesting && arr && arr + 0x10u < guest_mem_size && lp_rec_logs < 30) {
                    lp_rec_logs++;
                    uint32_t n = total < 16 ? total : 16;
                    for (uint32_t i = 0; i < n; i++) {
                        uint32_t rec = arr + i * 0x10u;
                        uint32_t sdata = *(uint32_t *)(mem + rec);
                        uint32_t cls = *(uint16_t *)(mem + rec + 4u);
                        uint32_t type = *(uint8_t *)(mem + rec + 6u);
                        uint32_t asize = *(uint32_t *)(mem + rec + 8u);
                        uint32_t loc = *(uint32_t *)(mem + rec + 0xcu);
                        char nm[48] = {0};
                        if (sdata && sdata + 8u < guest_mem_size) {
                            for (int off = 4; off <= 8; off += 4) {
                                uint32_t p = sdata + (uint32_t)off;
                                if (p < guest_mem_size && mem[p] >= 0x20 && mem[p] < 0x7f) {
                                    guest_cstr(mem, p, nm, sizeof nm);
                                    if (nm[0]) break;
                                }
                            }
                        }
                        fprintf(stderr, "  urec[%2u] cls=0x%02x type=0x%02x size=%u loc=%d name=%s\n",
                                i, cls, type, asize, (int32_t)loc, nm);
                    }
                }
            }
        }
        return 1;
    }
    if (target == 0x62e300u) {
        if (tex_trace) {
            static int aux_logs = 0;
            static uint32_t aux_key = 0;
            uint32_t fbo = (uint32_t)gl_bridge_bound_fbo();
            uint32_t key = c->r1 ^ c->r2 ^ c->r3 ^ (fbo << 24);
            if (aux_logs < 4000 && key != aux_key && (fbo == 4u || fbo == 5u)) {
                aux_key = key;
                uint32_t start = c->r3;
                uint32_t end = (c->sp < guest_mem_size) ? *(uint32_t *)(mem + c->sp) : 0;
                uint32_t nb = (start <= end && end - start <= 64u) ? (end - start) / 4u : 99u;
                fprintf(stderr, "aux: shader=0x%08x mat=0x%08x fbo=%d start=0x%08x end=0x%08x n=%u\n",
                        c->r1, c->r2, (int32_t)fbo, start, end, nb);
                if (fbo == 4 || fbo == 5) {
                    fprintf(stderr, "  stack@sp:");
                    for (int k = -2; k < 6; k++) {
                        int32_t off = (int32_t)k * 4;
                        if (off >= 0 && (uint32_t)(c->sp + off) + 4u <= guest_mem_size) {
                            fprintf(stderr, " [%+d]=%08x", off, *(uint32_t *)(mem + c->sp + off));
                        }
                    }
                    fprintf(stderr, "  lr=0x%08x\n", c->lr);
                }
                if (start && nb >= 1u && nb <= 16u) {
                    for (uint32_t k = 0; k < nb; k++) {
                        uint32_t b = *(uint32_t *)(mem + start + k * 4u);
                        fprintf(stderr, "  gbind[%u] %04x %04x\n", k, b >> 16, b & 0xffffu);
                    }
                }
                aux_logs++;
            }
        }
        fn_62e300(c, mem);
        return 1;
    }
    if (target == 0x62b770u) {
        if (tex_trace) {
            uint32_t drv = c->r0;
            uint32_t mat = (drv && drv + 0xc8u < guest_mem_size) ? *(uint32_t *)(mem + drv + 0xc8u) : 0;
            if (mat == 0x08a9dee0u || mat == 0x08a10980u) {
                static int ip2_logs = 0;
                if (ip2_logs < 12) {
                    uint32_t r1 = c->r1, r2 = c->r2, r3 = c->r3;
                    uint32_t pass = 0, b = 0, c1 = 0, c2 = 0;
                    if (mat + 8u < guest_mem_size) {
                        uint32_t desc = *(uint32_t *)(mem + mat + 4u);
                        uint32_t A = (desc && desc + 0x18u < guest_mem_size) ? *(uint32_t *)(mem + desc + 0x18u) : 0;
                        uint32_t node = (A && A + 12u < guest_mem_size) ? *(uint32_t *)(mem + A + 8u) : 0;
                        if (node && node + 0x2cu < guest_mem_size) {
                            pass = node + (r2 & 0x3ffu) * 16u;
                            if (pass + 0x2cu < guest_mem_size) {
                                b = *(uint32_t *)(mem + pass + 0x24u);
                                c1 = *(uint16_t *)(mem + pass + 0x28u);
                                c2 = *(uint16_t *)(mem + pass + 0x2au);
                            }
                        }
                        fprintf(stderr, "indirect[%s]: mat=0x%08x r1=%08x r2=%08x r3=%08x node=%08x pass=%08x binds=%08x n1=%u n2=%u\n",
                                mat == 0x08a9dee0u ? "ENV" : "CAR", mat, r1, r2, r3, node, pass, b, c1, c2);
                    }
                    ip2_logs++;
                }
            }
        }
        fn_62b770(c, mem);
        return 1;
    }
    if (target == 0x51c260u || target == 0x523f04u || target == 0x525048u) {
        if (tex_trace) {
            static int sp2_logs = 0;
            if (sp2_logs < 500) {
                uint32_t imat = c->r0, pidx = c->r1 & 0xffffu, pel = c->r2;
                uint32_t ipp = c->r3;
                uint32_t tex = (ipp && ipp + 4u < guest_mem_size) ? *(uint32_t *)(mem + ipp) : 0;
                char nmm[96] = {0};
                if (tex && tex + 12u < guest_mem_size) {
                    uint32_t np = *(uint32_t *)(mem + tex + 8u);
                    guest_cstr(mem, np, nmm, sizeof nmm);
                }
                fprintf(stderr, "setTexParam2[%s]: mat=0x%08x idx=%u el=%u tex=0x%08x name=%s\n",
                        target == 0x51c260u ? "mat2" : (target == 0x523f04u ? "rend1" : "rend2"),
                        imat, pidx, pel, tex, nmm);
                sp2_logs++;
            }
        }
        if (target == 0x51c260u) fn_51c260(c, mem);
        else if (target == 0x523f04u) fn_523f04(c, mem);
        else fn_525048(c, mem);
        return 1;
    }
    if (target == 0x51ac28u) {
        if (tex_trace) {
            static int sp_logs = 0;
            if (sp_logs < 3000 || (c->lr >= 0x59c000u && c->lr < 0x59d000u)) {
                uint32_t imat = c->r0, pidx = c->r1 & 0xffffu, pel = c->r2;
                uint32_t ipp = c->r3;
                uint32_t tex = (ipp && ipp + 4u < guest_mem_size) ? *(uint32_t *)(mem + ipp) : 0;
                uint32_t dsc = (imat && imat + 8u < guest_mem_size) ? *(uint32_t *)(mem + imat + 4u) : 0;
                uint32_t defcnt = (dsc && dsc + 0x24u < guest_mem_size) ? *(uint16_t *)(mem + dsc + 0xeu) : 0;
                uint32_t def = 0, dtype = 0, dsize = 0;
                if (dsc && pidx < defcnt && dsc + 0x24u < guest_mem_size) {
                    uint32_t defs = *(uint32_t *)(mem + dsc + 0x20u);
                    def = defs + pidx * 0x10u;
                    if (def + 0x10u < guest_mem_size) {
                        dtype = mem[def + 6u];
                        dsize = *(uint32_t *)(mem + def + 8u);
                    }
                }
                uint32_t ttype = (tex && tex + 0x28u < guest_mem_size) ? (*(uint32_t *)(mem + tex + 0x24u) & 3u) : 0xffffffffu;
                if (c->lr >= 0x59c000u && c->lr < 0x59d000u) {
                    static int sc_logs = 0;
                    if (sc_logs < 200) {
                        char tn[96] = {0};
                        if (tex && tex + 12u < guest_mem_size) {
                            uint32_t tnp = *(uint32_t *)(mem + tex + 8u);
                            guest_cstr(mem, tnp, tn, sizeof tn);
                        }
                        char rn[96] = {0}, dn[96] = {0};
                        uint32_t rp = (imat && imat + 8u < guest_mem_size) ? *(uint32_t *)(mem + imat + 4u) : 0;
                        if (rp && rp + 12u < guest_mem_size) {
                            uint32_t rnp = *(uint32_t *)(mem + rp + 8u);
                            if (rnp) guest_cstr(mem, rnp, rn, sizeof rn);
                        }
                        if (dsc && pidx < defcnt) {
                            uint32_t defs2 = *(uint32_t *)(mem + dsc + 0x20u);
                            uint32_t sd2 = *(uint32_t *)(mem + defs2 + pidx * 0x10u);
                            if (sd2) guest_cstr(mem, sd2 + 4u, dn, sizeof dn);
                        }
                        fprintf(stderr, "sceneSetTex: mat=0x%08x rend='%s' param='%s' idx=%u tex=0x%08x name=%s\n",
                                imat, rn, dn, pidx, tex, tn);
                        sc_logs++;
                    }
                }
                fprintf(stderr, "setTexParam: lr=0x%06x mat=0x%08x idx=%u el=%u tex=0x%08x ttype=%u defType=%02x defSize=%u\n",
                        c->lr, imat, pidx, pel, tex, ttype, dtype, dsize);
                sp_logs++;
            }
        }
        fn_51ac28(c, mem);
        if (tex_trace) {
            static int sr_logs = 0;
            if (sr_logs < 60) {
                fprintf(stderr, "  -> ret=%u\n", c->r0);
                sr_logs++;
            }
        }
        return 1;
    }
    /*
     * Resolution trace (FAST5_RES_TRACE=1).
     *
     * The 2D layer and the 3D scene are laid out in different coordinate spaces, so every step
     * that decides "how big is the screen" is logged here:
     *   - the orientation hand-off (UI orientation -> driver orientation),
     *   - the driver viewport rect,
     *   - the destination rectangles the 2D layer asks the driver to fill.
     * The rect layout is {left, top, right, bottom} (glitch::core::rect<int>).
     */
    {
        static int res_trace = -1;
        if (res_trace < 0) res_trace = getenv("FAST5_RES_TRACE") != NULL;
        if (res_trace) {
            if (target == 0x2a0304u) {   /* Application::SetUIOrientation(this, orient, force) */
                fprintf(stderr, "res: SetUIOrientation orient=%d force=%d\n",
                        (int)c->r1, (int)c->r2);
            } else if (target == 0x2a036cu) { /* Application::ReadOrientation(x,y,z) in s0..s2 */
                float x, y, z;
                memcpy(&x, &c->vfp[0], 4);
                memcpy(&y, &c->vfp[1], 4);
                memcpy(&z, &c->vfp[2], 4);
                fprintf(stderr, "res: ReadOrientation x=%.3f y=%.3f z=%.3f\n", x, y, z);
            } else if (target == 0x2a8390u) { /* Game::OnOrientationChange(this, orient) */
                fprintf(stderr, "res: OnOrientationChange orient=%d\n", (int)c->r1);
            } else if (target == 0x567424u) { /* IVideoDriver::setOrientation(this, orient) */
                fprintf(stderr, "res: driver setOrientation orient=%d\n", (int)c->r1);
            } else if (target == 0x624bbcu) { /* IRenderTarget::setViewport(this, rect) */
                uint32_t p = c->r1;
                if (p && p + 0x10u < guest_mem_size) {
                    const int32_t *r = (const int32_t *)(mem + p);
                    fprintf(stderr, "res: setViewport %d,%d %dx%d\n",
                            r[0], r[1], r[2] - r[0], r[3] - r[1]);
                }
            } else if (target == 0x631da0u) { /* CCommonGLDriver::draw2DRectangle(dest, src, color, clip) */
                extern uint32_t g_host_frame;
                static int dr_logs;
                uint32_t p = c->r1;
                /* FAST5_RES_FRAME=<n> dumps the whole 2D layer of one frame, which is what
                 * makes the 2D layout comparable between resolutions. */
                static unsigned res_frame = 0xffffffffu;
                static int res_frame_parsed;
                if (!res_frame_parsed) {
                    res_frame_parsed = 1;
                    const char *e = getenv("FAST5_RES_FRAME");
                    if (e) res_frame = (unsigned)strtoul(e, NULL, 10);
                }
                int wanted = (res_frame != 0xffffffffu && g_host_frame == res_frame);
                int fbo = gl_bridge_bound_fbo();
                if ((wanted || dr_logs < 80 || fbo != 0) && p && p + 0x10u < guest_mem_size) {
                    const int32_t *r = (const int32_t *)(mem + p);
                    fprintf(stderr, "res: draw2D dest=%d,%d %dx%d fbo=%d\n",
                            r[0], r[1], r[2] - r[0], r[3] - r[1], fbo);
                    if (!wanted) dr_logs++;
                }
            }
        }
    }
    /*
     * Resolution presentation.
     *
     * The guest runs at the design resolution (800x480); the window can be any size. Two
     * engine entry points have to agree with the host's presentation:
     *
     *  - CCameraSceneNode::setAspectRatio(float): the scene data carries the aspect the
     *    game was composed for (5:3). On a wider window the camera would stretch the scene,
     *    so the aspect follows the framebuffer instead.
     *  - CCommonGLDriver::draw2DRectangle(rect, rect, color, rect): every 2D draw goes
     *    through here. The engine's 2D projection maps the design space onto the viewport
     *    with the projection factors, so in letterbox mode the destination rect has to be
     *    pre-scaled to land where the host wants it.
     */
    if (target == 0x4d8228u) { /* CCameraSceneNode::setAspectRatio(this, float aspect) */
        if (g_resolution.frame_w != g_resolution.design_w ||
            g_resolution.frame_h != g_resolution.design_h) {
            /* The soft-float ABI passes the float in a core register (r1), not in s0:
             * the translated body stores c->r1 into this+0x140. */
            float old_aspect;
            memcpy(&old_aspect, &c->r1, 4);
            float aspect = (float)((double)g_resolution.frame_w / (double)g_resolution.frame_h);
            memcpy(&c->r1, &aspect, 4);
            static int aspect_logs;
            if (aspect_logs < 8) {
                fprintf(stderr, "res: camera aspect %.4f -> %.4f\n",
                        (double)old_aspect, (double)aspect);
                aspect_logs++;
            }
        }
        return 0;   /* fall through to the translated body */
    }
    if (target == 0x29d504u) { /* the engine's "re-init the application" helper (writes this+0x224/+0x250) */
        extern int g_engine_native;
        fn_29d504(c, mem);
        uint32_t app = live_application(mem);
        g_application_ptr = app;
        if (g_engine_native && !g_ui_native_2d) {
            *(int32_t *)(mem + 0x7d7820u) = 800;
            *(int32_t *)(mem + 0x7d7824u) = 480;
            if (app) {
                *(int32_t *)(mem + app + 0x250u) = 400;
                *(int32_t *)(mem + app + 0x254u) = 240;
            }
        }
        return 1;
    }
    if (target == 0x29eaecu) { /* Application::Init(path, driver, ?, w, h, version) */
        /*
         * The engine owns its screen-size globals: `Application::Init` computes the logical
         * size from the screen it was given, and the application is destroyed and created
         * again during the boot (the Universal logo screen does that), which reset the
         * logical size to the native 960x540 and pushed the 2D layout out of the window.
         *
         * Re-apply the design-space conversion after every init.
         */
        fn_29eaec(c, mem);
        extern int g_engine_native;
        /*
         * The engine destroys and creates the application again during the boot, so the
         * live instance is read from the engine's own singleton rather than remembered.
         */
        uint32_t app = live_application(mem);
        g_application_ptr = app;
        if (g_engine_native && !g_ui_native_2d) {
            *(int32_t *)(mem + 0x7d7820u) = 800;   /* SCR_WIDTH  */
            *(int32_t *)(mem + 0x7d7824u) = 480;   /* SCR_HEIGHT */
            if (app) {
                *(int32_t *)(mem + app + 0x250u) = 400;  /* logical width  */
                *(int32_t *)(mem + app + 0x254u) = 240;  /* logical height */
            }
        }
        return 1;
    }
    if (target == 0x3c7748u) { /* CFont::DrawString(font, text, x, y, align, ...) */
        /*
         * The 2D text is positioned in the logical space (the app scale is applied by the
         * sprite layer). Log the requested position so the space can be verified.
         */
        static int font_logs;
        if (font_logs < 4 && c->sp && c->sp + 8u < guest_mem_size) {
            uint32_t align = *(uint32_t *)(mem + c->sp);
            char txt[64] = {0};
            if (c->r1 && c->r1 + 16u < guest_mem_size) {
                guest_cstr(mem, c->r1, txt, sizeof txt);
            }
            uint32_t app = live_application(mem);
            fprintf(stderr, "res: font x=%d y=%d align=0x%x app=0x%08x logical=%dx%d '%s'\n",
                    (int)(int32_t)c->r2, (int)(int32_t)c->r3, align, app,
                    app ? *(int32_t *)(mem + app + 0x250u) : -1,
                    app ? *(int32_t *)(mem + app + 0x254u) : -1,
                    txt);
            font_logs++;
        }
        return 0;
    }
    if (target == 0x4460f0u) { /* GS_UniversalLogo::StateRender() */
        /*
         * The Universal logo screen positions its legal text from the application's logical
         * size (Application+0x250/0x254). Log it: the text must land in the 2D design space.
         */
        extern uint32_t g_host_frame;
        static int logo_logs;
        if (logo_logs < 4) {
            uint32_t app = live_application(mem);
            if (app) {
                fprintf(stderr, "res: universal logo logical=%dx%d (frame %u)\n",
                        *(int32_t *)(mem + app + 0x250u), *(int32_t *)(mem + app + 0x254u),
                        g_host_frame);
            }
            logo_logs++;
        }
        return 0;
    }
    if (target == 0x3e6f80u) { /* CMenuManager::GetRayFromScreenCoordinates(ray*, this, pos*, camera) */
        /*
         * The touch reaches the game in the design space (the 2D layout and the 2D hit
         * rectangles live there), but the 3D picking works on the engine's own screen,
         * which is the window in native mode. Convert the position for the ray.
         *
         * The position2d<int> is passed by reference in r2 (r0 = the ray, r1 = this).
         */
        if (g_resolution.engine_native && g_resolution.sx != 1.0 &&
            c->r2 && c->r2 + 8u < guest_mem_size) {
            int32_t *pos = (int32_t *)(mem + c->r2);
            double fx, fy;
            fast5_resolution_to_frame((double)pos[0], (double)pos[1], &fx, &fy);
            static int ray_logs;
            if (ray_logs < 20) {
                fprintf(stderr, "res: ray %d,%d -> %.0f,%.0f\n", pos[0], pos[1], fx, fy);
                ray_logs++;
            }
            pos[0] = (int32_t)floor(fx + 0.5);
            pos[1] = (int32_t)floor(fy + 0.5);
        }
        return 0;
    }
    if (target == 0x632ca8u) { /* CCommonGLDriver::createRenderTarget(textureRef, index) */
        static int rt_logs;
        static int res_trace_draw = -1;
        if (res_trace_draw < 0) res_trace_draw = getenv("FAST5_RES_TRACE") != NULL;
        if (res_trace_draw && rt_logs < 40) {
            uint32_t tex = 0;
            if (c->r1 && c->r1 + 4u < guest_mem_size) tex = *(uint32_t *)(mem + c->r1);
            float w = 0.0f, h = 0.0f;
            if (tex && tex + 0x14u < guest_mem_size) {
                memcpy(&w, mem + tex + 0x0cu, 4);
                memcpy(&h, mem + tex + 0x10u, 4);
            }
            fprintf(stderr, "res: createRenderTarget tex=0x%08x size=%.0fx%.0f lr=0x%06x\n",
                    tex, (double)w, (double)h, c->lr);
            rt_logs++;
        }
        return 0;
    }
    if (target == 0x631da0u) { /* CCommonGLDriver::draw2DRectangle(dest, src, color, clip) */
        static int screen_draws;
        static int res_trace_draw = -1;
        if (res_trace_draw < 0) res_trace_draw = getenv("FAST5_RES_TRACE") != NULL;
        /*
         * Every 2D draw is authored in the game's design space (800x480).  The screen pass is
         * converted to the window with the one shared transform (runtime/resolution.c), so the
         * 2D layer, the pointer and the 3D picking can never drift apart.  `FAST5_UI_FIT`
         * chooses between one uniform scale (the artwork and the text keep their proportions)
         * and a per-axis stretch (the layer covers the whole window).
         *
         * Off-screen passes are left alone: the engine renders its 2D screens (the menu's RTT
         * screens) with the target's own projection, so their content is already in the
         * target's space and transforming it again would move it off its quads.
         *
         * The engine clips the destination against its own screen clip, which lives in the
         * design space, so the clip has to be converted together with the destination or the
         * draw is culled away.  The clip is persistent engine state, so it is restored right
         * after the draw.  If there is no usable clip the draw is left completely alone.
         */
        if (gl_bridge_bound_fbo() == 0) {
            uint32_t dest = c->r1;
            uint32_t clip = 0;
            if (c->sp && c->sp + 4u < guest_mem_size) {
                clip = *(uint32_t *)(mem + c->sp);
            }
            if (dest && dest + 0x10u < guest_mem_size &&
                clip && clip + 0x10u < guest_mem_size) {
                int32_t *r = (int32_t *)(mem + dest);
                int32_t *cr = (int32_t *)(mem + clip);
                int32_t saved[4];
                int x = r[0], y = r[1], w = r[2] - r[0], h = r[3] - r[1];
                int ox = x, oy = y, ow = w, oh = h;
                int cx, cy, cw, ch;
                saved[0] = cr[0]; saved[1] = cr[1];
                saved[2] = cr[2]; saved[3] = cr[3];
                fast5_resolution_fit_rect(&x, &y, &w, &h);
                cx = saved[0]; cy = saved[1];
                cw = saved[2] - saved[0]; ch = saved[3] - saved[1];
                fast5_resolution_fit_rect(&cx, &cy, &cw, &ch);
                r[0] = x; r[1] = y; r[2] = x + w; r[3] = y + h;
                cr[0] = cx; cr[1] = cy; cr[2] = cx + cw; cr[3] = cy + ch;
                screen_draws++;
                if (screen_draws < 4 || (res_trace_draw && screen_draws < 40)) {
                    fprintf(stderr, "res: draw2D %d,%d %dx%d -> %d,%d %dx%d\n",
                            ox, oy, ow, oh, x, y, w, h);
                }
                fn_631da0(c, mem);
                cr[0] = saved[0]; cr[1] = saved[1];
                cr[2] = saved[2]; cr[3] = saved[3];
                return 1;
            }
        }
        return 0;
    }
    (void)c;
    (void)mem;
    return 0;
}
