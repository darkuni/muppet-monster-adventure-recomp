#include "rumble.h"
#include "game.h"
#include "config.h"
#include "log.h"
#include <algorithm>
#include <cstdio>

void platform_rumble_f(float low, float high);

struct Effect {
    uint32_t hash;
    float low, high;     // big (low-frequency) and small (high-frequency) motor, 0..1
    int frames;          // length (60 fps frames), the pulse fades out over it
    bool weapon;         // only with "On + weapons"
    bool by_volume;      // scale with how loud the game plays it
    const char* what;
};
#ifdef GAME_SOUND_RUMBLE
// Sound effects of the common SE bank (the same sample data is in every stage's archive)
static const Effect kEffects[] = {
    {0x2f422009u, 0.85f, 1.00f, 14, false, false, "player hit"},
    {0x15ae8801u, 1.00f, 0.70f, 30, false, false, "player ship explodes"},
    {0x7b601e52u, 1.00f, 1.00f, 40, false, false, "big blast"},
    {0xaeeb3a35u, 0.55f, 0.35f, 10, false, true, "explosion"},
    {0x5884f5ffu, 0.00f, 0.30f, 4, false, false, "enemy destroyed"},
    {0xb5daa2d5u, 0.00f, 0.40f, 6, false, false, "gunpod pick-up"},
    {0xefe8972fu, 0.00f, 0.12f, 2, true, false, "machine gun"},
    {0x6c468fc7u, 0.10f, 0.15f, 3, true, false, "gunpod fire"},
};
#else
// the game drives the DualShock motors itself (sio.cpp): no sound-driven effects
static const Effect kEffects[] = {{0, 0, 0, 0, false, false, ""}};
#endif

struct Active { float low, high; int left, len; };
static Active g_act[16];

void rumble_sample(uint32_t hash, int volume) {
    if (!g_cfg.vibration || !hash) return;
    for (const Effect& e : kEffects) {
        if (e.hash != hash) continue;
        if (e.weapon && g_cfg.vibration < 2) return;
        float s = e.by_volume ? std::clamp(volume / (float)0x1800, 0.4f, 1.0f) : 1.0f;
        int slot = 0;
        for (int i = 0; i < 16; i++) if (g_act[i].left < g_act[slot].left) slot = i;
        g_act[slot] = {e.low * s, e.high * s, e.frames, e.frames};
        LOG("rumble", "%s (%08x, volume %04x)", e.what, hash, volume);
        return;
    }
}

void rumble_frame() {
#ifndef GAME_SOUND_RUMBLE
    return;
#endif
    float low = 0, high = 0;
    for (Active& a : g_act) {
        if (a.left <= 0) continue;
        float f = (float)a.left / (float)a.len;   // linear fade
        low = std::max(low, a.low * f);
        high = std::max(high, a.high * f);
        a.left--;
    }
    float k = g_cfg.vibration ? g_cfg.vibration_strength / 100.0f : 0.0f;
    platform_rumble_f(std::min(1.0f, low * k), std::min(1.0f, high * k));
}
