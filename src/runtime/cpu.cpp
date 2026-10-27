// Guest control transfers: indirect calls, COP0, syscall/break, and the interpreter fallback
// for code the recompiler did not see (BIOS-area code, missed function pointers).
#include "recomp.h"
#include "table.h"
#include "game.h"
#include "hw.h"
#include "bios.h"
#include "log.h"
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <unordered_set>

Cpu g_cpu;

static GuestFn find_in(const FuncEntry* t, size_t n, uint32_t a) {
    size_t lo = 0, hi = n;
    while (lo < hi) {
        size_t m = (lo + hi) / 2;
        if (t[m].addr < a) lo = m + 1; else hi = m;
    }
    return (lo < n && t[lo].addr == a) ? t[lo].fn : nullptr;
}

static const OverlayEntry* g_cur_ovl = nullptr;
static uint32_t g_cur_ovl_id = 0xffffffffu;

const OverlayEntry* current_overlay() {
    uint32_t id;
    memcpy(&id, g_ram + (OVL_BASE & 0x1fffff), 4);
    if (id != g_cur_ovl_id) {
        g_cur_ovl_id = id;
        g_cur_ovl = nullptr;
        for (size_t i = 0; i < g_overlays_count; i++)
            if (g_overlays[i].id == id) { g_cur_ovl = &g_overlays[i]; break; }
        LOG("ovl", "overlay %02x %s loaded", id, g_cur_ovl ? g_cur_ovl->name : "(unknown)");
    }
    return g_cur_ovl;
}

GuestFn lookup_func(uint32_t addr) {
    uint32_t a = (addr & 0x1fffffffu) | 0x80000000u;
    if (a >= EXE_TEXT && a < OVL_BASE) return find_in(g_exe_funcs, g_exe_funcs_count, a);
    if (a >= OVL_BASE && a < OVL_END) {
        const OverlayEntry* o = current_overlay();
        if (o && a < OVL_BASE + o->size) return find_in(o->funcs, o->count, a);
    }
    return nullptr;
}

void rt_call(Cpu* c, uint32_t addr) {
    if (bios_dispatch(c, addr)) return;
    if (GuestFn f = lookup_func(addr)) { f(c); return; }
    rt_interp(c, addr);
}

void hle_nop(Cpu* c) { c->r[2] = 0; }

// ---------------------------------------------------------------- COP0
uint32_t cop0_read(Cpu* c, int reg) {
    switch (reg) {
        case 8: return c->badvaddr;
        case 12: return c->sr;
        case 13: return c->cause | (irq_pending() ? 0x400u : 0u);
        case 14: return c->epc;
        case 15: return 0x00000002u;   // PRId: R3000A
        default: return 0;
    }
}
void cop0_write(Cpu* c, int reg, uint32_t v) {
    if (reg == 12) {
        c->sr = v;
        g_cache_isolated = (v & 0x10000u) != 0;
        irq_update();
    } else if (reg == 13) {
        c->cause = (c->cause & ~0x300u) | (v & 0x300u);
    }
}
void cop0_rfe(Cpu* c) {
    c->sr = (c->sr & ~0xfu) | ((c->sr >> 2) & 0xfu);
    irq_update();
}

void rt_syscall(Cpu* c, uint32_t pc) {
    bios_syscall(c, pc);
}
void rt_break(Cpu* c, uint32_t pc) {
    LOG("cpu", "break at %08x (ignored)", pc);
}
void rt_bad_insn(Cpu* c, uint32_t pc, uint32_t word) {
    fprintf(stderr, "[cpu] invalid instruction %08x at %08x (ra=%08x)\n", word, pc, c->r[31]);
    fflush(stderr);
    abort();
}

// ---------------------------------------------------------------- interpreter
// Runs one guest "call": from `addr` until control returns to the caller (a jump to the
// return address that was in ra at entry), calling into recompiled/HLE code for calls.
static inline uint32_t sx16(uint32_t v) { return (uint32_t)(int32_t)(int16_t)v; }

