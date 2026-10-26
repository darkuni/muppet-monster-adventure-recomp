// Windowed platform layer (Windows, or Linux with -DUSE_SDL): SDL2 window with an
// OpenGL 3.3 core context, audio output and keyboard/gamepad input.
#if defined(_WIN32) || defined(USE_SDL)
#include "game.h"
#include "gl_include.h"
#include <SDL2/SDL.h>
#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <cmath>
#include <algorithm>

#include "config.h"
#include "hostinput.h"
#include "overlay.h"
#include "keybinds.h"
#include "app_icon.h"
#include <vector>
#include <string>
#include <cstring>
#include "audio_ring.h"
#include <atomic>
#include <thread>
#include <mutex>
#include <condition_variable>

static SDL_Window* g_win = nullptr;
static SDL_GLContext g_glc = nullptr;
static SDL_AudioDeviceID g_audio = 0;
static SDL_GameController* g_pad = nullptr;   // a controller SDL knows (XInput, PlayStation, Switch, ...)
static SDL_Joystick* g_joy = nullptr;          // any other joystick/gamepad, used through raw inputs
static SDL_JoystickID g_dev_id = -1;

static void set_pad_name() {
    const char* n = g_pad ? SDL_GameControllerName(g_pad) : g_joy ? SDL_JoystickName(g_joy) : nullptr;
    snprintf(g_pad_name, sizeof g_pad_name, "%s%s", n ? n : "no controller", (g_joy && !g_pad) ? " (generic)" : "");
}
static void close_pad() {
    if (g_pad) SDL_GameControllerClose(g_pad);
    else if (g_joy) SDL_JoystickClose(g_joy);
    g_pad = nullptr; g_joy = nullptr; g_dev_id = -1;
    set_pad_name();
}
static void open_pad(int index) {
    if (g_pad || g_joy) return;
    if (SDL_IsGameController(index)) {
        g_pad = SDL_GameControllerOpen(index);
        if (g_pad) g_dev_id = SDL_JoystickInstanceID(SDL_GameControllerGetJoystick(g_pad));
    } else {
        g_joy = SDL_JoystickOpen(index);
        if (g_joy) g_dev_id = SDL_JoystickInstanceID(g_joy);
    }
    set_pad_name();
    if (g_pad || g_joy) fprintf(stderr, "[input] controller: %s\n", g_pad_name);
}

// Raw-joystick defaults for controllers SDL has no mapping for (DirectInput PlayStation-style layout)
static const char* raw_default(const char* gc) {
    static const struct { const char* gc; const char* raw; } t[] = {
        {"dpup", "h0u"}, {"dpdown", "h0d"}, {"dpleft", "h0l"}, {"dpright", "h0r"},
        {"a", "b1"}, {"b", "b2"}, {"x", "b0"}, {"y", "b3"}, {"leftshoulder", "b4"}, {"rightshoulder", "b5"},
        {"lefttrigger", "b6"}, {"righttrigger", "b7"}, {"back", "b8"}, {"start", "b9"},
        {"leftstick", "b10"}, {"rightstick", "b11"}};
    for (auto& e : t) if (!strcmp(gc, e.gc)) return e.raw;
    return "";
}
// Is this controller input (see keybinds.h) held right now?
static bool pad_input_held(const std::string& in) {
    if (in.empty()) return false;
    if (g_pad) {
        SDL_GameControllerButton b = SDL_GameControllerGetButtonFromString(in.c_str());
        if (b != SDL_CONTROLLER_BUTTON_INVALID) return SDL_GameControllerGetButton(g_pad, b) != 0;
        char sign = in.back();
        std::string name = (sign == '+' || sign == '-') ? in.substr(0, in.size() - 1) : in;
        SDL_GameControllerAxis a = SDL_GameControllerGetAxisFromString(name.c_str());
        if (a == SDL_CONTROLLER_AXIS_INVALID) return false;
        int v = SDL_GameControllerGetAxis(g_pad, a);
        return sign == '-' ? v < -16000 : v > 16000;
    }
    if (!g_joy) return false;
    int n = atoi(in.c_str() + 1);
    switch (in[0]) {
        case 'b': return SDL_JoystickGetButton(g_joy, n) != 0;
        case 'h': {
            Uint8 h = SDL_JoystickGetHat(g_joy, n);
            char d = in.back();
            return (d == 'u' && (h & SDL_HAT_UP)) || (d == 'd' && (h & SDL_HAT_DOWN)) ||
                   (d == 'l' && (h & SDL_HAT_LEFT)) || (d == 'r' && (h & SDL_HAT_RIGHT));
        }
        case 'a': {
            int v = SDL_JoystickGetAxis(g_joy, n);
            return in.back() == '-' ? v < -16000 : v > 16000;
        }
    }
    return false;
}
// The binding for control i on the connected device
static std::string pad_binding(int i) {
    std::string in = padbind_input(i);
    if (g_joy && !g_pad && in == keybind(i).pad_def) in = raw_default(keybind(i).pad_def);
    return in;
}
static GLuint g_present_fbo = 0;
// Presentation runs on its own thread with a second (shared) GL context, so waiting for
// vsync never blocks the emulated CPU (which must keep servicing 3 ms audio interrupts).
static SDL_GLContext g_present_ctx = nullptr;
struct Slot { GLuint tex = 0; int w = 0, h = 0; GLsync fence = nullptr; };
static Slot g_slots[3];
static int g_ready = -1, g_showing = -1;
static std::mutex g_pm;
static std::condition_variable g_pcv;
static void present_thread();
static thread_local bool t_current = false;

