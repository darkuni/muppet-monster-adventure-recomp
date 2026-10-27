// Interface between recompiled code (gen/*.cpp) and the runtime.
#pragma once
#include <cstdint>
#include <cstring>
#include <cstddef>

struct Cpu {
    uint32_t r[32];
    uint32_t hi, lo;
    int64_t cycles;       // emulated CPU cycles since boot
    int64_t next_event;   // cycles value at which the scheduler must run
    uint32_t sr, cause, epc, badvaddr;   // COP0
};
extern Cpu g_cpu;

// ---------------------------------------------------------------- memory
extern uint8_t* g_ram;        // 2 MB main RAM
extern bool g_cache_isolated; // COP0 SR.IsC: stores go to the (unemulated) cache, not RAM

uint32_t mem_read_slow(uint32_t a, int size);
void mem_write_slow(uint32_t a, uint32_t v, int size);

static inline uint32_t MEM_RW(uint32_t a) {
    uint32_t p = a & 0x1fffffffu;
    if (p < 0x800000u) { uint32_t v; memcpy(&v, g_ram + (p & 0x1ffffcu), 4); return v; }
    return mem_read_slow(a, 4);
}
static inline uint32_t MEM_RH(uint32_t a) {
    uint32_t p = a & 0x1fffffffu;
    if (p < 0x800000u) { uint16_t v; memcpy(&v, g_ram + (p & 0x1ffffeu), 2); return v; }
    return mem_read_slow(a, 2) & 0xffff;
}
static inline uint32_t MEM_RB(uint32_t a) {
    uint32_t p = a & 0x1fffffffu;
    if (p < 0x800000u) return g_ram[p & 0x1fffffu];
    return mem_read_slow(a, 1) & 0xff;
}
static inline void MEM_WW(uint32_t a, uint32_t v) {
    uint32_t p = a & 0x1fffffffu;
    if (p < 0x800000u) { if (!g_cache_isolated) memcpy(g_ram + (p & 0x1ffffcu), &v, 4); return; }
    mem_write_slow(a, v, 4);
}
static inline void MEM_WH(uint32_t a, uint32_t v) {
    uint32_t p = a & 0x1fffffffu;
    if (p < 0x800000u) { if (!g_cache_isolated) { uint16_t h = (uint16_t)v; memcpy(g_ram + (p & 0x1ffffeu), &h, 2); } return; }
    mem_write_slow(a, v & 0xffff, 2);
}
static inline void MEM_WB(uint32_t a, uint32_t v) {
    uint32_t p = a & 0x1fffffffu;
    if (p < 0x800000u) { if (!g_cache_isolated) g_ram[p & 0x1fffffu] = (uint8_t)v; return; }
    mem_write_slow(a, v & 0xff, 1);
}
// Unaligned loads/stores (little-endian R3000 semantics)
static inline uint32_t MEM_LWL(uint32_t a, uint32_t rt) {
    uint32_t w = MEM_RW(a & ~3u);
    switch (a & 3) {
        case 0: return (rt & 0x00ffffffu) | (w << 24);
        case 1: return (rt & 0x0000ffffu) | (w << 16);
        case 2: return (rt & 0x000000ffu) | (w << 8);
        default: return w;
    }
}
static inline uint32_t MEM_LWR(uint32_t a, uint32_t rt) {
    uint32_t w = MEM_RW(a & ~3u);
    switch (a & 3) {
        case 0: return w;
        case 1: return (rt & 0xff000000u) | (w >> 8);
        case 2: return (rt & 0xffff0000u) | (w >> 16);
        default: return (rt & 0xffffff00u) | (w >> 24);
    }
}
static inline void MEM_SWL(uint32_t a, uint32_t rt) {
    uint32_t al = a & ~3u, m = MEM_RW(al);
    switch (a & 3) {
        case 0: m = (m & 0xffffff00u) | (rt >> 24); break;
        case 1: m = (m & 0xffff0000u) | (rt >> 16); break;
        case 2: m = (m & 0xff000000u) | (rt >> 8); break;
        default: m = rt; break;
    }
    MEM_WW(al, m);
}
static inline void MEM_SWR(uint32_t a, uint32_t rt) {
    uint32_t al = a & ~3u, m = MEM_RW(al);
    switch (a & 3) {
        case 0: m = rt; break;
        case 1: m = (m & 0x000000ffu) | (rt << 8); break;
        case 2: m = (m & 0x0000ffffu) | (rt << 16); break;
        default: m = (m & 0x00ffffffu) | (rt << 24); break;
    }
    MEM_WW(al, m);
}

// ---------------------------------------------------------------- CPU helpers
static inline void cpu_div(Cpu* c, uint32_t n, uint32_t d) {
    int32_t a = (int32_t)n, b = (int32_t)d;
    if (b == 0) { c->hi = n; c->lo = a >= 0 ? 0xffffffffu : 1u; }
    else if (n == 0x80000000u && b == -1) { c->hi = 0; c->lo = 0x80000000u; }
    else { c->lo = (uint32_t)(a / b); c->hi = (uint32_t)(a % b); }
}
static inline void cpu_divu(Cpu* c, uint32_t n, uint32_t d) {
    if (d == 0) { c->hi = n; c->lo = 0xffffffffu; }
    else { c->lo = n / d; c->hi = n % d; }
}
uint32_t cop0_read(Cpu* c, int reg);
void cop0_write(Cpu* c, int reg, uint32_t v);
void cop0_rfe(Cpu* c);

// GTE (COP2)
uint32_t gte_read_data(int reg);
void gte_store_sxy(uint32_t addr, int reg);   // swc2 of SXY0-2/SXYP (keeps precise coordinates)
void gte_write_data(int reg, uint32_t v);
uint32_t gte_read_ctrl(int reg);
void gte_write_ctrl(int reg, uint32_t v);
void gte_command(uint32_t op);

// ---------------------------------------------------------------- control transfer
void rt_call(Cpu* c, uint32_t addr);            // call guest code at addr (recompiled, HLE or interpreted)
void rt_interp(Cpu* c, uint32_t addr);          // run guest code at addr in the interpreter until it returns
void rt_syscall(Cpu* c, uint32_t pc);
void rt_break(Cpu* c, uint32_t pc);
void rt_bad_insn(Cpu* c, uint32_t pc, uint32_t word);
void rt_events(Cpu* c);                         // scheduler: hardware events and interrupts
void hle_nop(Cpu* c);

// Bisect builds (-DRECOMP_BISECT) can run chosen guest functions in the interpreter instead
// (BISECT=lo-hi[,lo-hi...], BISECT_FROM=frame) to find code generation bugs.
#ifdef RECOMP_BISECT
bool bisect_interp(uint32_t addr);
#define FUNC_ENTRY(c, a) do { if (bisect_interp(a)) { rt_interp(c, a); return; } } while (0)
#else
#define FUNC_ENTRY(c, a) do {} while (0)
#endif
#define CHECK_EVENTS(c) do { if ((c)->cycles >= (c)->next_event) rt_events(c); } while (0)