void rt_interp(Cpu* c, uint32_t addr) {
    static std::unordered_set<uint32_t> reported;
    if (reported.insert(addr).second)
        LOG("interp", "interpreting %08x (ra=%08x)", addr, c->r[31]);
    const uint32_t ret = c->r[31];
    uint32_t pc = addr;
    int depth_guard = 0;
    (void)depth_guard;
    CHECK_EVENTS(c);
    for (;;) {
        uint32_t w = MEM_RW(pc);
        uint32_t op = w >> 26, rs = (w >> 21) & 31, rt = (w >> 16) & 31, rd = (w >> 11) & 31, sa = (w >> 6) & 31;
        uint32_t imm = w & 0xffff, simm = sx16(imm);
        uint32_t* r = c->r;
        uint32_t next = pc + 4;
        // control transfer bookkeeping
        int kind = 0;               // 0 none, 1 branch/jump to target, 2 call to target
        bool taken = false;
        uint32_t target = 0;
        uint32_t link_reg = 0, link_val = 0;
        c->cycles += 1;
        switch (op) {
            case 0x00: {
                uint32_t f = w & 63;
                switch (f) {
                    case 0x00: if (rd) r[rd] = r[rt] << sa; break;
                    case 0x02: if (rd) r[rd] = r[rt] >> sa; break;
                    case 0x03: if (rd) r[rd] = (uint32_t)((int32_t)r[rt] >> sa); break;
                    case 0x04: if (rd) r[rd] = r[rt] << (r[rs] & 31); break;
                    case 0x06: if (rd) r[rd] = r[rt] >> (r[rs] & 31); break;
                    case 0x07: if (rd) r[rd] = (uint32_t)((int32_t)r[rt] >> (r[rs] & 31)); break;
                    case 0x08: kind = 1; taken = true; target = r[rs]; break;
                    case 0x09: kind = 2; taken = true; target = r[rs]; link_reg = rd; link_val = pc + 8; break;
                    case 0x0c: c->epc = pc; rt_syscall(c, pc); break;
                    case 0x0d: rt_break(c, pc); break;
                    case 0x10: if (rd) r[rd] = c->hi; break;
                    case 0x11: c->hi = r[rs]; break;
                    case 0x12: if (rd) r[rd] = c->lo; break;
                    case 0x13: c->lo = r[rs]; break;
                    case 0x18: { int64_t m = (int64_t)(int32_t)r[rs] * (int32_t)r[rt]; c->lo = (uint32_t)m; c->hi = (uint32_t)((uint64_t)m >> 32); } break;
                    case 0x19: { uint64_t m = (uint64_t)r[rs] * r[rt]; c->lo = (uint32_t)m; c->hi = (uint32_t)(m >> 32); } break;
                    case 0x1a: cpu_div(c, r[rs], r[rt]); break;
                    case 0x1b: cpu_divu(c, r[rs], r[rt]); break;
                    case 0x20: case 0x21: if (rd) r[rd] = r[rs] + r[rt]; break;
                    case 0x22: case 0x23: if (rd) r[rd] = r[rs] - r[rt]; break;
                    case 0x24: if (rd) r[rd] = r[rs] & r[rt]; break;
                    case 0x25: if (rd) r[rd] = r[rs] | r[rt]; break;
                    case 0x26: if (rd) r[rd] = r[rs] ^ r[rt]; break;
                    case 0x27: if (rd) r[rd] = ~(r[rs] | r[rt]); break;
                    case 0x2a: if (rd) r[rd] = (int32_t)r[rs] < (int32_t)r[rt]; break;
                    case 0x2b: if (rd) r[rd] = r[rs] < r[rt]; break;
                    default: rt_bad_insn(c, pc, w);
                }
                break;
            }
            case 0x01: {
                bool ge = (rt & 1) != 0;
                taken = ge ? (int32_t)r[rs] >= 0 : (int32_t)r[rs] < 0;
                target = pc + 4 + (simm << 2);
                if ((rt & 0x1e) == 0x10) { kind = 2; link_reg = 31; link_val = pc + 8; r[31] = pc + 8; }
                else kind = 1;
                break;
            }
            case 0x02: kind = 1; taken = true; target = (next & 0xf0000000u) | ((w & 0x3ffffff) << 2); break;
            case 0x03: kind = 2; taken = true; target = (next & 0xf0000000u) | ((w & 0x3ffffff) << 2); link_reg = 31; link_val = pc + 8; break;
            case 0x04: kind = 1; taken = r[rs] == r[rt]; target = pc + 4 + (simm << 2); break;
            case 0x05: kind = 1; taken = r[rs] != r[rt]; target = pc + 4 + (simm << 2); break;
            case 0x06: kind = 1; taken = (int32_t)r[rs] <= 0; target = pc + 4 + (simm << 2); break;
            case 0x07: kind = 1; taken = (int32_t)r[rs] > 0; target = pc + 4 + (simm << 2); break;
            case 0x08: case 0x09: if (rt) r[rt] = r[rs] + simm; break;
            case 0x0a: if (rt) r[rt] = (int32_t)r[rs] < (int32_t)simm; break;
            case 0x0b: if (rt) r[rt] = r[rs] < simm; break;
            case 0x0c: if (rt) r[rt] = r[rs] & imm; break;
            case 0x0d: if (rt) r[rt] = r[rs] | imm; break;
            case 0x0e: if (rt) r[rt] = r[rs] ^ imm; break;
            case 0x0f: if (rt) r[rt] = imm << 16; break;
            case 0x10: {
                if (w & (1u << 25)) { if ((w & 63) == 0x10) cop0_rfe(c); break; }
                switch (rs) {
                    case 0: if (rt) r[rt] = cop0_read(c, rd); break;
                    case 4: cop0_write(c, rd, r[rt]); break;
                    default: break;
                }
                break;
            }
            case 0x12: {
                if (w & (1u << 25)) { gte_command(w); break; }
                switch (rs) {
                    case 0: if (rt) r[rt] = gte_read_data(rd); break;
                    case 2: if (rt) r[rt] = gte_read_ctrl(rd); break;
                    case 4: gte_write_data(rd, r[rt]); break;
                    case 6: gte_write_ctrl(rd, r[rt]); break;
                }
                break;
            }
            case 0x20: { uint32_t v = (uint32_t)(int32_t)(int8_t)MEM_RB(r[rs] + simm); if (rt) r[rt] = v; break; }
            case 0x21: { uint32_t v = (uint32_t)(int32_t)(int16_t)MEM_RH(r[rs] + simm); if (rt) r[rt] = v; break; }
            case 0x22: { uint32_t v = MEM_LWL(r[rs] + simm, r[rt]); if (rt) r[rt] = v; break; }
            case 0x23: { uint32_t v = MEM_RW(r[rs] + simm); if (rt) r[rt] = v; break; }
            case 0x24: { uint32_t v = MEM_RB(r[rs] + simm); if (rt) r[rt] = v; break; }
            case 0x25: { uint32_t v = MEM_RH(r[rs] + simm); if (rt) r[rt] = v; break; }
            case 0x26: { uint32_t v = MEM_LWR(r[rs] + simm, r[rt]); if (rt) r[rt] = v; break; }
            case 0x28: MEM_WB(r[rs] + simm, r[rt]); break;
            case 0x29: MEM_WH(r[rs] + simm, r[rt]); break;
            case 0x2a: MEM_SWL(r[rs] + simm, r[rt]); break;
            case 0x2b: MEM_WW(r[rs] + simm, r[rt]); break;
            case 0x2e: MEM_SWR(r[rs] + simm, r[rt]); break;
            case 0x32: gte_write_data(rt, MEM_RW(r[rs] + simm)); break;
            case 0x3a: if (rt >= 12 && rt <= 15) gte_store_sxy(r[rs] + simm, rt); else MEM_WW(r[rs] + simm, gte_read_data(rt)); break;
            default: rt_bad_insn(c, pc, w);
        }
        if (!kind) { pc = next; continue; }
        // delay slot: execute the next instruction in the interpreter (it is never a branch)
        {
            uint32_t dpc = pc + 4;
            // Run exactly one instruction by recursion-free re-dispatch: decode inline via a tiny loop.
            // (Simplest: temporarily treat it as a straight-line instruction.)
            uint32_t save_ret = c->r[31];
            (void)save_ret;
            if (link_reg && kind == 2) { if (link_reg) r[link_reg] = link_val; }
            // execute delay slot
            uint32_t dw = MEM_RW(dpc);
            if (dw != 0) {
                // reuse the main switch by a nested call on a one-instruction "program"
                extern void interp_one(Cpu* c, uint32_t pc, uint32_t w);
                interp_one(c, dpc, dw);
            }
            c->cycles += 1;
        }
        if (kind == 2) {
            if (taken) {
                if (op == 1) { /* bltzal/bgezal: link already written */ }
                rt_call(c, target);
            }
            pc += 8;
            continue;
        }
        if (!taken) { pc += 8; continue; }
        if (target == ret && op == 0 && (w & 63) == 8) return;   // jr back to our caller
        if (target <= pc) CHECK_EVENTS(c);
        if (target == ret) return;
        // j / jr into a known function is a tail call; conditional branches always stay here
        bool is_jump = op == 2 || (op == 0 && (w & 63) == 8);
        if (is_jump && target != addr && (lookup_func(target) || bios_is_vector(target))) { rt_call(c, target); return; }
        pc = target;
    }
}

