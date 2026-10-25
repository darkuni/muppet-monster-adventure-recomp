// HLE BIOS: the kernel functions the game calls through the A0/B0/C0 vectors, the event
// system, BIOS threads (each guest thread runs on its own host thread; only one runs at a
// time), the interrupt path including libetc's custom exit hook, and memory card access.
//
// Kernel structures live in guest RAM where the real BIOS keeps them (the table of tables
// at 0x100), because the game reads the thread control blocks directly.
#include "bios.h"
#include "hw.h"
#include "game.h"
#include "log.h"
#include "gpu.h"
#include <algorithm>
#include <condition_variable>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <algorithm>
#include <thread>
#include <vector>

// ---------------------------------------------------------------- kernel memory layout
static const uint32_t K_TOT = 0x80000100u;     // table of tables
static const uint32_t K_EXCB = 0x8000A000u;    // 4 priority chains, 8 bytes each
static const uint32_t K_PCB = 0x8000A040u;     // process control block: [0] = current TCB
static const uint32_t K_TCB = 0x8000A100u;     // TCBs, 0xC0 bytes each
static const uint32_t K_EVCB = 0x8000A500u;    // event control blocks, 0x1C bytes each
static const uint32_t K_DCB = 0x8000A800u;     // device control blocks, 0x50 bytes each ("cdrom", "bu")
static const uint32_t K_DCB_NAMES = 0x8000A8C0u;
static const uint32_t K_FCB = 0x8000A900u;     // scratch file control block for device calls
static const uint32_t DCB_SIZE = 0x50;
static const uint32_t DEV_BU_FIRSTFILE = 0xbfc20000u + 0xf0 * 4;   // B0:F0 (private): "bu" device firstfile
static const uint32_t K_EXC_STACK = 0x8000EFF0u;
static const uint32_t TCB_SIZE = 0xC0, EVCB_SIZE = 0x1C;

// ---------------------------------------------------------------- guest memory helpers
uint32_t gw(uint32_t a) { return MEM_RW(a); }
void sw_(uint32_t a, uint32_t v) { MEM_WW(a, v); }
extern uint8_t g_scratch[1024];
static bool is_scratch(uint32_t a) { return (a & 0x1ffffc00u) == 0x1f800000u; }
uint8_t* guest_ptr(uint32_t a) { return is_scratch(a) ? g_scratch + (a & 0x3ff) : g_ram + (a & 0x1fffff); }
std::string_view guest_str(uint32_t a) {
    if (!a) return {};
    const char* p = (const char*)guest_ptr(a);
    size_t lim = is_scratch(a) ? 0x400 - (a & 0x3ff) : 4096;   // stack/strings may live in the scratchpad
    size_t n = 0;
    while (n < lim && p[n]) n++;
    return std::string_view(p, n);
}
void guest_strcpy_out(uint32_t a, const char* s) {
    do { MEM_WB(a++, (uint8_t)*s); } while (*s++);
}

struct RfeUnwind {};
struct ThreadKill {};

uint32_t guest_call(Cpu* c, uint32_t addr, uint32_t a0, uint32_t a1, uint32_t a2, uint32_t a3) {
    uint32_t save[32];
    memcpy(save, c->r, sizeof save);
    uint32_t hi = c->hi, lo = c->lo;
    c->r[4] = a0; c->r[5] = a1; c->r[6] = a2; c->r[7] = a3;
    c->r[31] = 0;   // a guest function that returns does so through its ra; the host just regains control
    rt_call(c, addr);
    uint32_t v0 = c->r[2];
    memcpy(c->r, save, sizeof save);
    c->hi = hi; c->lo = lo;
    return v0;
}

// ---------------------------------------------------------------- state
static uint32_t g_custom_exit = 0;        // jmp_buf address from SetCustomExitFromException
static bool g_clear_rcnt[4] = {true, true, true, true};
static bool g_clear_pad = true;
static uint32_t g_heap_base = 0, g_heap_size = 0;
static uint32_t g_kheap = 0, g_kheap_end = 0;   // kernel heap (SysInitMemory / alloc_kernel_memory)

// ---------------------------------------------------------------- threads
struct HostThread {
    std::thread th;
    bool started = false;
    unsigned gen = 0;        // bumped when the TCB's host thread is retired; stale threads exit
};
static HostThread g_ht[SYS_TCBS];
static std::mutex g_bmx;
static std::condition_variable g_bcv;
static int g_running = 0;     // host thread that holds the baton
static int g_cur = 0;         // current TCB (what PCB points at)
static thread_local int t_me = 0;
static thread_local unsigned t_gen = 0;

// The kernel tables are found through the table of tables, which the game may re-point
// (PsyQ's SetConf allocates new event/thread tables from the kernel heap).
static uint32_t tcb_addr(int i) { return gw(K_TOT + 0x10) + i * TCB_SIZE; }
static int tcb_count() { return std::min<int>(SYS_TCBS, (int)(gw(K_TOT + 0x14) / TCB_SIZE)); }
static uint32_t pcb_addr() { return gw(K_TOT + 0x08); }
static uint32_t excb_addr() { return gw(K_TOT + 0x00); }

