#include "game.h"
#include "config.h"
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

Config g_cfg;
static std::string g_ini_path = GAME_INI;

static void set(const std::string& k, const std::string& v) {
    auto b = [&] { return v == "1" || v == "true" || v == "yes" || v == "on"; };
    auto clampi = [&](int lo, int hi) { return std::max(lo, std::min(hi, atoi(v.c_str()))); };
    if (k == "resolution_scale") g_cfg.resolution_scale = clampi(1, 8);
    else if (k == "widescreen") g_cfg.widescreen = b();
    else if (k == "vsync") g_cfg.vsync = b();
    else if (k == "fullscreen") g_cfg.fullscreen = b();
    else if (k == "window_scale") g_cfg.window_scale = clampi(1, 8);
    else if (k == "show_perf") g_cfg.show_perf = b();
    else if (k == "swap_ab_xy") g_cfg.swap_ab_xy = b();
    else if (k == "speed") g_cfg.speed = clampi(1, 8);
    else if (k == "vibration") g_cfg.vibration = b();
    else if (k == "smooth_scaling") g_cfg.smooth_scaling = b();
    else if (k == "fast_cd") g_cfg.fast_cd = b();
    else if (k == "keys") g_cfg.keys = v;
    else if (k == "pad") g_cfg.pad = v;
}

static std::string trim(std::string s) {
    while (!s.empty() && strchr(" \t\r\n", s.back())) s.pop_back();
    while (!s.empty() && strchr(" \t", s[0])) s.erase(0, 1);
    return s;
}

void config_load(const char* exe_path) {
    std::string dir = exe_path ? exe_path : "";
    size_t slash = dir.find_last_of("/\\");
    dir = slash == std::string::npos ? "" : dir.substr(0, slash + 1);
    std::string paths[2] = {dir + GAME_INI, GAME_INI};
    FILE* f = nullptr;
    for (auto& p : paths) if ((f = fopen(p.c_str(), "r"))) { g_ini_path = p; break; }
    bool write_defaults = !f;
    if (!f) g_ini_path = dir + GAME_INI;
    if (f) {
        char line[1024];
        while (fgets(line, sizeof line, f)) {
            std::string l = trim(line);
            if (l.empty() || l[0] == ';' || l[0] == '#' || l[0] == '[') continue;
            size_t eq = l.find('=');
            if (eq == std::string::npos) continue;
            set(trim(l.substr(0, eq)), trim(l.substr(eq + 1)));
        }
        fclose(f);
    }
    const char* keys[] = {"resolution_scale", "widescreen", "vsync", "fullscreen", "window_scale", "show_perf",
                          "swap_ab_xy", "speed", "vibration", "smooth_scaling", "fast_cd", "keys", "pad"};
    for (const char* k : keys) {
        std::string env = k;
        std::transform(env.begin(), env.end(), env.begin(), ::toupper);
        if (const char* v = getenv(env.c_str())) set(k, v);
    }
    fprintf(stderr, "[config] %s: resolution x%d, %s, vsync %d\n", g_ini_path.c_str(), g_cfg.resolution_scale,
            g_cfg.widescreen ? "16:9" : "4:3", g_cfg.vsync);
    if (write_defaults) config_save();
}

void config_save() {
    FILE* w = fopen(g_ini_path.c_str(), "w");
    if (!w) return;
    fprintf(w,
        "; " GAME_NAME " - native PC port settings (also in-game: F1 or Esc, or Start+Select on a controller)\n\n"
        "; Internal rendering resolution: 1 = original (320x240), 2, 3, 4 ... 8 times\n"
        "resolution_scale = %d\n"
        "; 16:9 widescreen: wider view of the 3D world (the 2D HUD is stretched). 0 = original 4:3\n"
        "widescreen = %d\n"
        "vsync = %d\n"
        "fullscreen = %d\n"
        "; Initial window size: 240 lines x this\n"
        "window_scale = %d\n"
        "; Frame rate in the corner of the screen\n"
        "show_perf = %d\n"
        "; Gamepad: swap A<->B and X<->Y (use the face buttons by position)\n"
        "swap_ab_xy = %d\n"
        "; Game speed: 1 = normal ... 8 (fast-forward; hold Tab for a burst)\n"
        "speed = %d\n"
        "; Controller vibration\n"
        "vibration = %d\n"
        "; Smooth (bilinear) scaling to the window; 0 = sharp pixels\n"
        "smooth_scaling = %d\n"
        "; Load game data faster than the original CD drive (music and movies still play in real time)\n"
        "fast_cd = %d\n"
        "; Keyboard bindings, e.g. keys = cross=X,circle=Z,square=A,triangle=S (edit in the F1 menu)\n"
        "keys = %s\n"
        "; Controller bindings, e.g. pad = cross=a,circle=b (edit in the F1 menu)\n"
        "pad = %s\n",
        g_cfg.resolution_scale, g_cfg.widescreen, g_cfg.vsync, g_cfg.fullscreen, g_cfg.window_scale, g_cfg.show_perf,
        g_cfg.swap_ab_xy, g_cfg.speed, g_cfg.vibration, g_cfg.smooth_scaling, g_cfg.fast_cd, g_cfg.keys.c_str(), g_cfg.pad.c_str());
    fclose(w);
}
