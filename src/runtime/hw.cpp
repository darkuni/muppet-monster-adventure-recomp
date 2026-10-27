// Memory map slow paths, scheduler, interrupt controller, root counters and DMA.
#include "hw.h"
#include "bios.h"
#include "log.h"
#include "config.h"
#include "hostclock.h"
#include "hostinput.h"
#include "overlay.h"
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstdarg>
#include <cstring>
#include <string>

HostInput g_host_input;
uint8_t* g_ram = nullptr;
bool g_cache_isolated = false;
bool g_in_exception = false;
uint8_t g_scratch[1024];   // data cache used as scratchpad RAM (0x1F800000)
static uint32_t g_memctrl[16];
static uint32_t g_ram_size = 0x00000b88;
static uint32_t g_cache_ctrl = 0;

// ---------------------------------------------------------------- logging
static std::string g_log_cats;
static bool g_log_all = false;
bool log_enabled(const char* cat) {
    static bool init = false;
    if (!init) {
        init = true;
        const char* e = getenv("LOG");
        if (e) { g_log_cats = std::string(",") + e + ","; g_log_all = g_log_cats.find(",all,") != std::string::npos; }
    }
    if (g_log_all) return true;
    if (g_log_cats.empty()) return false;
    return g_log_cats.find(std::string(",") + cat + ",") != std::string::npos;
}
void log_printf(const char* cat, const char* fmt, ...) {
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    fprintf(stderr, "[%s %lld] %s\n", cat, (long long)g_cpu.cycles, buf);
}

// ---------------------------------------------------------------- scheduler
extern uint32_t g_gp0_addr;
static int64_t g_ev[EV_COUNT];
static const int64_t NEVER = INT64_MAX;

static void recompute_next() {
    int64_t n = NEVER;
    for (int i = 0; i < EV_COUNT; i++) n = std::min(n, g_ev[i]);
    if (!g_in_exception && irq_pending() && (g_cpu.sr & 0x401) == 0x401) n = std::min(n, g_cpu.cycles);
    g_cpu.next_event = n;
}
void sched_at(int ev, int64_t when) { g_ev[ev] = when; recompute_next(); }
void sched_in(int ev, int64_t cycles) { sched_at(ev, g_cpu.cycles + cycles); }
void sched_cancel(int ev) { g_ev[ev] = NEVER; recompute_next(); }
bool sched_active(int ev) { return g_ev[ev] != NEVER; }

// ---------------------------------------------------------------- interrupts
static uint32_t g_istat = 0, g_imask = 0;
void irq_raise(int irq) {
    g_istat |= 1u << irq;
    irq_update();
}
bool irq_pending() { return (g_istat & g_imask) != 0; }
uint32_t irq_stat() { return g_istat; }
uint32_t irq_mask() { return g_imask; }
void irq_update() { recompute_next(); }

// ---------------------------------------------------------------- frame pacing
void hw_frame_pacing() {
    using namespace std::chrono;
    static steady_clock::time_point next = steady_clock::now();
    static bool init = false;
    static int frames = 0;
    static steady_clock::time_point perf_t0 = steady_clock::now();
    static int64_t perf_c0 = 0;
    if (!init) { init = true; next = steady_clock::now(); }
    int speed = std::max(1, g_cfg.speed);
    if (g_host_input.buttons & IN_TURBO) speed = std::min(8, speed == 1 ? 4 : speed * 2);
    if (getenv("NO_THROTTLE")) speed = 1000;
    const double frame_us = 1e6 / 59.826 / speed;
    next += microseconds((int64_t)frame_us);
    auto now = steady_clock::now();
    if (next > now) precise_sleep_until(next);
    else if (now - next > milliseconds(100)) next = now;   // fell behind: don't try to catch up
    if (++frames >= 60) {
        auto t = steady_clock::now();
        double secs = duration_cast<duration<double>>(t - perf_t0).count();
        extern int g_game_frames;
        static int last_game_frames = 0;
        double fps = (g_game_frames - last_game_frames) / secs;
        last_game_frames = g_game_frames;
        // game frames per second, and emulation speed relative to a real console
        snprintf(g_perf_text, sizeof g_perf_text, "%.0f FPS  %.0f%%", fps, frames / secs / 59.826 * 100.0);
        if (log_enabled("perf"))
            fprintf(stderr, "[perf] %.1f vblank/s, %.1f game fps, %.1f MHz guest\n", frames / secs, fps,
                    (g_cpu.cycles - perf_c0) / secs / 1e6);
        perf_c0 = g_cpu.cycles;
        perf_t0 = t;
        frames = 0;
    }
}