bool gl_platform_init(int w, int h) {
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_GAMECONTROLLER | SDL_INIT_EVENTS) != 0) {
        fprintf(stderr, "[sdl] init failed: %s\n", SDL_GetError());
        return false;
    }
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
    int scale = g_cfg.window_scale;
    int ww = (g_cfg.widescreen ? 854 : 640) * scale, wh = 480 * scale;
    Uint32 flags = SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI;
    if (g_cfg.fullscreen) flags |= SDL_WINDOW_FULLSCREEN_DESKTOP;
    g_win = SDL_CreateWindow(GAME_NAME " (native)", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                             ww, wh, flags);
    if (!g_win) { fprintf(stderr, "[sdl] window failed: %s\n", SDL_GetError()); return false; }
    if (SDL_Surface* icon = SDL_CreateRGBSurfaceWithFormatFrom((void*)APP_ICON_RGBA, APP_ICON_W, APP_ICON_H, 32,
                                                               APP_ICON_W * 4, SDL_PIXELFORMAT_RGBA32)) {
        SDL_SetWindowIcon(g_win, icon);
        SDL_FreeSurface(icon);
    }
    g_present_ctx = SDL_GL_CreateContext(g_win);              // presenter
    if (!g_present_ctx) { fprintf(stderr, "[sdl] GL context failed: %s\n", SDL_GetError()); return false; }
    SDL_GL_SetAttribute(SDL_GL_SHARE_WITH_CURRENT_CONTEXT, 1);
    g_glc = SDL_GL_CreateContext(g_win);                      // renderer (shares textures/syncs)
    if (!g_glc) { fprintf(stderr, "[sdl] GL context failed: %s\n", SDL_GetError()); return false; }
    SDL_GL_MakeCurrent(g_win, g_glc);
    t_current = true;
#ifdef _WIN32
    if (!gl_load(SDL_GL_GetProcAddress)) return false;