static void save_ctx(Cpu* c, int i) {
    uint32_t t = tcb_addr(i);
    for (int r = 0; r < 32; r++) sw_(t + 8 + r * 4, c->r[r]);
    sw_(t + 0x8c, c->hi);
    sw_(t + 0x90, c->lo);
    sw_(t + 0x94, (c->sr & ~0x3fu) | ((c->sr << 2) & 0x3cu));   // as pushed by an exception
}
static void load_ctx(Cpu* c, int i) {
    uint32_t t = tcb_addr(i);
    for (int r = 1; r < 32; r++) c->r[r] = gw(t + 8 + r * 4);
    c->r[0] = 0;
    c->hi = gw(t + 0x8c);
    c->lo = gw(t + 0x90);
    uint32_t sr = gw(t + 0x94);
    c->sr = (sr & ~0xfu) | ((sr >> 2) & 0xfu);                  // rfe
    g_cache_isolated = (c->sr & 0x10000u) != 0;
    irq_update();
}

static void host_thread_main(int idx, unsigned gen);

// Hand the CPU to thread `to` and block until someone hands it back to us.
static void host_switch(int to) {
    gfx_release_context();   // the GL context follows the CPU to the other host thread
    std::unique_lock<std::mutex> lk(g_bmx);
    int me = t_me;
    unsigned my_gen = t_gen;
    g_running = to;
    if (!g_ht[to].started) {
        if (g_ht[to].th.joinable()) g_ht[to].th.detach();
        g_ht[to].started = true;
        g_ht[to].th = std::thread(host_thread_main, to, g_ht[to].gen);
    }
    g_bcv.notify_all();
    g_bcv.wait(lk, [&] { return (g_running == me && g_ht[me].gen == my_gen) || g_ht[me].gen != my_gen; });
    if (g_ht[me].gen != my_gen) throw ThreadKill();
    lk.unlock();
    gfx_acquire_context();
}

static void host_thread_main(int idx, unsigned gen) {
    t_me = idx;
    t_gen = gen;
    {
        std::unique_lock<std::mutex> lk(g_bmx);
        g_bcv.wait(lk, [&] { return (g_running == idx && g_ht[idx].gen == gen) || g_ht[idx].gen != gen; });
        if (g_ht[idx].gen != gen) return;
    }
    gfx_acquire_context();
    Cpu* c = &g_cpu;
    try {
        load_ctx(c, idx);
        uint32_t pc = gw(tcb_addr(idx) + 0x88);
        LOG("thread", "thread %d starts at %08x sp=%08x", idx, pc, c->r[29]);
        rt_call(c, pc);
        LOG("thread", "thread %d entry function returned; idling", idx);
        for (;;) { c->cycles += 64; CHECK_EVENTS(c); }
    } catch (ThreadKill&) {
        LOG("thread", "host thread for TCB %d ended", idx);
    }
}

static void set_current(int i) {
    g_cur = i;
    sw_(pcb_addr(), tcb_addr(i));
}

static uint32_t bios_open_thread(Cpu* c, uint32_t pc, uint32_t sp, uint32_t gp) {
    for (int i = 0; i < tcb_count(); i++) {
        uint32_t t = tcb_addr(i);
        if (gw(t) == 0x4000) continue;
        if (g_ht[i].started) {
            // a closed thread's host thread may still be parked: retire it
            std::lock_guard<std::mutex> lk(g_bmx);
            g_ht[i].gen++;
            g_ht[i].started = false;
            g_bcv.notify_all();
        }
        for (uint32_t k = 0; k < TCB_SIZE; k += 4) sw_(t + k, 0);
        sw_(t, 0x4000);
        sw_(t + 8 + 29 * 4, sp);
        sw_(t + 8 + 30 * 4, sp);
        sw_(t + 8 + 28 * 4, gp);
        sw_(t + 0x88, pc);
        sw_(t + 0x94, 0x40000404u);   // interrupts on (after rfe), COP2 usable
        LOG("thread", "OpenThread(%08x, sp=%08x, gp=%08x) -> %d", pc, sp, gp, i);
        return 0xff000000u | i;
    }
    return 0xffffffffu;
}

static uint32_t bios_change_thread(Cpu* c, uint32_t h) {
    int i = h & 0xffff;
    if (i >= tcb_count() || gw(tcb_addr(i)) != 0x4000) return 0;
    if (i == g_cur) return 1;
    LOG("thread", "ChangeThread %d -> %d%s", g_cur, i, g_in_exception ? " (in exception)" : "");
    if (g_in_exception) { set_current(i); return 1; }
    int me = g_cur;
    c->r[2] = 1;
    save_ctx(c, me);
    set_current(i);
    host_switch(i);
    load_ctx(c, me);
    return 1;
}

// ---------------------------------------------------------------- events
static uint32_t evcb(int i) { return gw(K_TOT + 0x20) + i * EVCB_SIZE; }
static int evcb_count() { return (int)(gw(K_TOT + 0x24) / EVCB_SIZE); }

