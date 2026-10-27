// Muppet Monster Adventure - native PC port (static recompilation of SLUS-01238).
#include "game.h"
#include "config.h"
#include "hw.h"
#include "bios.h"
#include "gte.h"
#include "log.h"
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>
#if defined(_WIN32) || defined(USE_SDL)
#include <SDL2/SDL.h>
#endif

namespace fs = std::filesystem;
static std::string g_exe_dir;

#include "hostinput.h"
// INPUT_SCRIPT="2400=cross,2410=,3000=up+cross" : from frame N hold these buttons (headless tests)
static void scripted_input(int frame) {
    static std::vector<std::pair<int, uint32_t>> steps;
    static bool parsed = false;
    if (!parsed) {
        parsed = true;
        const char* e = getenv("INPUT_SCRIPT");
        if (!e) return;
        std::string s = e;
        size_t p = 0;
        while (p < s.size()) {
            size_t c = s.find(',', p);
            std::string item = s.substr(p, c == std::string::npos ? std::string::npos : c - p);
            p = c == std::string::npos ? s.size() : c + 1;
            size_t eq = item.find('=');
            if (eq == std::string::npos) continue;
            int f = atoi(item.substr(0, eq).c_str());
            uint32_t b = 0;
            std::string names = item.substr(eq + 1) + "+";
            size_t q = 0;
            while (q < names.size()) {
                size_t pl = names.find('+', q);
                std::string n = names.substr(q, pl - q);
                q = pl + 1;
                static const struct { const char* n; uint32_t b; } map[] = {
                    {"up", IN_UP}, {"down", IN_DOWN}, {"left", IN_LEFT}, {"right", IN_RIGHT}, {"cross", IN_SOUTH},
                    {"circle", IN_EAST}, {"square", IN_WEST}, {"triangle", IN_NORTH}, {"l1", IN_L1}, {"r1", IN_R1},
                    {"l2", IN_L2}, {"r2", IN_R2}, {"start", IN_START}, {"select", IN_BACK}};
                for (auto& m : map) if (n == m.n) b |= m.b;
            }
            steps.push_back({f, b});
        }
    }
    for (auto& st : steps) if (st.first == frame) g_host_input.buttons = st.second;
    // ANCHOR=addr=value: ASCRIPT frames count from the first frame RAM[addr] == value (compare runs)
    static int anchor = -1;
    static uint32_t aa = 0, av = 0;
    static std::vector<std::pair<int, uint32_t>> as;
    static bool ainit = false;
    if (!ainit) {
        ainit = true;
        if (getenv("ANCHOR")) sscanf(getenv("ANCHOR"), "%x=%x", &aa, &av);
        if (const char* e = getenv("ASCRIPT")) {
            auto save = steps;
            steps.clear();
            // reuse the parser on a temporary copy
            std::string s2 = e; size_t p = 0;
            while (p < s2.size()) {
                size_t c = s2.find(',', p);
                std::string item = s2.substr(p, c == std::string::npos ? std::string::npos : c - p);
                p = c == std::string::npos ? s2.size() : c + 1;
                size_t eq = item.find('=');
                if (eq == std::string::npos) continue;
                uint32_t b = 0; std::string names = item.substr(eq + 1) + "+"; size_t q = 0;
                while (q < names.size()) {
                    size_t pl = names.find('+', q); std::string n = names.substr(q, pl - q); q = pl + 1;
                    static const struct { const char* n; uint32_t b; } map[] = {
                        {"up", IN_UP}, {"down", IN_DOWN}, {"left", IN_LEFT}, {"right", IN_RIGHT}, {"cross", IN_SOUTH},
                        {"circle", IN_EAST}, {"square", IN_WEST}, {"triangle", IN_NORTH}, {"l1", IN_L1}, {"r1", IN_R1},
                        {"l2", IN_L2}, {"r2", IN_R2}, {"start", IN_START}, {"select", IN_BACK}};
                    for (auto& m : map) if (n == m.n) b |= m.b;
                }
                as.push_back({atoi(item.substr(0, eq).c_str()), b});
            }
            steps = save;
        }
    }
    static int after = getenv("ANCHOR_AFTER") ? atoi(getenv("ANCHOR_AFTER")) : 0;
    if (aa && anchor < 0 && frame > after && MEM_RW(aa & ~3u) == av) { anchor = frame; fprintf(stderr, "[anchor] frame %d\n", frame); }
    if (anchor >= 0) for (auto& st : as) if (st.first == frame - anchor) g_host_input.buttons = st.second;
}