#endif
    SDL_GL_SetSwapInterval(0);
    std::thread(present_thread).detach();
    fprintf(stderr, "[gl] %s | %s\n", glGetString(GL_RENDERER), glGetString(GL_VERSION));
    // audio: 44.1 kHz stereo s16, fed from the SPU
    SDL_AudioSpec want = {}, have = {};
    want.freq = 44100; want.format = AUDIO_S16SYS; want.channels = 2; want.samples = 512;
    want.callback = [](void*, Uint8* stream, int len) { audio_ring_pull((int16_t*)stream, len / 4); };
    g_audio = SDL_OpenAudioDevice(nullptr, 0, &want, &have, 0);
    if (g_audio) SDL_PauseAudioDevice(g_audio, 0);
    // extra controller mappings (https://github.com/mdqinc/SDL_GameControllerDB) next to the exe
    if (char* base = SDL_GetBasePath()) {
        std::string db = std::string(base) + "gamecontrollerdb.txt";
        int n = SDL_GameControllerAddMappingsFromFile(db.c_str());
        if (n > 0) fprintf(stderr, "[input] %d controller mappings from %s\n", n, db.c_str());
        SDL_free(base);
    }
    for (int i = 0; i < SDL_NumJoysticks() && !g_pad; i++) if (SDL_IsGameController(i)) open_pad(i);
    for (int i = 0; i < SDL_NumJoysticks() && !g_pad && !g_joy; i++) open_pad(i);
    if (!g_pad && !g_joy) fprintf(stderr, "[input] no controller found (keyboard active; controllers can be plugged in any time)\n");
    (void)w; (void)h;
    return true;
}

void gl_platform_release_current() {
    if (!t_current) return;
    SDL_GL_MakeCurrent(g_win, nullptr);
    t_current = false;
}

// Controller vibration (DualShock motors driven by the game)
void platform_rumble(int small_on, int large) {
    static int last_s = -1, last_l = -1;
    if ((!g_pad && !g_joy) || !g_cfg.vibration) return;
    if (small_on == last_s && large == last_l) return;
    last_s = small_on; last_l = large;
    Uint16 lo = (Uint16)(large * 257), hi = small_on ? 0xc000 : 0;
    if (g_pad) SDL_GameControllerRumble(g_pad, lo, hi, 1000);
    else SDL_JoystickRumble(g_joy, lo, hi, 1000);
}

void gl_platform_make_current() {
    if (t_current) return;
    SDL_GL_MakeCurrent(g_win, g_glc);
    t_current = true;
}

void platform_audio_push(const int16_t* lr, int frames) {
    if (g_audio) audio_ring_push(lr, frames);
}