// One non-branch instruction (used for delay slots in the interpreter).
void interp_one(Cpu* c, uint32_t pc, uint32_t w) {
    uint32_t op = w >> 26, rs = (w >> 21) & 31, rt = (w >> 16) & 31, rd = (w >> 11) & 31, sa = (w >> 6) & 31;
    uint32_t imm = w & 0xffff, simm = sx16(imm);
    uint32_t* r = c->r;
    switch (op) {
        case 0x00:
            switch (w & 63) {
                case 0x00: if (rd) r[rd] = r[rt] << sa; break;
                case 0x02: if (rd) r[rd] = r[rt] >> sa; break;
                case 0x03: if (rd) r[rd] = (uint32_t)((int32_t)r[rt] >> sa); break;
                case 0x04: if (rd) r[rd] = r[rt] << (r[rs] & 31); break;
                case 0x06: if (rd) r[rd] = r[rt] >> (r[rs] & 31); break;
                case 0x07: if (rd) r[rd] = (uint32_t)((int32_t)r[rt] >> (r[rs] & 31)); break;
                case 0x10: if (rd) r[rd] = c->hi; break;
                case 0x11: c->hi = r[rs]; break;
                case 0x12: if (rd) r[rd] = c->lo; break;
                case 0x13: c->lo = r[rs]; break;
                case 0x18: { int64_t m = (int64_t)(int32_t)r[rs] * (int32_t)r[rt]; c->lo = (uint32_t)m; c->hi = (uint32_t)((uint64_t)m >> 32); } break;
                case 0x19: { uint64_t m = (uint64_t)r[rs] * r[rt]; c->lo = (uint32_t)m; c->hi = (uint32_t)(m >> 32); } break;
                case 0x1a: cpu_div(c, r[rs], r[rt]); break;
                case 0x1b: cpu_divu(c, r[rs], r[rt]); break;
                case 0x20: case 0x21: if (rd) r[rd] = r[rs] + r[rt]; break;
                case 0x22: case 0x23: if (rd) r[rd] = r[rs] - r[rt]; break;
                case 0x24: if (rd) r[rd] = r[rs] & r[rt]; break;
                case 0x25: if (rd) r[rd] = r[rs] | r[rt]; break;
                case 0x26: if (rd) r[rd] = r[rs] ^ r[rt]; break;
                case 0x27: if (rd) r[rd] = ~(r[rs] | r[rt]); break;
                case 0x2a: if (rd) r[rd] = (int32_t)r[rs] < (int32_t)r[rt]; break;
                case 0x2b: if (rd) r[rd] = r[rs] < r[rt]; break;
                default: rt_bad_insn(c, pc, w);
            }
            break;
        case 0x08: case 0x09: if (rt) r[rt] = r[rs] + simm; break;
        case 0x0a: if (rt) r[rt] = (int32_t)r[rs] < (int32_t)simm; break;
        case 0x0b: if (rt) r[rt] = r[rs] < simm; break;
        case 0x0c: if (rt) r[rt] = r[rs] & imm; break;
        case 0x0d: if (rt) r[rt] = r[rs] | imm; break;
        case 0x0e: if (rt) r[rt] = r[rs] ^ imm; break;
        case 0x0f: if (rt) r[rt] = imm << 16; break;
        case 0x10:
            if (w & (1u << 25)) { if ((w & 63) == 0x10) cop0_rfe(c); break; }
            if (rs == 0) { if (rt) r[rt] = cop0_read(c, rd); } else if (rs == 4) cop0_write(c, rd, r[rt]);
            break;
        case 0x12:
            if (w & (1u << 25)) { gte_command(w); break; }
            if (rs == 0) { if (rt) r[rt] = gte_read_data(rd); }
            else if (rs == 2) { if (rt) r[rt] = gte_read_ctrl(rd); }
            else if (rs == 4) gte_write_data(rd, r[rt]);
            else if (rs == 6) gte_write_ctrl(rd, r[rt]);
            break;
        case 0x20: { uint32_t v = (uint32_t)(int32_t)(int8_t)MEM_RB(r[rs] + simm); if (rt) r[rt] = v; break; }
        case 0x21: { uint32_t v = (uint32_t)(int32_t)(int16_t)MEM_RH(r[rs] + simm); if (rt) r[rt] = v; break; }
        case 0x22: { uint32_t v = MEM_LWL(r[rs] + simm, r[rt]); if (rt) r[rt] = v; break; }
        case 0x23: { uint32_t v = MEM_RW(r[rs] + simm); if (rt) r[rt] = v; break; }
        case 0x24: { uint32_t v = MEM_RB(r[rs] + simm); if (rt) r[rt] = v; break; }
        case 0x25: { uint32_t v = MEM_RH(r[rs] + simm); if (rt) r[rt] = v; break; }
        case 0x26: { uint32_t v = MEM_LWR(r[rs] + simm, r[rt]); if (rt) r[rt] = v; break; }
        case 0x28: MEM_WB(r[rs] + simm, r[rt]); break;
        case 0x29: MEM_WH(r[rs] + simm, r[rt]); break;
        case 0x2a: MEM_SWL(r[rs] + simm, r[rt]); break;
        case 0x2b: MEM_WW(r[rs] + simm, r[rt]); break;
        case 0x2e: MEM_SWR(r[rs] + simm, r[rt]); break;
        case 0x32: gte_write_data(rt, MEM_RW(r[rs] + simm)); break;
        case 0x3a: if (rt >= 12 && rt <= 15) gte_store_sxy(r[rs] + simm, rt); else MEM_WW(r[rs] + simm, gte_read_data(rt)); break;
        default: rt_bad_insn(c, pc, w);
    }
}

#ifdef RECOMP_BISECT
#include <string>
#include <vector>
bool bisect_interp(uint32_t addr) {
    static std::vector<std::pair<uint32_t, uint32_t>> ranges;
    static int64_t from = -1;
    if (from < 0) {
        const char* e = getenv("BISECT");
        std::string s = e ? e : "";
        size_t p = 0;
        while (p < s.size()) {
            size_t c = s.find(',', p);
            std::string it = s.substr(p, c == std::string::npos ? std::string::npos : c - p);
            p = c == std::string::npos ? s.size() : c + 1;
            uint32_t lo = 0, hi = 0;
            if (sscanf(it.c_str(), "%x-%x", &lo, &hi) == 2) ranges.push_back({lo, hi});
        }
        const char* f = getenv("BISECT_FROM_SEC");
        from = f ? (int64_t)(atof(f) * CPU_HZ) : 0;
        fprintf(stderr, "[bisect] %zu ranges from cycle %lld\n", ranges.size(), (long long)from);
    }
    if (g_cpu.cycles < from) return false;
    for (auto& r : ranges) if (addr >= r.first && addr < r.second) return true;
    return false;
}
#endif
