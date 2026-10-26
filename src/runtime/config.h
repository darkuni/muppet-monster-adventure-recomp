#pragma once
#include <string>

// Settings from the .ini file next to the executable. Every key can also be overridden with
// an environment variable of the same name in upper case (e.g. RESOLUTION_SCALE=4).
struct Config {
    int resolution_scale = 3;        // internal resolution = native * scale
    bool widescreen = false;         // 16:9: wider 3D field of view (GTE projection hack), HUD stretched
    bool vsync = true;
    bool fullscreen = false;
    int window_scale = 3;            // initial window height = 240 * window_scale
    bool show_perf = false;
    bool swap_ab_xy = false;         // gamepad: Nintendo-style face button positions
    int speed = 1;                   // 1..8 (fast-forward)
    bool vibration = true;
    bool smooth_scaling = true;      // bilinear final scaling (off = sharp pixels)
    bool fast_cd = true;             // read game data faster than a real drive (music and movies stay real-time)
    std::string keys;                // keyboard bindings, "button=key,..." (empty = defaults)
    std::string pad;                 // controller bindings, "button=input,..." (empty = defaults)
};
extern Config g_cfg;
void config_load(const char* exe_path);
void config_save();