// WATCH="800bb360,gp+178c" : print these RAM words whenever they change (debugging)
static void watch_ram(int frame) {
    static std::vector<std::pair<std::string, uint32_t>> w;
    static std::vector<uint32_t> last;
    static bool parsed = false;
    if (!parsed) {
        parsed = true;
        const char* e = getenv("WATCH");
        if (!e) return;
        std::string s = std::string(e) + ",";
        size_t p = 0, c;
        while ((c = s.find(',', p)) != std::string::npos) {
            std::string it = s.substr(p, c - p);
            p = c + 1;
            if (!it.empty()) w.push_back({it, 0});
        }
        last.assign(w.size(), 0xdeadbeef);
    }
    for (size_t i = 0; i < w.size(); i++) {
        const std::string& it = w[i].first;
        uint32_t a = it.rfind("gp+", 0) == 0 ? g_cpu.r[28] + (uint32_t)strtoul(it.c_str() + 3, nullptr, 16)
                                             : (uint32_t)strtoul(it.c_str(), nullptr, 16);
        uint32_t v = MEM_RW(a & ~3u);
        if (v != last[i]) {
            fprintf(stderr, "[watch f%d] %s (%08x) = %08x\n", frame, it.c_str(), a, v);
            last[i] = v;
        }
    }
}

void platform_vblank() {
    static int frames = 0;
    static int vbl = 0;
    scripted_input(++vbl);
    watch_ram(vbl);
    static int dump_at = -1;
    static char dump_fn[256];
    if (dump_at == -1) { dump_at = 0; if (getenv("RAM_DUMP_AT")) sscanf(getenv("RAM_DUMP_AT"), "%d:%255s", &dump_at, dump_fn); }
    if (dump_at && vbl == dump_at) { FILE* f = fopen(dump_fn, "wb"); if (f) { fwrite(g_ram, 1, 0x200000, f); fclose(f); } }
    static int limit = getenv("FRAMES") ? atoi(getenv("FRAMES")) : 0;
    if (limit && ++frames >= limit) {
        fprintf(stderr, "[main] frame limit reached (%d frames, %.1f s emulated)\n", frames, g_cpu.cycles / (double)CPU_HZ);
        fflush(stderr);
        _Exit(0);
    }
    static int stop_at = getenv("STOP_AT_FRAME") ? atoi(getenv("STOP_AT_FRAME")) : 0;
    static int vb = 0;
#ifdef SIGTRAP
    if (stop_at && ++vb == stop_at) raise(SIGTRAP);
#endif
    hw_frame_pacing();
}

[[noreturn]] static void fatal(const std::string& msg) {
    fprintf(stderr, "%s\n", msg.c_str());
#if defined(_WIN32) || defined(USE_SDL)
    SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, GAME_NAME, msg.c_str(), nullptr);
#endif
    exit(1);
}

// A .cue in the folder, or a folder prepared by tools/prepare_disc.py
static std::string find_disc_in(const fs::path& dir) {
    std::error_code ec;
    if (!fs::is_directory(dir, ec)) return {};
    for (auto& e : fs::directory_iterator(dir, ec)) {
        std::string ext = e.path().extension().string();
        for (auto& ch : ext) ch = (char)tolower(ch);
        if (ext == ".cue") return e.path().string();
    }
    if (fs::exists(dir / "track01.bin")) return dir.string();
    return {};
}

// Reads and checks the game executable from the open disc (empty string = ok)
static std::string check_exe(std::vector<uint8_t>& exe) {
    if (!cd_read_file(GAME_EXE_NAME, exe) || exe.size() < 0x800 || memcmp(exe.data(), "PS-X EXE", 8) != 0)
        return "This disc image does not contain " GAME_EXE_NAME ".\n"
               "This port needs the North American release of Muppet Monster Adventure (SLUS-01238).";
    uint32_t pc;
    memcpy(&pc, exe.data() + 0x10, 4);
    if (exe.size() != 692224 || pc != 0x8009c520u)
        return "This copy of " GAME_EXE_NAME " is a different build than the one this port was made from.";
    return {};
}

// ---------------------------------------------------------------- one-time disc import
#if defined(_WIN32) || defined(USE_SDL)
static SDL_Window* g_imp_win = nullptr;
static SDL_Renderer* g_imp_ren = nullptr;
static void import_progress(int done, int total) {
    SDL_Event e;
    while (SDL_PollEvent(&e)) {}
    int pct = total ? done * 100 / total : 0;
    char t[96];
    snprintf(t, sizeof t, "Importing disc into assets.dat - %d%%", pct);
    SDL_SetWindowTitle(g_imp_win, t);
    if (g_imp_ren) {
        SDL_SetRenderDrawColor(g_imp_ren, 24, 18, 32, 255);
        SDL_RenderClear(g_imp_ren);
        SDL_Rect frame = {20, 40, 440, 28}, bar = {24, 44, total ? 432 * done / total : 0, 20};
        SDL_SetRenderDrawColor(g_imp_ren, 120, 110, 140, 255);
        SDL_RenderDrawRect(g_imp_ren, &frame);
        SDL_SetRenderDrawColor(g_imp_ren, 240, 140, 40, 255);
        SDL_RenderFillRect(g_imp_ren, &bar);
        SDL_RenderPresent(g_imp_ren);
    }
    fprintf(stderr, "\r[import] %d%%", pct);
}
#else
static void import_progress(int done, int total) { fprintf(stderr, "\r[import] %d%%", total ? done * 100 / total : 0); }
#endif