static uint32_t bios_open_event(uint32_t cls, uint32_t spec, uint32_t mode, uint32_t func) {
    for (int i = 0; i < evcb_count(); i++) {
        uint32_t e = evcb(i);
        if (gw(e + 4) != 0) continue;
        sw_(e + 0, cls); sw_(e + 4, 0x1000); sw_(e + 8, spec); sw_(e + 0xc, mode); sw_(e + 0x10, func);
        LOG("event", "OpenEvent(%08x, %04x, %04x, %08x) -> %d", cls, spec, mode, func, i);
        return 0xf1000000u | i;
    }
    return 0xffffffffu;
}

void bios_deliver_event(Cpu* c, uint32_t cls, uint32_t spec) {
    for (int i = 0; i < evcb_count(); i++) {
        uint32_t e = evcb(i);
        if (gw(e + 4) != 0x2000 || gw(e) != cls || gw(e + 8) != spec) continue;
        uint32_t mode = gw(e + 0xc);
        if (mode == 0x2000) sw_(e + 4, 0x4000);
        else if (mode == 0x1000 && gw(e + 0x10)) guest_call(c, gw(e + 0x10));
    }
}

static uint32_t ev_index(uint32_t h) { return h & 0xffff; }

// ---------------------------------------------------------------- printf
static std::string guest_format(Cpu* c, uint32_t fmt_addr, int first_arg) {
    int argn = first_arg;   // index into a0..a3 then stack
    auto next = [&]() -> uint32_t {
        uint32_t v = argn < 4 ? c->r[4 + argn] : gw(c->r[29] + argn * 4);
        argn++;
        return v;
    };
    std::string out;
    std::string_view f = guest_str(fmt_addr);
    for (size_t i = 0; i < f.size(); i++) {
        char ch = f[i];
        if (ch != '%') { out += ch; continue; }
        std::string spec = "%";
        i++;
        while (i < f.size() && strchr("-+ #0123456789.", f[i])) spec += f[i++];
        while (i < f.size() && (f[i] == 'l' || f[i] == 'h')) i++;
        if (i >= f.size()) break;
        char conv = f[i];
        char buf[256];
        switch (conv) {
            case 'd': case 'i': snprintf(buf, sizeof buf, (spec + "d").c_str(), (int)next()); out += buf; break;
            case 'u': case 'x': case 'X': case 'o': case 'c':
                snprintf(buf, sizeof buf, (spec + conv).c_str(), (unsigned)next()); out += buf; break;
            case 'p': snprintf(buf, sizeof buf, "%08x", (unsigned)next()); out += buf; break;
            case 's': { std::string s(guest_str(next())); snprintf(buf, sizeof buf, (spec + "s").c_str(), s.c_str()); out += buf; break; }
            case '%': out += '%'; break;
            default: out += spec; out += conv; break;
        }
    }
    return out;
}

// ---------------------------------------------------------------- memory card file handles
static uint32_t g_card_last_error = 0;

static void card_done_events(Cpu* c, uint32_t spec) {
    LOG("card", "card event %04x", spec);
    bios_deliver_event(c, 0xf4000001u, spec);   // SwCARD
    bios_deliver_event(c, 0xf0000011u, spec);   // HwCARD
}
// Asynchronous card operations complete later, from the card interrupt: libcard/libmcrd clear
// their event flags after starting a transfer and only then wait, so completion must not be
// signaled inside the call.
static std::vector<std::pair<int64_t, uint32_t>> g_card_pending;   // (due cycle, event spec)
static void card_async_done(Cpu* c, uint32_t spec, int sectors = 1) {
    // a 128-byte sector takes about 1/100 s over the serial link
    g_card_pending.push_back({c->cycles + (int64_t)std::max(1, sectors) * (CPU_HZ / 100), spec});
}
static bool card_busy() { return !g_card_pending.empty(); }
static void card_deliver_pending(Cpu* c) {
    while (!g_card_pending.empty() && c->cycles >= g_card_pending.front().first) {
        uint32_t spec = g_card_pending.front().second;
        g_card_pending.erase(g_card_pending.begin());
        card_done_events(c, spec);
    }
}
// A new transfer discards completions that were delivered but not yet consumed (TestEvent style)
static void card_undeliver() {
    for (int i = 0; i < evcb_count(); i++) {
        uint32_t e = evcb(i);
        if (gw(e) == 0xf4000001u && gw(e + 4) == 0x4000) sw_(e + 4, 0x2000);
    }
}
static bool g_fd_async[16];

static int card_slot(uint32_t port) { return (port & 0x10) ? 1 : 0; }

// ---------------------------------------------------------------- A0/B0/C0
static void unknown(Cpu* c, int tab, int fn) {
    static bool seen[3][256];
    if (!seen[tab][fn & 255]) {
        seen[tab][fn & 255] = true;
        fprintf(stderr, "[bios] unimplemented %X0:%02X (a0=%08x a1=%08x ra=%08x)\n", 0xa + tab, fn, c->r[4], c->r[5], c->r[31]);
    }
    c->r[2] = 0;
}

