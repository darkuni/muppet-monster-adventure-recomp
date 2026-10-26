#include "keybinds.h"
#include "config.h"
#include "hostinput.h"
#include <atomic>
#include <map>

static const KeyBind kBinds[] = {
    {"up", "D-pad up", "Up", IN_UP, "dpup"},
    {"down", "D-pad down", "Down", IN_DOWN, "dpdown"},
    {"left", "D-pad left", "Left", IN_LEFT, "dpleft"},
    {"right", "D-pad right", "Right", IN_RIGHT, "dpright"},
    {"cross", "Cross (jump)", "X", IN_SOUTH, "a"},
    {"circle", "Circle", "Z", IN_EAST, "b"},
    {"square", "Square", "A", IN_WEST, "x"},
    {"triangle", "Triangle", "S", IN_NORTH, "y"},
    {"l1", "L1", "Q", IN_L1, "leftshoulder"},
    {"r1", "R1", "W", IN_R1, "rightshoulder"},
    {"l2", "L2", "1", IN_L2, "lefttrigger"},
    {"r2", "R2", "2", IN_R2, "righttrigger"},
    {"start", "Start", "Return", IN_START, "start"},
    {"select", "Select", "Backspace", IN_BACK, "back"},
    {"l3", "L3 (left stick click)", "", IN_L3, "leftstick"},
    {"r3", "R3 (right stick click)", "", IN_R3, "rightstick"},
    {"cam_up", "Right stick up", "I", IN_ALT_UP, ""},
    {"cam_down", "Right stick down", "K", IN_ALT_DOWN, ""},
    {"cam_left", "Right stick left", "J", IN_ALT_LEFT, ""},
    {"cam_right", "Right stick right", "L", IN_ALT_RIGHT, ""},
    {"turbo", "Fast-forward (hold)", "Tab", IN_TURBO, ""},
};
static const int kCount = (int)(sizeof kBinds / sizeof kBinds[0]);
static std::atomic<unsigned> g_version{1};

int keybind_count() { return kCount; }
const KeyBind& keybind(int i) { return kBinds[i]; }
unsigned keybind_version() { return g_version; }

static std::map<std::string, std::string> parse(const std::string& s) {
    std::map<std::string, std::string> m;
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

// "none" marks a control deliberately left unbound (so it differs from an empty default)
static std::string lookup(const std::string& store, int i, const char* def) {
    auto m = parse(store);
    auto it = m.find(kBinds[i].id);
    if (it == m.end()) return def;
    return it->second == "none" ? std::string() : it->second;
}
static void store_set(std::string& store, int i, const std::string& v, const char* def) {
    auto m = parse(store);
    if (v == def) m.erase(kBinds[i].id);
    else m[kBinds[i].id] = v.empty() ? "none" : v;
    std::string s;
    for (int k = 0; k < kCount; k++) {
        auto it = m.find(kBinds[k].id);
        if (it == m.end()) continue;
        if (!s.empty()) s += ',';
        s += std::string(kBinds[k].id) + "=" + it->second;
    }
    store = s;
    config_save();
    g_version++;
}

std::string keybind_key(int i) { return lookup(g_cfg.keys, i, kBinds[i].def); }
void keybind_set(int i, const std::string& key) { store_set(g_cfg.keys, i, key, kBinds[i].def); }
std::string padbind_input(int i) { return lookup(g_cfg.pad, i, kBinds[i].pad_def); }
void padbind_set(int i, const std::string& in) { store_set(g_cfg.pad, i, in, kBinds[i].pad_def); }
void padbind_reset() {
    g_cfg.pad.clear();
    config_save();
    g_version++;
}

void keybind_reset() {
    g_cfg.keys.clear();
    config_save();
    g_version++;
}
