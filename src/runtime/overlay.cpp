// In-game settings menu and frame-rate overlay (windowed builds).
// Open/close: F1 or Esc on the keyboard, Start+Back together on a controller.
// UI state is changed on the emulation thread (input); drawing happens on the
// presentation thread after the game frame is blitted.
#if defined(_WIN32) || defined(USE_SDL)
#include "game.h"
#include "gl_include.h"
#include "overlay.h"
#include "config.h"
#include "font_atlas.h"
#include "keybinds.h"
#include "gte.h"
#include <mutex>
#include <string>
#include <vector>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <algorithm>

static std::mutex g_ui_mx;
static bool g_open = false;
static int g_sel = 0;
static bool g_quit_armed = false;
static int g_keys_page = 0;        // bindings page: 0 none, 1 keyboard, 2 controller
static int g_key_sel = 0;
static int g_capturing = -1;       // binding waiting for a key
char g_pad_name[64] = "no controller";   // set by the platform layer

struct Item {
    const char* label;
    std::vector<const char*> opts;
    const char* note;
};
static const std::vector<Item>& items() {
    static std::vector<Item> v = {
        {"Resolution", {"1x  320x240 (original)", "2x  640x480", "3x  960x720", "4x  1280x960", "5x  1600x1200", "6x  1920x1440"}, "Applies after a restart."},
        {"Widescreen 16:9", {"Off (4:3)", "On"}, "Wider view of the 3D world; the HUD is stretched."},
        {"VSync", {"Off", "On"}, nullptr},
        {"Fullscreen", {"Off", "On"}, "Also F11 or Alt+Enter."},
        {"Scaling", {"Sharp", "Smooth"}, "How the picture is scaled to the window."},
        {"Texture filtering", {"Off (original)", "Smooth 3D", "Smooth all"}, "Smooth 3D keeps the 2D art and text sharp."},
        {"Colour", {"True colour", "Original (dithered)"}, "Original: the PlayStation's 15-bit colour and dither pattern."},
        {"Precise geometry", {"Off (original)", "On"}, "No wobbling polygons or warping textures."},
        {"Screen filter", {"Off", "Scanlines", "CRT"}, "Needs a window at least 600 pixels tall."},
        {"Audio resampling", {"Standard", "High quality"}, "Cleaner streamed music and voices."},
#ifdef GAME_SOUND_RUMBLE
        {"Vibration", {"Off", "Hits and explosions", "Also when firing"}, "Added by this port: the original game has no vibration."},
#else
        {"Vibration", {"Off", "On"}, "DualShock rumble, when the game uses it."},
#endif
        {"Vibration strength", {"25%", "50%", "75%", "100%"}, nullptr},
        {"Swap A/B and X/Y", {"Off", "On"}, "Gamepad: use the face buttons by position (Nintendo style)."},
        {"Fast CD loading", {"Off", "On"}, "Loads levels faster; music and movies stay real-time."},
        {"Game speed", {"Normal", "2x", "3x", "4x", "5x", "6x", "7x", "8x"}, "Fast-forward. Hold Tab for a burst."},
        {"Frame rate display", {"Off", "On"}, nullptr},
        {"Keyboard keys", {}, "Change which key presses each PlayStation button."},
        {"Controller buttons", {}, "Change which controller button presses each PlayStation button."},
        {"Close menu", {}, nullptr},
        {"Quit game", {}, nullptr},
    };
    return v;
}
static int get_value(int i) {
    switch (i) {
        case 0: return std::min(5, g_cfg.resolution_scale - 1);
        case 1: return g_cfg.widescreen;
        case 2: return g_cfg.vsync;
        case 3: return g_cfg.fullscreen;
        case 4: return g_cfg.smooth_scaling;
        case 5: return g_cfg.texture_filter;
        case 6: return g_cfg.dither;
        case 7: return g_cfg.subpixel;
        case 8: return g_cfg.crt;
        case 9: return g_cfg.hq_audio;
        case 10: return std::min(g_cfg.vibration, (int)items()[10].opts.size() - 1);
        case 11: return std::clamp((g_cfg.vibration_strength + 12) / 25 - 1, 0, 3);
        case 12: return g_cfg.swap_ab_xy;
        case 13: return g_cfg.fast_cd;
        case 14: return std::max(1, std::min(8, g_cfg.speed)) - 1;
        case 15: return g_cfg.show_perf;
    }
    return 0;
}
static void set_value(int i, int v) {
    switch (i) {
        case 0: g_cfg.resolution_scale = v + 1; break;
        case 1: g_cfg.widescreen = v; g_gte_widescreen = v ? 0.75f : 1.0f; break;
        case 2: g_cfg.vsync = v; break;
        case 3: g_cfg.fullscreen = v; break;
        case 4: g_cfg.smooth_scaling = v; break;
        case 5: g_cfg.texture_filter = v; break;
        case 6: g_cfg.dither = v; break;
        case 7: g_cfg.subpixel = v; break;
        case 8: g_cfg.crt = v; break;
        case 9: g_cfg.hq_audio = v; break;
        case 10: g_cfg.vibration = v; break;
        case 11: g_cfg.vibration_strength = (v + 1) * 25; break;
        case 12: g_cfg.swap_ab_xy = v; break;
        case 13: g_cfg.fast_cd = v; break;
        case 14: g_cfg.speed = v + 1; break;
        case 15: g_cfg.show_perf = v; break;
    }
    config_save();
}