static void apply_action(int a);
static void update_input() {
    const Uint8* k = SDL_GetKeyboardState(nullptr);
    // keyboard bindings (F1 menu / .ini), looked up again whenever they change
    static unsigned ver = 0;
    static std::vector<std::pair<SDL_Scancode, uint32_t>> binds;
    if (ver != keybind_version()) {
        ver = keybind_version();
        binds.clear();
        for (int i = 0; i < keybind_count(); i++) {
            SDL_Scancode sc = SDL_GetScancodeFromName(keybind_key(i).c_str());
            if (sc == SDL_SCANCODE_UNKNOWN) sc = SDL_GetScancodeFromName(keybind(i).def);
            binds.push_back({sc, keybind(i).bit});
        }
    }
    uint32_t b = 0;
    float lx = 0, ly = 0;
    for (auto& kb : binds) if (kb.first != SDL_SCANCODE_UNKNOWN && k[kb.first]) b |= kb.second;
    if (b & (IN_UP | IN_DOWN | IN_LEFT | IN_RIGHT)) b |= IN_KB_ARROWS;
    float rx = 0, ry = 0, lt = 0, rt = 0;
    if (b & IN_ALT_LEFT) rx = -1;
    if (b & IN_ALT_RIGHT) rx = 1;
    if (b & IN_ALT_UP) ry = 1;
    if (b & IN_ALT_DOWN) ry = -1;
    uint32_t pb = 0;   // controller part, also drives the settings menu
    if (g_pad || g_joy) {
        for (int i = 0; i < keybind_count(); i++)
            if (pad_input_held(pad_binding(i))) pb |= keybind(i).bit;
        if (g_cfg.swap_ab_xy) {   // face buttons by position (Nintendo layout)
            uint32_t f = pb & (IN_SOUTH | IN_EAST | IN_WEST | IN_NORTH);
            pb &= ~f;
            if (f & IN_SOUTH) pb |= IN_EAST;
            if (f & IN_EAST) pb |= IN_SOUTH;
            if (f & IN_WEST) pb |= IN_NORTH;
            if (f & IN_NORTH) pb |= IN_WEST;
        }
        auto dz = [](float v) {
            const float d = 0.2f;   // dead zone
            if (std::fabs(v) < d) return 0.f;
            return (v > 0 ? v - d : v + d) / (1 - d);
        };
        float ax[4] = {0, 0, 0, 0}, tl = 0, tr = 0;
        if (g_pad) {
            ax[0] = SDL_GameControllerGetAxis(g_pad, SDL_CONTROLLER_AXIS_LEFTX) / 32767.f;
            ax[1] = SDL_GameControllerGetAxis(g_pad, SDL_CONTROLLER_AXIS_LEFTY) / 32767.f;
            ax[2] = SDL_GameControllerGetAxis(g_pad, SDL_CONTROLLER_AXIS_RIGHTX) / 32767.f;
            ax[3] = SDL_GameControllerGetAxis(g_pad, SDL_CONTROLLER_AXIS_RIGHTY) / 32767.f;
            tl = SDL_GameControllerGetAxis(g_pad, SDL_CONTROLLER_AXIS_TRIGGERLEFT) / 32767.f;
            tr = SDL_GameControllerGetAxis(g_pad, SDL_CONTROLLER_AXIS_TRIGGERRIGHT) / 32767.f;
        } else {
            int na = SDL_JoystickNumAxes(g_joy);
            for (int i = 0; i < 4 && i < na; i++) ax[i] = SDL_JoystickGetAxis(g_joy, i) / 32767.f;
        }
        lx = dz(ax[0]);
        ly = -dz(ax[1]);
        float px = dz(ax[2]), py = -dz(ax[3]);
        if (px || py) { rx = px; ry = py; }
        lt = std::max(lt, tl);
        rt = std::max(rt, tr);
        b |= pb;
    }
    // controller drives the settings menu: Start+Select toggles it, d-pad/stick and Cross/Circle navigate
    {
        static uint32_t prev = 0;
        uint32_t nav = pb;
        if (lx > 0.5f) nav |= IN_RIGHT;
        if (lx < -0.5f) nav |= IN_LEFT;
        if (ly > 0.5f) nav |= IN_UP;
        if (ly < -0.5f) nav |= IN_DOWN;
        uint32_t down = nav & ~prev;
        prev = nav;
        // the button that was just bound must not also act on the menu
        static bool was_capturing = false;
        bool capturing = overlay_capturing() != 0;
        if (was_capturing) down = 0;
        was_capturing = capturing;
        if (!capturing) {
            if ((down & (IN_START | IN_BACK)) && (nav & IN_START) && (nav & IN_BACK)) overlay_toggle();
            else if (overlay_open()) {
                int key = -1;
                if (down & IN_UP) key = OV_KEY_UP;
                else if (down & IN_DOWN) key = OV_KEY_DOWN;
                else if (down & IN_LEFT) key = OV_KEY_LEFT;
                else if (down & IN_RIGHT) key = OV_KEY_RIGHT;
                else if (down & IN_SOUTH) key = OV_KEY_OK;
                else if (down & IN_EAST) key = OV_KEY_BACK;
                if (key >= 0) apply_action(overlay_input(key));
            }
        }
    }
    if (b & IN_L2) lt = 1;
    if (b & IN_R2) rt = 1;
    if (overlay_open()) { b = 0; lx = ly = rx = ry = lt = rt = 0; }   // the menu owns the controls while open
    g_host_input.rx = std::max(-1.f, std::min(1.f, rx));
    g_host_input.ry = std::max(-1.f, std::min(1.f, ry));
    g_host_input.lt = std::max(0.f, std::min(1.f, lt));
    g_host_input.rt = std::max(0.f, std::min(1.f, rt));
    g_host_input.buttons = b;
    g_host_input.lx = std::max(-1.f, std::min(1.f, lx));
    g_host_input.ly = std::max(-1.f, std::min(1.f, ly));
}