static void bios_a0(Cpu* c, int fn) {
    uint32_t a0 = c->r[4], a1 = c->r[5], a2 = c->r[6];
    uint32_t& v0 = c->r[2];
    switch (fn) {
        case 0x13: {   // setjmp(buf)
            sw_(a0 + 0, c->r[31]); sw_(a0 + 4, c->r[29]); sw_(a0 + 8, c->r[30]);
            for (int i = 0; i < 8; i++) sw_(a0 + 12 + i * 4, c->r[16 + i]);
            sw_(a0 + 44, c->r[28]);
            v0 = 0;
            break;
        }
        case 0x15: {   // strcat
            uint32_t d = a0;
            while (MEM_RB(d)) d++;
            uint8_t ch;
            do { ch = MEM_RB(a1++); MEM_WB(d++, ch); } while (ch);
            v0 = a0;
            break;
        }
        case 0x17: {   // strcmp
            int r = 0;
            for (;; a0++, a1++) {
                int x = MEM_RB(a0), y = MEM_RB(a1);
                if (x != y) { r = x - y; break; }
                if (!x) break;
            }
            v0 = (uint32_t)r;
            break;
        }
        case 0x18: {   // strncmp
            int r = 0;
            for (uint32_t i = 0; i < a2; i++, a0++, a1++) {
                int x = MEM_RB(a0), y = MEM_RB(a1);
                if (x != y) { r = x - y; break; }
                if (!x) break;
            }
            v0 = (uint32_t)r;
            break;
        }
        case 0x19: {   // strcpy
            uint32_t d = a0;
            uint8_t ch;
            do { ch = MEM_RB(a1++); MEM_WB(d++, ch); } while (ch);
            v0 = a0;
            break;
        }
        case 0x1a: {   // strncpy
            uint32_t i = 0;
            for (; i < a2; i++) { uint8_t ch = MEM_RB(a1 + i); MEM_WB(a0 + i, ch); if (!ch) break; }
            for (; i < a2; i++) MEM_WB(a0 + i, 0);
            v0 = a0;
            break;
        }
        case 0x1b: { uint32_t n = 0; while (MEM_RB(a0 + n)) n++; v0 = n; break; }   // strlen
        case 0x25: v0 = (a0 >= 'a' && a0 <= 'z') ? a0 - 32 : a0; break;            // toupper
        case 0x26: v0 = (a0 >= 'A' && a0 <= 'Z') ? a0 + 32 : a0; break;            // tolower
        case 0x28: for (uint32_t i = 0; i < a1; i++) MEM_WB(a0 + i, 0); v0 = a0; break;   // bzero
        case 0x2a: for (uint32_t i = 0; i < a2; i++) MEM_WB(a0 + i, MEM_RB(a1 + i)); v0 = a0; break;  // memcpy
        case 0x2b: for (uint32_t i = 0; i < a2; i++) MEM_WB(a0 + i, a1); v0 = a0; break;  // memset
        case 0x2c: {   // memmove
            if (a0 < a1) for (uint32_t i = 0; i < a2; i++) MEM_WB(a0 + i, MEM_RB(a1 + i));
            else for (uint32_t i = a2; i-- > 0;) MEM_WB(a0 + i, MEM_RB(a1 + i));
            v0 = a0;
            break;
        }
        case 0x2d: {   // memcmp
            int r = 0;
            for (uint32_t i = 0; i < a2; i++) { int x = MEM_RB(a0 + i), y = MEM_RB(a1 + i); if (x != y) { r = x - y; break; } }
            v0 = (uint32_t)r;
            break;
        }
        case 0x2e: {   // memchr
            v0 = 0;
            for (uint32_t i = 0; i < a2; i++) if (MEM_RB(a0 + i) == (a1 & 0xff)) { v0 = a0 + i; break; }
            break;
        }
        case 0x39: g_heap_base = a0; g_heap_size = a1; v0 = 0; break;   // InitHeap
        case 0x3f: {   // printf
            std::string s = guest_format(c, a0, 1);
            if (log_enabled("printf") || log_enabled("tty")) fprintf(stderr, "[tty] %s%s", s.c_str(), (s.empty() || s.back() != '\n') ? "\n" : "");
            v0 = (uint32_t)s.size();
            break;
        }
        case 0x44: v0 = 0; break;   // FlushCache
        case 0x48: gpu_write(1, a0); v0 = 0; break;   // SendGP1Command
        case 0x49: gpu_write(0, a0); v0 = 0; break;   // GPU_cw
        case 0x70: v0 = 1; break;   // _bu_init
        case 0x72: v0 = 0; break;   // _96_remove (BIOS CD driver; the game uses libcd)
        case 0xab: {   // _card_info(port): async
            int slot = card_slot(a0);
            if (card_busy()) { v0 = 0; break; }
            v0 = 1;
            card_async_done(c, memcard_present(slot) ? 0x0004 : 0x0100);
            break;
        }
        case 0xac: {   // _card_load(port): async directory read
            int slot = card_slot(a0);
            if (card_busy()) { v0 = 0; break; }
            v0 = 1;
            card_async_done(c, memcard_present(slot) ? 0x0004 : 0x0100);
            break;
        }
        default: unknown(c, 0, fn);
    }
}