bool overlay_open() { return g_open; }
int overlay_capturing() { return (g_open && g_keys_page && g_capturing >= 0) ? g_keys_page : 0; }
void overlay_key_captured(const char* name) {
    std::lock_guard<std::mutex> g(g_ui_mx);
    if (g_capturing < 0) return;
    if (name && *name && !strchr(name, ',') && !strchr(name, '=')) {
        if (g_keys_page == 2) padbind_set(g_capturing, name);
        else keybind_set(g_capturing, name);
    }
    g_capturing = -1;
}
// Delete on a binding row clears it (no key / no button)
void overlay_clear_binding() {
    std::lock_guard<std::mutex> g(g_ui_mx);
    if (!g_open || !g_keys_page || g_capturing >= 0 || g_key_sel >= keybind_count()) return;
    if (g_keys_page == 2) padbind_set(g_key_sel, "");
    else keybind_set(g_key_sel, "");
}
void overlay_toggle() {
    std::lock_guard<std::mutex> g(g_ui_mx);
    g_open = !g_open;
    g_quit_armed = false;
    g_keys_page = 0;
    g_capturing = -1;
}

// rows of the keys page: one per binding, then "Reset to defaults" and "Back"
static int keys_rows() { return keybind_count() + 2; }
static int keys_input(int key) {
    int n = keys_rows();
    if (key == OV_KEY_UP) g_key_sel = (g_key_sel + n - 1) % n;
    else if (key == OV_KEY_DOWN) g_key_sel = (g_key_sel + 1) % n;
    else if (key == OV_KEY_BACK || key == OV_KEY_LEFT) g_keys_page = 0;
    else if (key == OV_KEY_OK || key == OV_KEY_RIGHT) {
        if (g_key_sel < keybind_count()) g_capturing = g_key_sel;
        else if (g_key_sel == keybind_count()) { if (g_keys_page == 2) padbind_reset(); else keybind_reset(); }
        else g_keys_page = 0;
    }
    return OV_NONE;
}

// returns an action the platform layer must carry out
int overlay_input(int key) {
    std::lock_guard<std::mutex> g(g_ui_mx);
    if (!g_open) return OV_NONE;
    if (g_keys_page) return g_capturing >= 0 ? OV_NONE : keys_input(key);
    const auto& it = items();
    int n = (int)it.size();
    if (key == OV_KEY_UP) { g_sel = (g_sel + n - 1) % n; g_quit_armed = false; }
    else if (key == OV_KEY_DOWN) { g_sel = (g_sel + 1) % n; g_quit_armed = false; }
    else if (key == OV_KEY_BACK) { g_open = false; }
    else if (key == OV_KEY_LEFT || key == OV_KEY_RIGHT || key == OV_KEY_OK) {
        const Item& m = it[g_sel];
        if (m.opts.empty()) {
            if (key != OV_KEY_OK) return OV_NONE;
            if (g_sel == n - 4) { g_keys_page = 1; g_key_sel = 0; return OV_NONE; }
            if (g_sel == n - 3) { g_keys_page = 2; g_key_sel = 0; return OV_NONE; }
            if (g_sel == n - 2) { g_open = false; return OV_NONE; }
            if (g_sel == n - 1) {
                if (g_quit_armed) return OV_QUIT;
                g_quit_armed = true;
            }
            return OV_NONE;
        }
        int cnt = (int)m.opts.size(), v = get_value(g_sel);
        v = key == OV_KEY_LEFT ? (v + cnt - 1) % cnt : (v + 1) % cnt;
        set_value(g_sel, v);
        if (g_sel == 3) return OV_FULLSCREEN;
        if (g_sel == 0 || g_sel == 1) return OV_WINDOW;
        if (g_sel == 2) return OV_VSYNC;
    }
    return OV_NONE;
}

// ---------------------------------------------------------------- drawing (presentation thread)
static GLuint g_prog = 0, g_vao = 0, g_vbo = 0, g_font = 0;
static GLint g_u_screen = -1, g_u_tex = -1;
struct V { float x, y, u, v, r, g, b, a; };
static std::vector<V> g_verts;