static std::atomic<bool> g_vsync_dirty{false};

static void apply_action(int a) {
    if (a == OV_FULLSCREEN) SDL_SetWindowFullscreen(g_win, g_cfg.fullscreen ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0);
    else if (a == OV_VSYNC) g_vsync_dirty = true;
    else if (a == OV_QUIT) exit(0);
}

bool gl_platform_poll() {
    SDL_Event e;
    while (SDL_PollEvent(&e)) {
        if (e.type == SDL_QUIT) exit(0);
        if (e.type == SDL_KEYDOWN && overlay_capturing()) {
            // keyboard page: the key to bind; controller page: Esc cancels
            SDL_Keycode k = e.key.keysym.sym;
            if (k == SDLK_ESCAPE || k == SDLK_F1 || k == SDLK_F11) overlay_key_captured(nullptr);
            else if (!e.key.repeat && overlay_capturing() == 1) overlay_key_captured(SDL_GetScancodeName(e.key.keysym.scancode));
            continue;
        }
        if (e.type == SDL_KEYDOWN) {
            SDL_Keycode k = e.key.keysym.sym;
            if (k == SDLK_F1 || k == SDLK_ESCAPE) {
                if (overlay_open() && k == SDLK_ESCAPE) overlay_input(OV_KEY_BACK);
                else overlay_toggle();
            } else if (k == SDLK_F11 || (k == SDLK_RETURN && (e.key.keysym.mod & KMOD_ALT))) {
                g_cfg.fullscreen = !g_cfg.fullscreen;
                config_save();
                apply_action(OV_FULLSCREEN);
            } else if (overlay_open()) {
                if (k == SDLK_DELETE) { overlay_clear_binding(); continue; }
                int key = -1;
                if (k == SDLK_UP) key = OV_KEY_UP;
                else if (k == SDLK_DOWN) key = OV_KEY_DOWN;
                else if (k == SDLK_LEFT) key = OV_KEY_LEFT;
                else if (k == SDLK_RIGHT) key = OV_KEY_RIGHT;
                else if (k == SDLK_RETURN || k == SDLK_SPACE || k == SDLK_x) key = OV_KEY_OK;
                else if (k == SDLK_BACKSPACE || k == SDLK_z) key = OV_KEY_BACK;
                if (key >= 0) apply_action(overlay_input(key));
            }
        }
        // capturing a controller binding in the settings menu
        if (overlay_capturing() == 2) {
            std::string in;
            if (g_pad && e.type == SDL_CONTROLLERBUTTONDOWN && e.cbutton.which == g_dev_id)
                in = SDL_GameControllerGetStringForButton((SDL_GameControllerButton)e.cbutton.button);
            else if (g_pad && e.type == SDL_CONTROLLERAXISMOTION && e.caxis.which == g_dev_id && std::abs(e.caxis.value) > 24000) {
                SDL_GameControllerAxis a = (SDL_GameControllerAxis)e.caxis.axis;
                in = SDL_GameControllerGetStringForAxis(a);
                if (a != SDL_CONTROLLER_AXIS_TRIGGERLEFT && a != SDL_CONTROLLER_AXIS_TRIGGERRIGHT) in += e.caxis.value < 0 ? "-" : "+";
            } else if (!g_pad && g_joy && e.type == SDL_JOYBUTTONDOWN && e.jbutton.which == g_dev_id) {
                in = "b" + std::to_string(e.jbutton.button);
            } else if (!g_pad && g_joy && e.type == SDL_JOYHATMOTION && e.jhat.which == g_dev_id && e.jhat.value) {
                Uint8 v = e.jhat.value;
                in = "h" + std::to_string(e.jhat.hat) + (v & SDL_HAT_UP ? "u" : v & SDL_HAT_DOWN ? "d" : v & SDL_HAT_LEFT ? "l" : "r");
            } else if (!g_pad && g_joy && e.type == SDL_JOYAXISMOTION && e.jaxis.which == g_dev_id && std::abs(e.jaxis.value) > 24000) {
                in = "a" + std::to_string(e.jaxis.axis) + (e.jaxis.value < 0 ? "-" : "+");
            }
            if (!in.empty()) { overlay_key_captured(in.c_str()); continue; }
        }
        if (e.type == SDL_CONTROLLERDEVICEADDED && !g_pad) {
            if (g_joy) close_pad();   // a known controller beats a generic joystick
            open_pad(e.cdevice.which);
        }
        if (e.type == SDL_JOYDEVICEADDED && !g_pad && !g_joy && !SDL_IsGameController(e.jdevice.which))
            open_pad(e.jdevice.which);
        if ((e.type == SDL_CONTROLLERDEVICEREMOVED && e.cdevice.which == g_dev_id) ||
            (e.type == SDL_JOYDEVICEREMOVED && e.jdevice.which == g_dev_id)) {
            close_pad();
            for (int i = 0; i < SDL_NumJoysticks() && !g_pad && !g_joy; i++) open_pad(i);
        }
    }
    update_input();
    return true;
}