static void bios_b0(Cpu* c, int fn) {
    uint32_t a0 = c->r[4], a1 = c->r[5], a2 = c->r[6], a3 = c->r[7];
    uint32_t& v0 = c->r[2];
    switch (fn) {
        case 0x00: {   // alloc_kernel_memory(size)
            uint32_t sz = (a0 + 3) & ~3u;
            if (g_kheap && g_kheap + sz <= g_kheap_end) { v0 = g_kheap; g_kheap += sz; }
            else v0 = 0;
            LOG("bios", "alloc_kernel_memory(%x) -> %08x", a0, v0);
            break;
        }
        case 0x07: bios_deliver_event(c, a0, a1); v0 = 1; break;   // DeliverEvent
        case 0x08: v0 = bios_open_event(a0, a1, a2, a3); break;    // OpenEvent
        case 0x09: sw_(evcb(ev_index(a0)) + 4, 0); v0 = 1; break;  // CloseEvent
        case 0x0a: {   // WaitEvent
            uint32_t e = evcb(ev_index(a0));
            while (gw(e + 4) == 0x2000) { c->cycles = std::max(c->cycles, c->next_event); rt_events(c); }
            if (gw(e + 4) == 0x4000) { sw_(e + 4, 0x2000); v0 = 1; } else v0 = 0;
            break;
        }
        case 0x0b: {   // TestEvent
            uint32_t e = evcb(ev_index(a0));
            if (gw(e + 4) == 0x4000) { sw_(e + 4, 0x2000); v0 = 1; } else v0 = 0;
            break;
        }
        case 0x0c: sw_(evcb(ev_index(a0)) + 4, 0x2000); v0 = 1; break;   // EnableEvent
        case 0x0d: sw_(evcb(ev_index(a0)) + 4, 0x1000); v0 = 1; break;   // DisableEvent
        case 0x0e: v0 = bios_open_thread(c, a0, a1, a2); break;          // OpenThread
        case 0x0f: {   // CloseThread
            int i = a0 & 0xffff;
            if (i < tcb_count()) sw_(tcb_addr(i), 0x1000);
            LOG("thread", "CloseThread %d", i);
            v0 = 1;
            break;
        }
        case 0x10: v0 = bios_change_thread(c, a0); break;                 // ChangeThread
        case 0x17:     // ReturnFromException
            if (g_in_exception) throw RfeUnwind();
            LOG("bios", "ReturnFromException outside an exception");
            break;
        case 0x18: g_custom_exit = 0; v0 = 0; break;                      // SetDefaultExitFromException
        case 0x19: g_custom_exit = a0; LOG("bios", "SetCustomExitFromException(%08x) ra=%08x sp=%08x",
                                           a0, gw(a0), gw(a0 + 4)); v0 = 0; break;
        case 0x32: {   // open
            std::string name(guest_str(a0));
            int slot = name.compare(0, 4, "bu10") == 0 ? 1 : 0;
            if (name.compare(0, 2, "bu") != 0) { v0 = 0xffffffffu; g_card_last_error = 2; break; }
            size_t colon = name.find(':');
            int h = memcard_open(slot, name.c_str() + (colon == std::string::npos ? 0 : colon + 1), a1);
            LOG("card", "open(%s, %x) -> %d", name.c_str(), a1, h);
            v0 = (uint32_t)h;
            g_card_last_error = h < 0 ? 2 : 0;
            if (h >= 0 && h < 16) g_fd_async[h] = (a1 & 0x8000) != 0;
            if (h >= 0 && (a1 & 0x8000)) card_done_events(c, 0x0004);
            break;
        }
        case 0x33:   // lseek
            v0 = (uint32_t)memcard_seek((int)a0, (int)a1, (int)a2);
            LOG("card", "lseek(%d, %d, %d) -> %d", a0, a1, a2, (int)v0);
            break;
        case 0x34: case 0x35: {   // read / write
            bool wr = fn == 0x35;
            if (a0 < 16 && g_fd_async[a0]) {
                // asynchronous: the transfer is started (0) or refused while another is running (-1);
                // completion is reported by an event
                if (card_busy()) { v0 = 0xffffffffu; break; }
                std::string tmp(a2, '\0');
                if (wr) for (uint32_t i = 0; i < a2; i++) tmp[i] = (char)MEM_RB(a1 + i);
                int n = memcard_rw((int)a0, (uint8_t*)tmp.data(), (int)a2, wr);
                if (!wr && n > 0) for (int i = 0; i < n; i++) MEM_WB(a1 + i, (uint8_t)tmp[i]);
                LOG("card", "async %s(%d, %08x, %d) -> %d", wr ? "write" : "read", a0, a1, a2, n);
                card_undeliver();
                card_async_done(c, n > 0 ? 0x0004 : 0x8000, (int)(a2 + 127) / 128);
                v0 = 0;
                break;
            }
            std::string tmp(a2, '\0');
            if (wr) for (uint32_t i = 0; i < a2; i++) tmp[i] = (char)MEM_RB(a1 + i);
            int n = memcard_rw((int)a0, (uint8_t*)tmp.data(), (int)a2, wr);
            if (!wr && n > 0) for (int i = 0; i < n; i++) MEM_WB(a1 + i, (uint8_t)tmp[i]);
            LOG("card", "%s(%d, %08x, %d) -> %d", wr ? "write" : "read", a0, a1, a2, n);
            v0 = (uint32_t)n;
            break;
        }
        case 0x36: memcard_close((int)a0); v0 = a0; LOG("card", "close(%d)", a0); break;   // close
        case 0x3f: {   // puts
            std::string s(guest_str(a0));
            if (log_enabled("printf") || log_enabled("tty")) fprintf(stderr, "[tty] %s\n", s.c_str());
            v0 = 0;
            break;
        }
        case 0x42: {   // firstfile(pattern, dirent): through the device's firstfile entry (which libmcrd hooks)
            std::string p(guest_str(a0));
            if (p.compare(0, 2, "bu") != 0) { v0 = 0; break; }
            size_t colon = p.find(':');
            for (uint32_t k = 0; k < 0x2c; k += 4) sw_(K_FCB + k, 0);
            sw_(K_FCB + 0x14, p.compare(0, 4, "bu10") == 0 ? 0x10 : 0x00);   // unit / port
            sw_(K_FCB + 0x24, K_DCB + DCB_SIZE);
            uint32_t pat = a0 + (uint32_t)(colon == std::string::npos ? 0 : colon + 1);
            v0 = guest_call(c, gw(K_DCB + DCB_SIZE + 0x34), K_FCB, pat, a1);
            break;
        }
        case 0xf0: case 0x43: {   // "bu" device firstfile(fcb, pattern, dirent) / nextfile(dirent)
            static std::string pattern;
            static int slot = 0, pos = 0;
            uint32_t de = fn == 0xf0 ? a2 : a0;
            if (fn == 0xf0) {
                slot = (gw(a0 + 0x14) & 0x10) ? 1 : 0;
                pattern = std::string(guest_str(a1));
                pos = 0;
                LOG("card", "firstfile(slot %d, %s)", slot, pattern.c_str());
                // the directory is read from the card: completion is signaled like any card access
                card_done_events(c, memcard_present(slot) ? 0x0004 : 0x0100);
            }
            char name[32];
            int size = 0, idx = 0;
            if (memcard_find(slot, pattern.c_str(), pos, name, &size, &idx)) {
                pos = idx + 1;
                for (int i = 0; i < 20; i++) MEM_WB(de + i, i < (int)strlen(name) + 1 ? name[i] : 0);
                sw_(de + 20, 0x50);         // attr
                sw_(de + 24, (uint32_t)size);
                sw_(de + 28, 0);
                sw_(de + 32, (uint32_t)idx);
                v0 = de;
                LOG("card", "%s -> %s (%d bytes)", fn == 0xf0 ? "firstfile" : "nextfile", name, size);
            } else {
                v0 = 0;
            }
            break;
        }
        case 0x45: {   // erase
            std::string name(guest_str(a0));
            int slot = name.compare(0, 4, "bu10") == 0 ? 1 : 0;
            size_t colon = name.find(':');
            v0 = memcard_erase(slot, name.c_str() + (colon == std::string::npos ? 0 : colon + 1)) ? 1 : 0;
            break;
        }
        case 0x4a: case 0x4b: case 0x4c: v0 = 1; break;   // InitCard / StartCard / StopCard
        case 0x4e: case 0x4f: {   // write_card_sector(port, sector, src) / read_card_sector(port, sector, dst)
            int slot = card_slot(a0);
            if (card_busy()) { v0 = 0; break; }
            uint8_t buf[128];
            bool ok;
            if (fn == 0x4e) {
                for (int i = 0; i < 128; i++) buf[i] = (uint8_t)MEM_RB(a2 + i);
                ok = memcard_write_sector(slot, (int)a1, buf);
            } else {
                ok = memcard_read_sector(slot, (int)a1, buf);
                if (ok) for (int i = 0; i < 128; i++) MEM_WB(a2 + i, buf[i]);
            }
            v0 = 1;
            LOG("card", "%s_card_sector(%x, %d) %s", fn == 0x4e ? "write" : "read", a0, a1, ok ? "ok" : "failed");
            card_async_done(c, ok ? 0x0004 : 0x0100);
            break;
        }
        case 0x50: v0 = 1; break;   // allow_new_card
        case 0x54: case 0x55: v0 = g_card_last_error; break;   // GetLastError / GetLastFileError
        case 0x56: v0 = 0x80000674u; break;   // GetC0Table
        case 0x57: v0 = 0x80000874u; break;   // GetB0Table
        case 0x5b: { bool old = g_clear_pad; g_clear_pad = a0 != 0; v0 = old; break; }   // ChangeClearPad
        default: unknown(c, 1, fn);
    }
}