char g_perf_text[128];

// ---------------------------------------------------------------- root counters
struct Timer {
    uint32_t count, mode, target;
    int64_t last;          // cycles at last update
    int64_t frac;          // fractional source ticks (x1000) carried over
    bool irq_done;         // one-shot mode: IRQ already fired
};
static Timer g_tm[3];

static int64_t timer_period_fp(int t) {   // CPU cycles per counter tick, x1000
    uint32_t src = (g_tm[t].mode >> 8) & 3;
    if (t == 0 && (src & 1)) {
        // dot clock: video clock / divider -> CPU cycles per dot = divider * 33.8688 / 53.693175
        return (int64_t)gpu_dot_divider() * 630779 / 1000;
    }
    if (t == 1 && (src & 1)) return CYCLES_PER_LINE_FP;
    if (t == 2 && (src & 2)) return 8000;
    return 1000;
}

static void timer_irq(int t) {
    Timer& m = g_tm[t];
    bool repeat = (m.mode & 0x40) != 0;
    if (!repeat && m.irq_done) return;
    m.irq_done = true;
    if (m.mode & 0x80) m.mode ^= 0x400;     // toggle mode
    else m.mode &= ~0x400u;                  // pulse: IRQ request (active low)
    irq_raise(IRQ_TIMER0 + t);
    if (!(m.mode & 0x80)) m.mode |= 0x400;
}

static void timer_update(int t) {
    Timer& m = g_tm[t];
    int64_t now = g_cpu.cycles;
    int64_t el = now - m.last;
    m.last = now;
    if (el <= 0) return;
    int64_t per = timer_period_fp(t);
    int64_t acc = m.frac + el * 1000;
    int64_t ticks = acc / per;
    m.frac = acc % per;
    if (!ticks) return;
    uint32_t tgt = m.target ? m.target : 0x10000;
    bool reset_tgt = (m.mode & 8) != 0;
    // walk crossings (bounded: ticks is small between updates)
    while (ticks > 0) {
        uint32_t lim = reset_tgt ? tgt : 0x10000;
        int64_t to_tgt = (int64_t)m.target - m.count;
        int64_t to_ovf = 0x10000 - (int64_t)m.count;
        if (to_tgt <= 0) to_tgt = 0x7fffffff;
        int64_t step = std::min<int64_t>(ticks, std::min(to_tgt, to_ovf));
        m.count += (uint32_t)step;
        ticks -= step;
        if (m.count == m.target && step == to_tgt) {
            m.mode |= 0x800;
            if (m.mode & 0x10) timer_irq(t);
            if (reset_tgt) m.count = 0;
        }
        if (m.count >= 0x10000) {
            m.mode |= 0x1000;
            if (m.mode & 0x20) timer_irq(t);
            m.count = 0;
        }
        (void)lim;
    }
}

static void timer_schedule(int t) {
    Timer& m = g_tm[t];
    if (!(m.mode & 0x30)) { sched_cancel(EV_TIMER0 + t); return; }
    int64_t ticks = 0x7fffffffffffLL;
    if ((m.mode & 0x10) && m.target > m.count) ticks = m.target - m.count;
    if ((m.mode & 0x10) && m.target <= m.count && !(m.mode & 8)) ticks = std::min<int64_t>(ticks, 0x10000 - m.count + m.target);
    if (m.mode & 0x20) ticks = std::min<int64_t>(ticks, 0x10000 - m.count);
    if ((m.mode & 0x10) && (m.mode & 8) && m.target <= m.count) ticks = std::min<int64_t>(ticks, 0x10000 - m.count + m.target);
    int64_t cyc = (ticks * timer_period_fp(t) - m.frac + 999) / 1000;
    sched_in(EV_TIMER0 + t, std::max<int64_t>(1, cyc));
}