static GLuint sh(GLenum t, const char* src) {
    GLuint s = glCreateShader(t);
    glShaderSource(s, 1, &src, nullptr);
    glCompileShader(s);
    return s;
}
static void init_gl() {
    const char* vs = "#version 330 core\nlayout(location=0) in vec2 p; layout(location=1) in vec2 t; layout(location=2) in vec4 c;\n"
                     "uniform vec2 u_screen; out vec2 vt; out vec4 vc;\n"
                     "void main(){ gl_Position = vec4(p.x / u_screen.x * 2.0 - 1.0, 1.0 - p.y / u_screen.y * 2.0, 0.0, 1.0); vt = t; vc = c; }\n";
    const char* fs = "#version 330 core\nin vec2 vt; in vec4 vc; uniform sampler2D u_tex; out vec4 o;\n"
                     "void main(){ float a = vt.x < 0.0 ? 1.0 : texture(u_tex, vt).r; o = vec4(vc.rgb, vc.a * a); }\n";
    g_prog = glCreateProgram();
    glAttachShader(g_prog, sh(GL_VERTEX_SHADER, vs));
    glAttachShader(g_prog, sh(GL_FRAGMENT_SHADER, fs));
    glLinkProgram(g_prog);
    g_u_screen = glGetUniformLocation(g_prog, "u_screen");
    g_u_tex = glGetUniformLocation(g_prog, "u_tex");
    glGenVertexArrays(1, &g_vao);
    glBindVertexArray(g_vao);
    glGenBuffers(1, &g_vbo);
    glBindBuffer(GL_ARRAY_BUFFER, g_vbo);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, sizeof(V), (void*)0);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, sizeof(V), (void*)8);
    glVertexAttribPointer(2, 4, GL_FLOAT, GL_FALSE, sizeof(V), (void*)16);
    glEnableVertexAttribArray(0); glEnableVertexAttribArray(1); glEnableVertexAttribArray(2);
    glGenTextures(1, &g_font);
    glBindTexture(GL_TEXTURE_2D, g_font);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_R8, FONT_W, FONT_H, 0, GL_RED, GL_UNSIGNED_BYTE, FONT_PIXELS);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
}

static void quad(float x, float y, float w, float h, float u0, float v0, float u1, float v1, const float* c) {
    V a{x, y, u0, v0, c[0], c[1], c[2], c[3]}, b{x + w, y, u1, v0, c[0], c[1], c[2], c[3]};
    V d{x, y + h, u0, v1, c[0], c[1], c[2], c[3]}, e{x + w, y + h, u1, v1, c[0], c[1], c[2], c[3]};
    g_verts.insert(g_verts.end(), {a, b, d, b, e, d});
}
static void rect(float x, float y, float w, float h, const float* c) { quad(x, y, w, h, -1, 0, -1, 0, c); }
static float text(float x, float y, float s, const char* str, const float* c) {
    for (const char* p = str; *p; p++) {
        int ch = (unsigned char)*p;
        if (ch < 32 || ch > 127) ch = '?';
        int i = ch - 32;
        float u0 = (i % 16) * FONT_CW / (float)FONT_W, v0 = (i / 16) * FONT_CH / (float)FONT_H;
        float u1 = u0 + FONT_CW / (float)FONT_W, v1 = v0 + FONT_CH / (float)FONT_H;
        quad(x, y, FONT_CW * s, FONT_CH * s, u0, v0, u1, v1, c);
        x += FONT_CW * s;
    }
    return x;
}

