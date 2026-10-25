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

void platform_vblank() {
    static int frames = 0;
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

int main(int argc, char** argv) {
    std::string self = argv[0];
    size_t slash = self.find_last_of("/\\");
    g_exe_dir = slash == std::string::npos ? "" : self.substr(0, slash + 1);
    config_load(argv[0]);

    std::string disc;
    if (argc > 1) {
        fs::path p = argv[1];
        disc = fs::is_directory(p) ? find_disc_in(p) : p.string();
    } else {
        fs::path base = g_exe_dir.empty() ? fs::path(".") : fs::path(g_exe_dir);
        disc = find_disc_in(base / "game");
        if (disc.empty()) disc = find_disc_in(base);
    }
    if (disc.empty() || !cd_init(disc.c_str()))
        fatal("Muppet Monster Adventure disc image not found.\n\n"
              "Put your .cue and .bin files (NTSC-U, SLUS-01238) in a folder named \"game\" "
              "next to muppetmonster.exe, or drag the .cue file onto the exe.");

    std::vector<uint8_t> exe;
    if (!cd_read_file(GAME_EXE_NAME, exe) || exe.size() < 0x800 || memcmp(exe.data(), "PS-X EXE", 8) != 0)
        fatal("This disc image does not contain " GAME_EXE_NAME ".\n"
              "This port needs the North American release of Muppet Monster Adventure (SLUS-01238).");
    uint32_t pc;
    memcpy(&pc, exe.data() + 0x10, 4);
    if (exe.size() != 692224 || pc != 0x8009c520u)
        fatal("This copy of " GAME_EXE_NAME " is a different build than the one this port was made from.");

    g_gte_widescreen = g_cfg.widescreen ? 0.75f : 1.0f;
    hw_init();
    gpu_init();
    spu_init();
    mdec_init();
    sio_init();
    bios_init(g_exe_dir.c_str());
    bios_run_exe(exe.data(), exe.size());
}