void timer_event(int t) {
    timer_update(t);
    timer_schedule(t);
}

uint32_t timer_read(uint32_t off) {
    int t = (off >> 4) & 3;
    if (t > 2) return 0;
    timer_update(t);
    switch (off & 0xc) {
        case 0: return g_tm[t].count & 0xffff;
        case 4: { uint32_t v = g_tm[t].mode; g_tm[t].mode &= ~0x1800u; return v; }
        case 8: return g_tm[t].target;
    }
    return 0;
}

void timer_write(uint32_t off, uint32_t v) {
    int t = (off >> 4) & 3;
    if (t > 2) return;
    timer_update(t);
    switch (off & 0xc) {
        case 0: g_tm[t].count = v & 0xffff; break;
        case 4:
            g_tm[t].mode = (v & 0x3ff) | 0x400;
            g_tm[t].count = 0;
            g_tm[t].frac = 0;
            g_tm[t].irq_done = false;
            break;
        case 8: g_tm[t].target = v & 0xffff; break;
    }
    timer_schedule(t);
}

// ---------------------------------------------------------------- DMA
struct DmaCh { uint32_t madr, bcr, chcr; };
static DmaCh g_dma[7];
static uint32_t g_dpcr = 0x07654321, g_dicr = 0;

static void dicr_update_master() {
    bool old = (g_dicr >> 31) & 1;
    bool m = (g_dicr & 0x8000) || ((g_dicr & 0x800000) && ((g_dicr >> 24) & (g_dicr >> 16) & 0x7f));
    g_dicr = (g_dicr & 0x7fffffffu) | (m ? 0x80000000u : 0);
    if (m && !old) irq_raise(IRQ_DMA);
}

static void dma_finish(int ch) {
    g_dma[ch].chcr &= ~0x11000000u;
    if (g_dicr & (1u << (16 + ch))) g_dicr |= 1u << (24 + ch);
    dicr_update_master();
    LOG("dma", "ch%d irq flag, dicr=%08x", ch, g_dicr);
}

static void dma_run(int ch) {
    DmaCh& d = g_dma[ch];
    uint32_t mode = (d.chcr >> 9) & 3;
    bool from_ram = d.chcr & 1;
    int step = (d.chcr & 2) ? -4 : 4;
    uint32_t addr = d.madr & 0x1ffffc;
    const uint32_t dma_start_addr = addr;
    int64_t cost = 0;
    if (mode == 2) {
        // linked list (GPU); an address with bit 23 set (the 0xFFFFFF terminator) ends it at once
        int guard = 0;
        for (uint32_t a = d.madr; !(a & 0x800000);) {
            addr = a & 0x1ffffc;
            uint32_t hdr;
            memcpy(&hdr, g_ram + addr, 4);
            uint32_t n = hdr >> 24;
            for (uint32_t i = 0; i < n; i++) {
                uint32_t w;
                addr = (addr + 4) & 0x1ffffc;
                memcpy(&w, g_ram + addr, 4);
                g_gp0_addr = 0x80000000u | addr;
                gpu_write(0, w);
            }
            g_gp0_addr = 0xffffffffu;
            cost += n + 1;
            a = hdr & 0xffffff;
            if (++guard > 1000000) { LOG("dma", "GPU linked list loop"); break; }
        }
        d.madr = 0xffffff;
    } else {
        uint32_t words;
        if (mode == 0) { words = d.bcr & 0xffff; if (!words) words = 0x10000; }
        else { words = (d.bcr & 0xffff) * (d.bcr >> 16); }
        if (ch == 6) {
            // OTC: reverse-linked empty ordering table
            for (uint32_t i = 0; i < words; i++) {
                uint32_t v = (i == words - 1) ? 0xffffffu : ((addr - 4) & 0x1fffff);
                memcpy(g_ram + addr, &v, 4);
                addr = (addr - 4) & 0x1ffffc;
            }
        } else {
            for (uint32_t i = 0; i < words; i++) {
                if (from_ram) {
                    uint32_t w;
                    memcpy(&w, g_ram + addr, 4);
                    switch (ch) {
                        case 0: mdec_dma_in(w); break;
                        case 2: g_gp0_addr = 0x80000000u | addr; gpu_write(0, w); g_gp0_addr = 0xffffffffu; break;
                        case 4: spu_dma_write(w); break;
                        default: break;
                    }
                } else {
                    uint32_t w = 0;
                    switch (ch) {
                        case 1: w = mdec_dma_out(); break;
                        case 2: w = gpu_read(0); break;
                        case 3: w = cd_dma_read(); break;
                        case 4: w = spu_dma_read(); break;
                        default: break;
                    }
                    memcpy(g_ram + addr, &w, 4);
                }
                addr = (addr + step) & 0x1ffffc;
            }
        }
        if (mode == 1) { d.madr = addr; d.bcr &= 0xffff; }
        cost = words;
    }
    LOG("dma", "ch%d done: madr=%08x bcr=%08x chcr=%08x", ch, d.madr, d.bcr, d.chcr);
    if (ch == 3 && log_enabled("cdmap")) {
        extern int cd_sector_lba();
        fprintf(stderr, "[cdmap] lba %d -> %08x\n", cd_sector_lba(), dma_start_addr);
    }
    // The data moved instantly; the channel stays busy for a while like real hardware.
    sched_in(EV_DMA0 + ch, std::max<int64_t>(16, cost));
}