static void bios_c0(Cpu* c, int fn) {
    uint32_t a0 = c->r[4], a1 = c->r[5];
    uint32_t& v0 = c->r[2];
    switch (fn) {
        case 0x02: {   // SysEnqIntRP(priority, struct)
            uint32_t head = excb_addr() + (a0 & 3) * 8;
            sw_(a1, gw(head));
            sw_(head, a1);
            LOG("bios", "SysEnqIntRP(%d, %08x: f1=%08x f2=%08x)", a0, a1, gw(a1 + 8), gw(a1 + 4));
            v0 = 0;
            break;
        }
        case 0x03: {   // SysDeqIntRP(priority, struct)
            uint32_t prev = excb_addr() + (a0 & 3) * 8;
            for (uint32_t e = gw(prev); e; prev = e, e = gw(e)) {
                if (e == a1) { sw_(prev, gw(e)); break; }
            }
            v0 = 0;
            break;
        }
        case 0x00: case 0x01: case 0x0c: v0 = 0; break;   // EnqueueTimerAndVblankIrqs / EnqueueSyscallHandler / InitDefInt (HLE: built in)
        case 0x08: g_kheap = a0; g_kheap_end = a0 + a1; v0 = 0; break;   // SysInitMemory(addr, size)
        case 0x0a: { int t = a0 & 3; bool old = g_clear_rcnt[t]; g_clear_rcnt[t] = a1 != 0; v0 = old; break; }   // ChangeClearRCnt
        default: unknown(c, 2, fn);
    }
}

