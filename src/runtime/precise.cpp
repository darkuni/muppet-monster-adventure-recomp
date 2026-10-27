#include "precise.h"
#include "config.h"
#include <unordered_map>
#include <vector>

struct Shadow { uint32_t val; PreciseVtx p; bool ok; };
static std::vector<Shadow> g_ram_shadow(0x200000 / 4);
static Shadow g_scratch_shadow[256];
// Recent projections by value, for screen coordinates the game copies through CPU registers.
// Entries live for a few frames: games draw the lists they built one or two vblanks earlier.
struct Recent { PreciseVtx p; unsigned frame; };
static std::unordered_map<uint32_t, Recent> g_by_value;
static unsigned g_frame = 0;
static const unsigned kKeepFrames = 4;

static Shadow* slot(uint32_t addr) {
    uint32_t a = addr & 0x1fffffffu;
    if (a < 0x800000u) return &g_ram_shadow[(a & 0x1ffffc) >> 2];
    if ((a & 0xfffffc00u) == 0x1f800000u) return &g_scratch_shadow[(a & 0x3fc) >> 2];
    return nullptr;
}

void precise_note(uint32_t val, const PreciseVtx& p) {
    if (g_by_value.size() > 262144) g_by_value.clear();
    g_by_value[val] = Recent{p, g_frame};
}

void precise_store(uint32_t addr, uint32_t val, const PreciseVtx& p) {
    if (Shadow* s = slot(addr)) { s->val = val; s->p = p; s->ok = true; }
    precise_note(val, p);
}

bool precise_lookup(uint32_t addr, uint32_t val, PreciseVtx* p) {
    if (!g_cfg.subpixel) return false;
    if (addr != 0xffffffffu) {
        Shadow* s = slot(addr);
        if (s && s->ok && s->val == val) { *p = s->p; return true; }
    }
    auto it = g_by_value.find(val);
    if (it == g_by_value.end() || g_frame - it->second.frame > kKeepFrames) return false;
    *p = it->second.p;
    return true;
}

void precise_frame() {
    g_frame++;
    if (g_frame % 64 == 0)   // drop stale entries now and then
        for (auto it = g_by_value.begin(); it != g_by_value.end();)
            it = g_frame - it->second.frame > kKeepFrames ? g_by_value.erase(it) : std::next(it);
}
