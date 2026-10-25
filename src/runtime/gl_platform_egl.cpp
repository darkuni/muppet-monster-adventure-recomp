// Headless OpenGL 3.3 core context via EGL (Linux). Used for automated runs
// and screenshots; the Windows build uses SDL2 instead (gl_platform_sdl.cpp).
#if !defined(_WIN32) && !defined(USE_SDL)
#define GL_GLEXT_PROTOTYPES
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GL/gl.h>
#include <cstdio>
#include <cstdint>


static EGLDisplay g_dpy;
static EGLContext g_ctx;
void gl_platform_make_current();

bool gl_platform_init(int, int) {
    auto getPlat = (PFNEGLGETPLATFORMDISPLAYEXTPROC)eglGetProcAddress("eglGetPlatformDisplayEXT");
    g_dpy = getPlat ? getPlat(EGL_PLATFORM_SURFACELESS_MESA, EGL_DEFAULT_DISPLAY, nullptr) : eglGetDisplay(EGL_DEFAULT_DISPLAY);
    EGLint ma, mi;
    if (!eglInitialize(g_dpy, &ma, &mi)) { fprintf(stderr, "[gl] eglInitialize failed\n"); return false; }
    eglBindAPI(EGL_OPENGL_API);
    EGLint ca[] = {EGL_RENDERABLE_TYPE, EGL_OPENGL_BIT, EGL_NONE};
    EGLConfig cfg; EGLint n = 0;
    eglChooseConfig(g_dpy, ca, &cfg, 1, &n);
    EGLint cx[] = {EGL_CONTEXT_MAJOR_VERSION, 3, EGL_CONTEXT_MINOR_VERSION, 3,
                   EGL_CONTEXT_OPENGL_PROFILE_MASK, EGL_CONTEXT_OPENGL_CORE_PROFILE_BIT, EGL_NONE};
    g_ctx = eglCreateContext(g_dpy, n ? cfg : nullptr, EGL_NO_CONTEXT, cx);
    if (!g_ctx) { fprintf(stderr, "[gl] eglCreateContext failed\n"); return false; }
    gl_platform_make_current();
    fprintf(stderr, "[gl] %s | %s\n", glGetString(GL_RENDERER), glGetString(GL_VERSION));
    return true;
}
static thread_local bool t_current = false;
void gl_platform_make_current() { if (t_current) return; eglMakeCurrent(g_dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, g_ctx); t_current = true; }
void gl_platform_release_current() { if (!t_current) return; eglMakeCurrent(g_dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT); t_current = false; }
void platform_rumble(int, int) {}
// Simulated sound device for headless runs: pulls 32 kHz audio in real time from the
// jitter buffer so dropouts can be counted (see the [audio] line in the perf log).
#include "audio_ring.h"
#include "hostclock.h"
#include <thread>
#include <cstdlib>
#include <cstdio>
#include <chrono>
void platform_audio_push(const int16_t* lr, int frames) {
    static bool started = false;
    if (!started) {
        started = true;
        std::thread([] {
            int16_t buf[512 * 2];
            auto next = std::chrono::steady_clock::now();
            for (;;) { next += std::chrono::microseconds(11610); precise_sleep_until(next); audio_ring_pull(buf, 512); }
        }).detach();
    }
    audio_ring_push(lr, frames);
    // AUDIO_DUMP=file.wav: record everything the SPU outputs (headless verification)
    static FILE* wav = nullptr;
    static uint32_t total = 0;
    static bool tried = false;
    if (!tried) {
        tried = true;
        if (const char* p = getenv("AUDIO_DUMP")) {
            wav = fopen(p, "wb");
            if (wav) { uint8_t hdr[44] = {}; fwrite(hdr, 1, 44, wav); }
        }
    }
    if (wav) {
        fwrite(lr, 4, frames, wav);
        total += frames * 4;
        // keep the header valid as we go
        long pos = ftell(wav);
        uint32_t rate = 44100, byterate = 44100 * 4, riff = total + 36;
        uint16_t fmt = 1, ch = 2, align = 4, bits = 16;
        uint32_t fmtlen = 16;
        fseek(wav, 0, SEEK_SET);
        fwrite("RIFF", 1, 4, wav); fwrite(&riff, 4, 1, wav); fwrite("WAVEfmt ", 1, 8, wav);
        fwrite(&fmtlen, 4, 1, wav); fwrite(&fmt, 2, 1, wav); fwrite(&ch, 2, 1, wav); fwrite(&rate, 4, 1, wav);
        fwrite(&byterate, 4, 1, wav); fwrite(&align, 2, 1, wav); fwrite(&bits, 2, 1, wav);
        fwrite("data", 1, 4, wav); fwrite(&total, 4, 1, wav);
        fseek(wav, pos, SEEK_SET);
    }
}
void gl_platform_present(unsigned, int, int) {}
bool gl_platform_poll() { return true; }
#endif