// Called on the emulation thread with the finished frame (top-down texture rows).
void gl_platform_present(unsigned tex, int w, int h) {
    if (!g_present_fbo) glGenFramebuffers(1, &g_present_fbo);
    static GLuint dst_fbo = 0;
    if (!dst_fbo) glGenFramebuffers(1, &dst_fbo);
    int s;
    {
        std::lock_guard<std::mutex> g(g_pm);
        for (s = 0; s < 3; s++) if (s != g_ready && s != g_showing) break;
    }
    Slot& sl = g_slots[s];
    if (!sl.tex || sl.w != w || sl.h != h) {
        if (!sl.tex) glGenTextures(1, &sl.tex);
        glBindTexture(GL_TEXTURE_2D, sl.tex);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        sl.w = w; sl.h = h;
    }
    glBindFramebuffer(GL_READ_FRAMEBUFFER, g_present_fbo);
    glFramebufferTexture2D(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex, 0);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, dst_fbo);
    glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, sl.tex, 0);
    glDisable(GL_SCISSOR_TEST);
    glBlitFramebuffer(0, 0, w, h, 0, 0, w, h, GL_COLOR_BUFFER_BIT, GL_NEAREST);
    GLsync f = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
    glFlush();
    {
        std::lock_guard<std::mutex> g(g_pm);
        if (g_ready >= 0 && g_slots[g_ready].fence) { glDeleteSync(g_slots[g_ready].fence); g_slots[g_ready].fence = nullptr; }
        sl.fence = f;
        g_ready = s;
    }
    g_pcv.notify_one();
    gl_platform_poll();
}

// Final scaling of the frame to the window. Downscaling (internal resolution above the
// window size) averages a grid of bilinear taps across each window pixel's footprint
// so the extra resolution becomes anti-aliasing instead of shimmer; upscaling is bilinear.
static GLuint g_scale_prog = 0, g_scale_vao = 0;
static GLint g_u_src = -1, g_u_scale = -1, g_u_rect = -1;
static void init_scaler() {
    const char* vs = "#version 330 core\nuniform vec4 u_rect; out vec2 uv;\n"
        "void main(){ vec2 p = vec2(gl_VertexID & 1, gl_VertexID >> 1); uv = vec2(p.x, p.y);\n"
        " vec2 q = u_rect.xy + p * u_rect.zw; gl_Position = vec4(q.x * 2.0 - 1.0, 1.0 - q.y * 2.0, 0.0, 1.0); }\n";
    const char* fs = "#version 330 core\nin vec2 uv; uniform sampler2D u_src; uniform vec2 u_scale; out vec4 o;\n"
        "void main(){\n"
        " vec2 ts = vec2(textureSize(u_src, 0));\n"
        " vec2 n = clamp(ceil(u_scale), vec2(1.0), vec2(4.0));\n"   // taps per axis
        " vec2 fp = u_scale / ts;\n"                                  // footprint in uv
        " vec4 acc = vec4(0.0);\n"
        " for (int j = 0; j < 4; j++) for (int i = 0; i < 4; i++) {\n"
        "  if (float(i) >= n.x || float(j) >= n.y) continue;\n"
        "  vec2 off = (vec2(i, j) + 0.5) / n - 0.5;\n"
        "  acc += texture(u_src, uv + off * fp);\n"
        " }\n"
        " o = vec4(acc.rgb / (n.x * n.y), 1.0); }\n";
    auto sh = [](GLenum t, const char* src) { GLuint s = glCreateShader(t); glShaderSource(s, 1, &src, nullptr); glCompileShader(s); return s; };
    g_scale_prog = glCreateProgram();
    glAttachShader(g_scale_prog, sh(GL_VERTEX_SHADER, vs));
    glAttachShader(g_scale_prog, sh(GL_FRAGMENT_SHADER, fs));
    glLinkProgram(g_scale_prog);
    g_u_src = glGetUniformLocation(g_scale_prog, "u_src");
    g_u_scale = glGetUniformLocation(g_scale_prog, "u_scale");
    g_u_rect = glGetUniformLocation(g_scale_prog, "u_rect");
    glGenVertexArrays(1, &g_scale_vao);
}

