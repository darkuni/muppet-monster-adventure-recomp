#include "keybinds.h"
#include "config.h"
#include "hostinput.h"
#include <atomic>
#include <map>

static const KeyBind kBinds[] = {
    {"up", "D-pad up", "Up", IN_UP},
    {"down", "D-pad down", "Down", IN_DOWN},
    {"left", "D-pad left", "Left", IN_LEFT},
    {"right", "D-pad right", "Right", IN_RIGHT},
    {"cross", "Cross (jump)", "X", IN_SOUTH},
    {"circle", "Circle", "Z", IN_EAST},
    {"square", "Square", "A", IN_WEST},
    {"triangle", "Triangle", "S", IN_NORTH},
    {"l1", "L1", "Q", IN_L1},
    {"r1", "R1", "W", IN_R1},
    {"l2", "L2", "1", IN_L2},
    {"r2", "R2", "2", IN_R2},
    {"start", "Start", "Return", IN_START},
    {"select", "Select", "Backspace", IN_BACK},
    {"cam_up", "Right stick up", "I", IN_ALT_UP},
    {"cam_down", "Right stick down", "K", IN_ALT_DOWN},
    {"cam_left", "Right stick left", "J", IN_ALT_LEFT},
    {"cam_right", "Right stick right", "L", IN_ALT_RIGHT},
    {"turbo", "Fast-forward (hold)", "Tab", IN_TURBO},
};
static const int kCount = (int)(sizeof kBinds / sizeof kBinds[0]);
static std::atomic<unsigned> g_version{1};

int keybind_count() { return kCount; }
const KeyBind& keybind(int i) { return kBinds[i]; }
unsigned keybind_version() { return g_version; }

static std::map<std::string, std::string> parse() {
    std::map<std::string, std::string> m;
    const std::string& s = g_cfg.keys;
    size_t p = 0;
    while (p < s.size()) {
        size_t c = s.find(',', p);
        std::string it = s.substr(p, c == std::string::npos ? std::string::npos : c - p);
        p = c == std::string::npos ? s.size() : c + 1;
        size_t eq = it.find('=');
        if (eq != std::string::npos && eq + 1 < it.size()) m[it.substr(0, eq)] = it.substr(eq + 1);
    }
    return m;
}

std::string keybind_key(int i) {
    auto m = parse();
    auto it = m.find(kBinds[i].id);
    return it == m.end() ? kBinds[i].def : it->second;
}

void keybind_set(int i, const std::string& key) {
    auto m = parse();
    if (key == kBinds[i].def) m.erase(kBinds[i].id);
    else m[kBinds[i].id] = key;
    std::string s;
    for (int k = 0; k < kCount; k++) {
        auto it = m.find(kBinds[k].id);
        if (it == m.end()) continue;
        if (!s.empty()) s += ',';
        s += std::string(kBinds[k].id) + "=" + it->second;
    }
    g_cfg.keys = s;
    config_save();
    g_version++;
}

void keybind_reset() {
    g_cfg.keys.clear();
    config_save();
    g_version++;
}