void overlay_draw(int ww, int wh) {
    bool open;
    int keys_page;
    int sel, key_sel, capturing;
    bool armed;
    {
        std::lock_guard<std::mutex> g(g_ui_mx);
        open = g_open; sel = g_sel; armed = g_quit_armed;
        keys_page = g_keys_page; key_sel = g_key_sel; capturing = g_capturing;
    }
    if (!open && !(g_cfg.show_perf && g_perf_text[0])) return;
    if (!g_prog) init_gl();
    g_verts.clear();
    float s = wh / 900.0f;
    if (s < 0.6f) s = 0.6f;
    static const float white[4] = {1, 1, 1, 1}, grey[4] = {0.7f, 0.7f, 0.75f, 1}, gold[4] = {1.0f, 0.85f, 0.3f, 1},
                       panel[4] = {0.05f, 0.07f, 0.15f, 0.88f}, hl[4] = {0.25f, 0.35f, 0.7f, 0.9f}, shade[4] = {0, 0, 0, 0.45f};
    if (g_cfg.show_perf && g_perf_text[0]) {
        float tw = strlen(g_perf_text) * FONT_CW * s * 0.8f;
        rect(ww - tw - 16 * s, 8 * s, tw + 12 * s, FONT_CH * s * 0.8f + 6 * s, shade);
        text(ww - tw - 10 * s, 11 * s, s * 0.8f, g_perf_text, white);
    }
    if (open && keys_page) {
        int n = keys_rows();
        float s0 = s;
        s = std::min(s, wh * 0.92f / ((n + 4.5f) * FONT_CH * 1.25f));   // the long list must fit the window
        float lh = FONT_CH * s * 1.25f;
        float pw = 46 * FONT_CW * s, ph = (n + 4.5f) * lh;
        float px = (ww - pw) / 2, py = (wh - ph) / 2;
        rect(0, 0, (float)ww, (float)wh, shade);
        rect(px, py, pw, ph, panel);
        text(px + 20 * s, py + 14 * s, s, keys_page == 2 ? "CONTROLLER BUTTONS" : "KEYBOARD KEYS", gold);
        if (keys_page == 2) text(px + 20 * s + 22 * FONT_CW * s, py + 14 * s, s * 0.75f, g_pad_name, grey);
        float y = py + 14 * s + lh * 1.4f;
        for (int i = 0; i < n; i++) {
            if (i == key_sel) rect(px + 8 * s, y - 4 * s, pw - 16 * s, lh, hl);
            const float* col = i == key_sel ? white : grey;
            if (i < keybind_count()) {
                text(px + 20 * s, y, s, keybind(i).label, col);
                std::string k = i == capturing ? std::string(keys_page == 2 ? "press a button..." : "press a key...")
                                               : (keys_page == 2 ? padbind_input(i) : keybind_key(i));
                if (k.empty()) k = "-";
                text(px + 20 * s + 22 * FONT_CW * s, y, s, k.c_str(), i == capturing ? gold : white);
            } else {
                text(px + 20 * s, y, s, i == keybind_count() ? "Reset to defaults" : "Back", col);
            }
            y += lh;
        }
        text(px + 20 * s, y + lh * 0.4f, s * 0.75f,
             capturing >= 0 ? (keys_page == 2 ? "Press the controller button, trigger or stick direction. Esc cancels."
                                              : "Press the new key. Esc cancels.")
                            : "Enter: change   Delete: clear   Left / Backspace: back", capturing >= 0 ? gold : grey);
        text(px + 20 * s, y + lh * 1.3f, s * 0.75f, "Menu keys (arrows, Enter, Esc, F1) always work in this menu.", grey);
        s = s0;
    } else if (open) {
        const auto& it = items();
        float lh = FONT_CH * s * 1.35f;
        float pw = 46 * FONT_CW * s, ph = (it.size() + 4.5f) * lh;
        float px = (ww - pw) / 2, py = (wh - ph) / 2;
        rect(0, 0, (float)ww, (float)wh, shade);
        rect(px, py, pw, ph, panel);
        text(px + 20 * s, py + 14 * s, s, "SETTINGS", gold);
        float y = py + 14 * s + lh * 1.4f;
        for (size_t i = 0; i < it.size(); i++) {
            if ((int)i == sel) rect(px + 8 * s, y - 4 * s, pw - 16 * s, lh, hl);
            text(px + 20 * s, y, s, it[i].label, (int)i == sel ? white : grey);
            if (!it[i].opts.empty()) {
                char buf[96];
                snprintf(buf, sizeof buf, "< %s >", it[i].opts[get_value((int)i)]);
                text(px + 20 * s + 20 * FONT_CW * s, y, s, buf, white);
            } else if ((int)i == sel && armed) {
                text(px + 20 * s + 20 * FONT_CW * s, y, s, "press again to quit", gold);
            }
            y += lh;
        }
        const char* note = it[sel].note ? it[sel].note : "Up/Down select, Left/Right change. Saved to the .ini file.";
        text(px + 20 * s, y + lh * 0.4f, s * 0.75f, note, grey);
        text(px + 20 * s, y + lh * 1.3f, s * 0.75f, "F1 / Esc / Start+Back: close", grey);
    }
    glViewport(0, 0, ww, wh);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);
    glDisable(GL_SCISSOR_TEST);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glUseProgram(g_prog);
    glUniform2f(g_u_screen, (float)ww, (float)wh);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, g_font);
    const GLint unit0 = 0;
    glUniform1iv(g_u_tex, 1, &unit0);
    glBindVertexArray(g_vao);
    glBindBuffer(GL_ARRAY_BUFFER, g_vbo);
    glBufferData(GL_ARRAY_BUFFER, g_verts.size() * sizeof(V), g_verts.data(), GL_STREAM_DRAW);
    glDrawArrays(GL_TRIANGLES, 0, (GLsizei)g_verts.size());
    glDisable(GL_BLEND);
}
#endif