static void present_thread() {
    SDL_GL_MakeCurrent(g_win, g_present_ctx);
    if (g_cfg.vsync) { if (SDL_GL_SetSwapInterval(-1) != 0) SDL_GL_SetSwapInterval(1); }
    else SDL_GL_SetSwapInterval(0);
    GLuint fbo = 0;
    glGenFramebuffers(1, &fbo);
    for (;;) {
        int s;
        GLsync f;
        {
            std::unique_lock<std::mutex> lk(g_pm);
            g_pcv.wait(lk, [] { return g_ready >= 0; });
            s = g_ready; g_ready = -1; g_showing = s;
            f = g_slots[s].fence; g_slots[s].fence = nullptr;
        }
        if (f) { glWaitSync(f, 0, GL_TIMEOUT_IGNORED); glDeleteSync(f); }
        Slot& sl = g_slots[s];
        int ww, wh;
        SDL_GL_GetDrawableSize(g_win, &ww, &wh);
        const int an = g_cfg.widescreen ? 16 : 4, ad = g_cfg.widescreen ? 9 : 3;
        int dw = ww, dh = ww * ad / an;
        if (dh > wh) { dh = wh; dw = wh * an / ad; }
        int dx = (ww - dw) / 2, dy = (wh - dh) / 2;
        glBindFramebuffer(GL_READ_FRAMEBUFFER, fbo);
        glFramebufferTexture2D(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, sl.tex, 0);
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
        glViewport(0, 0, ww, wh);
        glDisable(GL_SCISSOR_TEST);
        glClearColor(0, 0, 0, 1);
        glClear(GL_COLOR_BUFFER_BIT);
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        if (!g_scale_prog) init_scaler();
        glDisable(GL_BLEND); glDisable(GL_DEPTH_TEST); glDisable(GL_CULL_FACE);
        glUseProgram(g_scale_prog);
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, sl.tex);
        {
            // sharp scaling: nearest-neighbour when enlarging (downscaling always averages)
            GLint f = (!g_cfg.smooth_scaling && dw >= sl.w) ? GL_NEAREST : GL_LINEAR;
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, f);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, f);
        }
        glUniform1i(g_u_src, 0);
        glUniform2f(g_u_scale, (float)sl.w / dw, (float)sl.h / dh);
        glUniform4f(g_u_rect, (float)dx / ww, (float)dy / wh, (float)dw / ww, (float)dh / wh);
        glBindVertexArray(g_scale_vao);
        glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
        overlay_draw(ww, wh);
        if (g_vsync_dirty.exchange(false)) {
            if (g_cfg.vsync) { if (SDL_GL_SetSwapInterval(-1) != 0) SDL_GL_SetSwapInterval(1); }
            else SDL_GL_SetSwapInterval(0);
        }
        SDL_GL_SwapWindow(g_win);
        { std::lock_guard<std::mutex> g(g_pm); g_showing = -1; }
    }
}
#endif