bool bios_is_vector(uint32_t addr) {
    uint32_t p = addr & 0x1fffffffu;
    return p == 0xa0 || p == 0xb0 || p == 0xc0 || (p >= 0x1fc10000u && p < 0x1fc40000u);
}

bool bios_dispatch(Cpu* c, uint32_t addr) {
    uint32_t p = addr & 0x1fffffffu;
    int tab, fn;
    if (p == 0xa0 || p == 0xb0 || p == 0xc0) { tab = (p >> 4) - 0xa; fn = c->r[9] & 0xff; }
    else if (p >= 0x1fc10000u && p < 0x1fc40000u) { tab = ((p >> 16) & 0xf) - 1; fn = (p & 0xffff) >> 2; }
    else return false;
    if (log_enabled("bioscall") && !(tab == 1 && (fn == 0x0b)))
        fprintf(stderr, "[bioscall] %X0:%02X a0=%08x a1=%08x a2=%08x ra=%08x\n", 0xa + tab, fn, c->r[4], c->r[5], c->r[6], c->r[31]);
    c->cycles += 20;
    switch (tab) {
        case 0: bios_a0(c, fn); break;
        case 1: bios_b0(c, fn); break;
        default: bios_c0(c, fn); break;
    }
    return true;
}

// ---------------------------------------------------------------- syscall / exceptions
void bios_syscall(Cpu* c, uint32_t pc) {
    uint32_t& v0 = c->r[2];
    switch (c->r[4]) {
        case 0: break;
        case 1: {   // EnterCriticalSection
            bool was = (c->sr & 0x401) == 0x401;
            c->sr &= ~0x401u;
            v0 = was;
            irq_update();
            break;
        }
        case 2:     // ExitCriticalSection
            c->sr |= 0x401u;
            irq_update();
            break;
        case 3:     // ChangeThreadSubFunction(tcb)
            for (int i = 0; i < tcb_count(); i++) if (tcb_addr(i) == c->r[5]) { bios_change_thread(c, 0xff000000u | i); break; }
            v0 = 1;
            break;
        default:
            LOG("bios", "syscall a0=%x at %08x", c->r[4], pc);
            break;
    }
}

static void run_chains(Cpu* c) {
    for (int prio = 0; prio < 4; prio++) {
        int guard = 0;
        for (uint32_t e = gw(excb_addr() + prio * 8); e && guard < 64; guard++) {
            uint32_t next = gw(e), second = gw(e + 4), first = gw(e + 8);
            if (first) {
                uint32_t v = guest_call(c, first);
                if (v && second) guest_call(c, second, v);
            }
            e = next;
        }
    }
}

static void default_handlers(Cpu* c) {
    uint32_t st = irq_stat() & irq_mask();
    // root counters 0-2 and the vblank counter (RCnt 3)
    static const int bits[4] = {IRQ_TIMER0, IRQ_TIMER1, IRQ_TIMER2, IRQ_VBLANK};
    for (int t = 0; t < 4; t++) {
        if (!(st & (1u << bits[t]))) continue;
        bios_deliver_event(c, 0xf2000000u + t, 0x0002);
        if (g_clear_rcnt[t]) mem_write_slow(0x1f801070u, ~(1u << bits[t]), 4);
    }
    if ((st & 1) && g_clear_pad) mem_write_slow(0x1f801070u, ~1u, 4);
}