static void info(const std::string& msg) {
    fprintf(stderr, "%s\n", msg.c_str());
#if defined(_WIN32) || defined(USE_SDL)
    if (!getenv("NO_MSGBOX")) SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_INFORMATION, GAME_NAME, msg.c_str(), nullptr);
#endif
}

// .cue/.bin -> assets.dat. The disc is checked first, then packed and verified sector by sector.
static void import_disc(const std::string& disc, const fs::path& pack) {
    if (!cd_init(disc.c_str())) fatal("Cannot read the disc image " + disc);
    std::vector<uint8_t> exe;
    std::string bad = check_exe(exe);
    if (!bad.empty()) fatal(bad);
#if defined(_WIN32) || defined(USE_SDL)
    SDL_Init(SDL_INIT_VIDEO);
    g_imp_win = SDL_CreateWindow("Importing disc", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, 480, 108, 0);
    if (g_imp_win) g_imp_ren = SDL_CreateRenderer(g_imp_win, -1, 0);
#endif
    std::string err;
    bool ok = cd_make_pack(pack.string().c_str(), import_progress, &err);
    fprintf(stderr, "\n");
#if defined(_WIN32) || defined(USE_SDL)
    if (g_imp_ren) SDL_DestroyRenderer(g_imp_ren);
    if (g_imp_win) SDL_DestroyWindow(g_imp_win);
    g_imp_ren = nullptr;
    g_imp_win = nullptr;
#endif
    cd_close_image();
    if (!ok) fatal("Importing the disc failed: " + err);
    std::error_code ec;
    double mb = (double)fs::file_size(pack, ec) / (1024.0 * 1024.0);
    char buf[64];
    snprintf(buf, sizeof buf, "%.0f MB", mb);
    info("Your disc was imported into assets.dat (" + std::string(buf) + ", verified lossless).\n\n"
         "The game now runs from assets.dat alone. The .cue and .bin files are no longer needed "
         "and can be deleted.");
}

int main(int argc, char** argv) {
    std::string self = argv[0];
    size_t slash = self.find_last_of("/\\");
    g_exe_dir = slash == std::string::npos ? "" : self.substr(0, slash + 1);
    config_load(argv[0]);
    fs::path base = g_exe_dir.empty() ? fs::path(".") : fs::path(g_exe_dir);
    fs::path pack = base / "assets.dat";

    // muppetmonster --import <cue> [out.dat] : build the packed file and exit
    if (argc > 2 && !strcmp(argv[1], "--import")) {
        fs::path p = argv[2];
        std::string disc = fs::is_directory(p) ? find_disc_in(p) : p.string();
        import_disc(disc, argc > 3 ? fs::path(argv[3]) : pack);
        return 0;
    }

    std::string disc;   // .cue (or prepared folder) to run from directly
    fs::path use_pack;
    std::error_code ec;
    if (argc > 1) {
        fs::path p = argv[1];
        std::string ext = p.extension().string();
        for (auto& ch : ext) ch = (char)tolower(ch);
        if (ext == ".dat") use_pack = p;
        else {
            disc = fs::is_directory(p, ec) ? find_disc_in(p) : p.string();
            if (!disc.empty() && !fs::exists(pack, ec)) { import_disc(disc, pack); use_pack = pack; disc.clear(); }
        }
    } else if (fs::exists(pack, ec)) {
        use_pack = pack;
    } else {
        std::string found = find_disc_in(base / "game");
        if (found.empty()) found = find_disc_in(base);
        if (!found.empty()) { import_disc(found, pack); use_pack = pack; }
    }

    if (!use_pack.empty()) {
        std::string err;
        if (!cd_init_pack(use_pack.string().c_str(), &err)) fatal(err);
    } else if (disc.empty() || !cd_init(disc.c_str())) {
        fatal("Muppet Monster Adventure game data not found.\n\n"
              "First run: put your .cue and .bin files (NTSC-U, SLUS-01238) next to muppetmonster.exe "
              "or in a folder named \"game\" (or drag the .cue onto the exe). They are imported once "
              "into assets.dat, after which they can be deleted.");
    }

    std::vector<uint8_t> exe;
    std::string bad = check_exe(exe);
    if (!bad.empty()) fatal(bad);

    g_gte_widescreen = g_cfg.widescreen ? 0.75f : 1.0f;
    hw_init();
    gpu_init();
    spu_init();
    mdec_init();
    sio_init();
    bios_init(g_exe_dir.c_str());
    bios_run_exe(exe.data(), exe.size());
}