void dma_event(int ch) {
    sched_cancel(EV_DMA0 + ch);
    dma_finish(ch);
}

static bool g_dma_waiting[7];

void dma_device_ready(int ch) {
    if (g_dma_waiting[ch]) { g_dma_waiting[ch] = false; extern void dma_check_start_ext(int); dma_check_start_ext(ch); }
}

static void dma_check_start(int ch) {
    DmaCh& d = g_dma[ch];
    if (!(d.chcr & 0x01000000)) return;
    if (!((g_dpcr >> (ch * 4 + 3)) & 1)) return;
    if (ch == 1 && !mdec_out_ready()) { g_dma_waiting[1] = true; return; }   // waits for decoded macroblocks
    uint32_t mode = (d.chcr >> 9) & 3;
    if (mode == 0 && !(d.chcr & 0x10000000) && ch != 6) return;    // burst waits for the trigger bit
    if (ch == 6) d.chcr = (d.chcr & 0x51000000u) | 2;                 // OTC: fixed direction/step
    LOG("dma", "ch%d start: madr=%08x bcr=%08x chcr=%08x", ch, d.madr, d.bcr, d.chcr);
    d.chcr &= ~0x10000000u;
    dma_run(ch);
}

void dma_check_start_ext(int ch) { dma_check_start(ch); }

uint32_t dma_read(uint32_t off) {
    if (off == 0x70) return g_dpcr;
    if (off == 0x74) return g_dicr;
    int ch = off >> 4;
    if (ch > 6) return 0;
    switch (off & 0xc) {
        case 0: return g_dma[ch].madr;
        case 4: return g_dma[ch].bcr;
        case 8: return g_dma[ch].chcr;
    }
    return 0;
}

void dma_write(uint32_t off, uint32_t v) {
    if (off == 0x70) { g_dpcr = v; for (int i = 0; i < 7; i++) dma_check_start(i); return; }
    if (off == 0x74) {
        uint32_t ack = v & 0x7f000000u;
        g_dicr = (g_dicr & 0xff000000u & ~ack) | (v & 0x00ff803fu);
        dicr_update_master();
        LOG("dma", "dicr write %08x -> %08x", v, g_dicr);
        return;
    }
    int ch = off >> 4;
    if (ch > 6) return;
    switch (off & 0xc) {
        case 0: g_dma[ch].madr = v & 0xffffff; break;
        case 4: g_dma[ch].bcr = v; break;
        case 8:
            if (ch == 6) v = (v & 0x51000000u) | 2;
            g_dma[ch].chcr = v;
            dma_check_start(ch);
            break;
    }
}