void bios_exception(Cpu* c) {
    int me = g_cur;
    LOG("irq", "exception: I_STAT=%03x I_MASK=%03x sr=%08x thread=%d", irq_stat(), irq_mask(), c->sr, me);
    save_ctx(c, me);
    sw_(tcb_addr(me) + 0x98, 0x400);   // cause: interrupt
    g_in_exception = true;
    c->sr = (c->sr & ~0x3fu) | ((c->sr << 2) & 0x3cu);   // interrupts off
    c->cause = 0x400;
    c->r[29] = K_EXC_STACK;
    try {
        card_deliver_pending(c);
        run_chains(c);
        default_handlers(c);
        if (g_custom_exit) {
            uint32_t b = g_custom_exit;
            c->r[31] = gw(b + 0);
            c->r[29] = gw(b + 4);
            c->r[30] = gw(b + 8);
            for (int i = 0; i < 8; i++) c->r[16 + i] = gw(b + 12 + i * 4);
            c->r[28] = gw(b + 44);
            c->r[2] = 1;
            uint32_t pc = c->r[31];
            c->r[31] = 0;
            rt_call(c, pc);
        }
    } catch (RfeUnwind&) {
    }
    g_in_exception = false;
    // The return goes to whichever TCB the PCB points at now: handlers may switch threads
    // with ChangeThread, or (as this game does) by writing the PCB pointer directly.
    {
        uint32_t cur = gw(pcb_addr());
        for (int i = 0; i < tcb_count(); i++)
            if (tcb_addr(i) == cur) { g_cur = i; break; }
    }
    if (g_cur != me) {
        LOG("thread", "exception return switches thread %d -> %d", me, g_cur);
        host_switch(g_cur);
    }
    load_ctx(c, me);
    c->cause = 0;
}

// ---------------------------------------------------------------- boot
void bios_init(const char* save_dir) {
    memcard_init(save_dir);
    // table of tables
    sw_(K_TOT + 0x00, K_EXCB); sw_(K_TOT + 0x04, 4 * 8);
    sw_(K_TOT + 0x08, K_PCB);  sw_(K_TOT + 0x0c, 4);
    sw_(K_TOT + 0x10, K_TCB);  sw_(K_TOT + 0x14, SYS_TCBS * TCB_SIZE);
    sw_(K_TOT + 0x20, K_EVCB); sw_(K_TOT + 0x24, SYS_EVENTS * EVCB_SIZE);
    // A0/B0/C0 function tables point at recognizable "ROM" addresses
    for (int i = 0; i < 0xc0; i++) sw_(0x80000200u + i * 4, 0xbfc10000u + i * 4);
    for (int i = 0; i < 0x60; i++) sw_(0x80000874u + i * 4, 0xbfc20000u + i * 4);
    for (int i = 0; i < 0x20; i++) sw_(0x80000674u + i * 4, 0xbfc30000u + i * 4);
    // device control blocks: libcard/libmcrd find the "bu" device here and hook its firstfile entry
    sw_(K_TOT + 0x50, K_DCB); sw_(K_TOT + 0x54, 2 * DCB_SIZE);
    for (uint32_t k = 0; k < 2 * DCB_SIZE; k += 4) sw_(K_DCB + k, 0);
    guest_strcpy_out(K_DCB_NAMES, "cdrom");
    guest_strcpy_out(K_DCB_NAMES + 8, "bu");
    sw_(K_DCB + 0x00, K_DCB_NAMES);
    sw_(K_DCB + DCB_SIZE + 0x00, K_DCB_NAMES + 8);
    sw_(K_DCB + DCB_SIZE + 0x04, 0x14);          // flags: block device
    sw_(K_DCB + DCB_SIZE + 0x08, 0x80);          // sector size
    sw_(K_DCB + DCB_SIZE + 0x34, DEV_BU_FIRSTFILE);
    for (int i = 0; i < SYS_TCBS; i++) sw_(tcb_addr(i), 0x1000);
    sw_(tcb_addr(0), 0x4000);
    set_current(0);
    // SetConf() finds the BIOS's config variables by decoding a lui/addiu pair at [0x474];
    // point it at a harmless scratch word.
    sw_(0x80000474u, 0x8000b000u);
    sw_(0x8000b000u, 0x3c028000u);   // lui v0, 0x8000
    sw_(0x8000b004u, 0x24427100u);   // addiu v0, v0, 0x7100
}

void bios_run_exe(const uint8_t* exe, size_t size) {
    Cpu* c = &g_cpu;
    uint32_t pc, gp, taddr, tsize, baddr, bsize;
    memcpy(&pc, exe + 0x10, 4); memcpy(&gp, exe + 0x14, 4);
    memcpy(&taddr, exe + 0x18, 4); memcpy(&tsize, exe + 0x1c, 4);
    memcpy(&baddr, exe + 0x28, 4); memcpy(&bsize, exe + 0x2c, 4);
    memcpy(g_ram + (taddr & 0x1fffff), exe + 0x800, std::min<size_t>(tsize, size - 0x800));
    if (bsize) memset(g_ram + (baddr & 0x1fffff), 0, bsize);
    memset(c->r, 0, sizeof c->r);
    c->r[28] = gp;
    c->r[29] = c->r[30] = SYS_STACK;
    c->sr = 0x40000401u;   // COP2 enabled, interrupts enabled
    irq_update();
    t_me = 0;
    g_running = 0;
    g_ht[0].started = true;   // TCB 0 runs on this (the main) host thread
    LOG("bios", "starting executable at %08x (sp=%08x)", pc, SYS_STACK);
    rt_call(c, pc);
    fprintf(stderr, "[bios] main() returned\n");
    exit(0);
}