// ---------------------------------------------------------------- memory map
static void io_write(uint32_t p, uint32_t v, int size);
static uint32_t io_read(uint32_t p, int size);

uint32_t mem_read_slow(uint32_t a, int size) {
    uint32_t p = a & 0x1fffffffu;
    if (p >= 0x1f800000u && p < 0x1f800400u) {
        uint32_t v = 0;
        memcpy(&v, g_scratch + (p & 0x3ff), size);
        return v;
    }
    if (p >= 0x1f801000u && p < 0x1f803000u) return io_read(p, size);
    if (p >= 0x1fc00000u && p < 0x1fc80000u) return 0;          // BIOS ROM (HLE: empty)
    if (p >= 0x1f000000u && p < 0x1f800000u) return 0xffffffffu; // expansion 1: nothing connected
    if (a == 0xfffe0130u) return g_cache_ctrl;
    LOG("mem", "read%d from unmapped %08x", size * 8, a);
    return 0;
}

void mem_write_slow(uint32_t a, uint32_t v, int size) {
    uint32_t p = a & 0x1fffffffu;
    if (p >= 0x1f800000u && p < 0x1f800400u) {
        if (!g_cache_isolated) memcpy(g_scratch + (p & 0x3ff), &v, size);
        return;
    }
    if (g_cache_isolated) return;
    if (p >= 0x1f801000u && p < 0x1f803000u) { io_write(p, v, size); return; }
    if (a == 0xfffe0130u) { g_cache_ctrl = v; return; }
    LOG("mem", "write%d %08x to unmapped %08x", size * 8, v, a);
}

static uint32_t io_read(uint32_t p, int size) {
    uint32_t off = p - 0x1f801000u;
    if (off < 0x24) return g_memctrl[off >> 2];
    if (off >= 0x40 && off < 0x60) return sio_read(off, size);
    if (off == 0x60) return g_ram_size;
    if ((off & ~3u) == 0x70) return g_istat >> ((off & 3) * 8);
    if ((off & ~3u) == 0x74) return g_imask >> ((off & 3) * 8);
    if (off >= 0x80 && off < 0x100) {
        uint32_t w = dma_read((off - 0x80) & ~3u);
        return w >> ((off & 3) * 8);
    }
    if (off >= 0x100 && off < 0x130) return timer_read(off - 0x100);
    if (off >= 0x800 && off < 0x804) {
        uint32_t v = cd_read(off & 3);
        if (size > 1) {   // 16/32-bit reads of the CD data port return repeated bytes
            for (int i = 1; i < size; i++) v |= (uint32_t)cd_read(off & 3) << (8 * i);
        }
        return v;
    }
    if (off == 0x810) return gpu_read(0);
    if (off == 0x814) return gpu_read(1);
    if (off == 0x820) return mdec_read(0);
    if (off == 0x824) return mdec_read(1);
    if (off >= 0xc00 && off < 0x1000) {
        uint32_t v = spu_read(off - 0xc00);
        if (size == 4) v |= (uint32_t)spu_read(off - 0xc00 + 2) << 16;
        return v;
    }
    if (off >= 0x1000) return 0xff;   // expansion 2
    LOG("io", "read%d %08x", size * 8, p);
    return 0;
}

static void io_write(uint32_t p, uint32_t v, int size) {
    uint32_t off = p - 0x1f801000u;
    if (off < 0x24) { g_memctrl[off >> 2] = v; return; }
    if (off >= 0x40 && off < 0x60) { sio_write(off, v, size); return; }
    if (off == 0x60) { g_ram_size = v; return; }
    if ((off & ~3u) == 0x70 || (off & ~3u) == 0x74) {
        // byte/halfword accesses only touch their own bits
        int sh = (off & 3) * 8;
        uint32_t mask = (size == 4 ? 0xffffffffu : size == 2 ? 0xffffu : 0xffu) << sh;
        uint32_t w = (v << sh) & mask;
        if ((off & ~3u) == 0x70) g_istat &= w | ~mask;                       // write 0 to acknowledge
        else g_imask = ((g_imask & ~mask) | w) & 0x7ff;
        irq_update();
        return;
    }
    if (off >= 0x80 && off < 0x100) {
        uint32_t reg = (off - 0x80) & ~3u;
        if (size < 4) {
            uint32_t cur = dma_read(reg);
            int sh = (off & 3) * 8;
            uint32_t mask = (size == 1 ? 0xffu : 0xffffu) << sh;
            // DICR: bytes that were not written must not acknowledge IRQ flags (write-1-to-clear)
            if (reg == 0x74) cur &= 0x00ffffffu;
            v = (cur & ~mask) | ((v << sh) & mask);
        }
        dma_write(reg, v);
        return;
    }
    if (off >= 0x100 && off < 0x130) { timer_write(off - 0x100, v); return; }
    if (off >= 0x800 && off < 0x804) { cd_write(off & 3, (uint8_t)v); return; }
    if (off == 0x810) { gpu_write(0, v); return; }
    if (off == 0x814) { gpu_write(1, v); return; }
    if (off == 0x820) { mdec_write(0, v); return; }
    if (off == 0x824) { mdec_write(1, v); return; }
    if (off >= 0xc00 && off < 0x1000) {
        spu_write(off - 0xc00, (uint16_t)v);
        if (size == 4) spu_write(off - 0xc00 + 2, (uint16_t)(v >> 16));
        return;
    }
    if (off >= 0x1000) return;   // expansion 2 (POST display etc.)
    LOG("io", "write%d %08x = %08x", size * 8, p, v);
}

// ---------------------------------------------------------------- events
int g_game_frames = 0;
static int64_t g_frame_cycles = 0;
extern void platform_vblank();

static int64_t g_last_vblank = 0;
int64_t hw_vblank_time() { return g_last_vblank; }

static void vblank_event() {
    g_last_vblank = g_ev[EV_VBLANK];
    // One NTSC field: 263 lines
    g_frame_cycles = (int64_t)LINES_PER_FRAME * CYCLES_PER_LINE_FP / 1000;
    sched_at(EV_VBLANK, g_ev[EV_VBLANK] + g_frame_cycles);
    irq_raise(IRQ_VBLANK);
    gpu_vblank();
    platform_vblank();
}

void rt_events(Cpu* c) {
    for (;;) {
        int64_t now = c->cycles;
        int best = -1;
        int64_t bt = INT64_MAX;
        for (int i = 0; i < EV_COUNT; i++)
            if (g_ev[i] <= now && g_ev[i] < bt) { bt = g_ev[i]; best = i; }
        if (best < 0) break;
        switch (best) {
            case EV_VBLANK: vblank_event(); break;
            case EV_TIMER0: case EV_TIMER1: case EV_TIMER2: timer_event(best - EV_TIMER0); break;
            case EV_CDROM: case EV_CDROM_READ: case EV_CDROM_AUDIO: cd_event(best); break;
            case EV_DMA0: case EV_DMA1: case EV_DMA2: case EV_DMA3: case EV_DMA4: case EV_DMA5: case EV_DMA6:
                dma_event(best - EV_DMA0); break;
            case EV_SIO0: sio_event(); break;
            case EV_SPU: spu_event(); break;
            default: sched_cancel(best); break;
        }
    }
    recompute_next();
    if (!g_in_exception && irq_pending() && (c->sr & 0x401) == 0x401) bios_exception(c);
}

void hw_init() {
    g_ram = (uint8_t*)calloc(1, 0x200000);
    for (int i = 0; i < EV_COUNT; i++) g_ev[i] = NEVER;
    for (int t = 0; t < 3; t++) { g_tm[t] = Timer{}; g_tm[t].mode = 0x400; }
    g_cpu.next_event = NEVER;
    sched_in(EV_VBLANK, (int64_t)LINES_PER_FRAME * CYCLES_PER_LINE_FP / 1000);
}
