// GTE (COP2) - the PlayStation geometry transformation engine, implemented to the documented
// hardware behavior (psx-spx): UNR division, 44-bit MAC overflow flags, saturation flags,
// the IR3 flag quirk of RTPS/RTPT and the MVMVA far-color bug.
#include "recomp.h"
#include "gte.h"
#include "precise.h"
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <algorithm>
#include <cstdint>

GteState g_gte;
float g_gte_widescreen = 1.0f;   // horizontal projection scale (0.75 = 16:9 widescreen hack)

static int32_t d[32];     // data registers (stored as written; reads apply sign/zero extension)
static int32_t cr[32];    // control registers
static uint32_t& FLAG = (uint32_t&)cr[31];

// register views
#define VX(n) ((int16_t)d[(n) * 2])
#define VY(n) ((int16_t)(d[(n) * 2] >> 16))
#define VZ(n) ((int16_t)d[(n) * 2 + 1])
#define IR0 d[8]
#define IR1 d[9]
#define IR2 d[10]
#define IR3 d[11]
#define MAC0 d[24]
#define MAC1 d[25]
#define MAC2 d[26]
#define MAC3 d[27]

static int16_t M(int m, int i, int j) {   // matrix m (0 RT, 1 LLM, 2 LCM), element (i, j)
    int base = m * 8;
    int idx = i * 3 + j;
    int32_t w = cr[base + idx / 2];
    return (idx & 1) ? (int16_t)(w >> 16) : (int16_t)w;
}
static int32_t TR(int i) { return cr[5 + i]; }
static int32_t BK(int i) { return cr[13 + i]; }
static int32_t FC(int i) { return cr[21 + i]; }
#define OFX cr[24]
#define OFY cr[25]
#define H ((uint16_t)cr[26])
#define DQA ((int16_t)cr[27])
#define DQB cr[28]
#define ZSF3 ((int16_t)cr[29])
#define ZSF4 ((int16_t)cr[30])

static uint8_t unr_table[257];
static bool g_init = false;
static void gte_init_tables() {
    for (int i = 0; i < 257; i++) unr_table[i] = (uint8_t)std::max(0, (0x40000 / (i + 0x100) + 1) / 2 - 0x101);
    g_init = true;
}

static inline int64_t mac_check(int i, int64_t v) {
    if (v > 0x7ffffffffffLL) FLAG |= 1u << (31 - i);          // MAC1..3 positive overflow: bits 30,29,28
    else if (v < -0x80000000000LL) FLAG |= 1u << (28 - i);    // negative overflow: bits 27,26,25
    // sign-extend to 44 bits (hardware keeps a 44-bit accumulator)
    return (v << 20) >> 20;
}
static inline void set_mac(int i, int64_t v, int shift) {
    v = mac_check(i, v);
    d[24 + i] = (int32_t)(v >> shift);
}
static inline void set_ir(int i, int32_t v, bool lm) {
    int32_t lo = lm ? 0 : -0x8000;
    if (v > 0x7fff) { v = 0x7fff; FLAG |= 1u << (25 - i); }
    else if (v < lo) { v = lo; FLAG |= 1u << (25 - i); }
    d[8 + i] = v;
}
static inline void set_mac_ir(int i, int64_t v, int shift, bool lm) {
    set_mac(i, v, shift);
    set_ir(i, d[24 + i], lm);
}
static inline void mac0_check(int64_t v) {
    if (v > 0x7fffffffLL) FLAG |= 1u << 16;
    else if (v < -0x80000000LL) FLAG |= 1u << 15;
}
static inline void set_mac0(int64_t v) { mac0_check(v); MAC0 = (int32_t)v; }
static inline void set_ir0(int32_t v) {
    if (v > 0x1000) { v = 0x1000; FLAG |= 1u << 12; }
    else if (v < 0) { v = 0; FLAG |= 1u << 12; }
    IR0 = v;
}
static inline void push_sz(int32_t v) {
    if (v > 0xffff) { v = 0xffff; FLAG |= 1u << 18; }
    else if (v < 0) { v = 0; FLAG |= 1u << 18; }
    d[16] = d[17]; d[17] = d[18]; d[18] = d[19]; d[19] = v;
}
// precise copies of the SXY FIFO (see precise.h)
static PreciseVtx g_pfifo[3];
static bool g_pvalid[3];
static inline void push_precise(bool ok, const PreciseVtx& p) {
    g_pfifo[0] = g_pfifo[1]; g_pvalid[0] = g_pvalid[1];
    g_pfifo[1] = g_pfifo[2]; g_pvalid[1] = g_pvalid[2];
    g_pfifo[2] = p; g_pvalid[2] = ok;
}
static inline void push_sxy(int32_t x, int32_t y) {
    if (x > 0x3ff) { x = 0x3ff; FLAG |= 1u << 14; } else if (x < -0x400) { x = -0x400; FLAG |= 1u << 14; }
    if (y > 0x3ff) { y = 0x3ff; FLAG |= 1u << 13; } else if (y < -0x400) { y = -0x400; FLAG |= 1u << 13; }
    d[12] = d[13]; d[13] = d[14];
    d[14] = (int32_t)((uint32_t)(uint16_t)x | ((uint32_t)(uint16_t)y << 16));
}
static inline uint8_t sat_color(int i, int32_t v) {
    if (v < 0) { FLAG |= 1u << (21 - i); return 0; }
    if (v > 0xff) { FLAG |= 1u << (21 - i); return 0xff; }
    return (uint8_t)v;
}
static inline void push_color() {
    uint8_t r = sat_color(0, MAC1 >> 4), g = sat_color(1, MAC2 >> 4), b = sat_color(2, MAC3 >> 4);
    uint8_t code = (uint8_t)(d[6] >> 24);
    d[20] = d[21]; d[21] = d[22];
    d[22] = (int32_t)(r | (g << 8) | (b << 16) | ((uint32_t)code << 24));
}

static uint32_t gte_divide(uint32_t h, uint32_t sz3) {
    if (h < sz3 * 2) {
        int z = 0;
        while (z < 16 && !(sz3 & (0x8000u >> z))) z++;
        uint64_t n = (uint64_t)h << z;
        uint32_t dd = sz3 << z;
        uint32_t u = unr_table[(dd - 0x7fc0) >> 7] + 0x101;
        dd = (0x2000080 - (dd * u)) >> 8;
        dd = (0x0000080 + (dd * u)) >> 8;
        return (uint32_t)std::min<uint64_t>(0x1ffff, ((n * dd) + 0x8000) >> 16);
    }
    FLAG |= 1u << 17;
    return 0x1ffff;
}

static void rtp(int v, int shift, bool lm, bool last) {
    // the 44-bit accumulator is checked (and wraps) after every addition
    auto acc = [](int i, int64_t t, int64_t a, int64_t b, int64_t c) {
        int64_t v = mac_check(i, t + a);
        v = mac_check(i, v + b);
        return mac_check(i, v + c);
    };
    int64_t x = acc(1, (int64_t)TR(0) << 12, (int64_t)M(0, 0, 0) * VX(v), (int64_t)M(0, 0, 1) * VY(v), (int64_t)M(0, 0, 2) * VZ(v));
    int64_t y = acc(2, (int64_t)TR(1) << 12, (int64_t)M(0, 1, 0) * VX(v), (int64_t)M(0, 1, 1) * VY(v), (int64_t)M(0, 1, 2) * VZ(v));
    int64_t z = acc(3, (int64_t)TR(2) << 12, (int64_t)M(0, 2, 0) * VX(v), (int64_t)M(0, 2, 1) * VY(v), (int64_t)M(0, 2, 2) * VZ(v));
    set_mac_ir(1, x, shift, lm);
    set_mac_ir(2, y, shift, lm);
    set_mac(3, z, shift);
    // IR3 quirk: the saturation flag is computed from z >> 12, the value from MAC3
    {
        int32_t zs = (int32_t)(mac_check(3, z) >> 12);
        if (zs > 0x7fff || zs < -0x8000) FLAG |= 1u << 22;
        int32_t lo = lm ? 0 : -0x8000;
        IR3 = std::clamp<int32_t>(MAC3, lo, 0x7fff);
    }
    push_sz((int32_t)(mac_check(3, z) >> 12));
    uint32_t q = gte_divide(H, (uint32_t)d[19]);
    int64_t ir1 = IR1;
    if (g_gte_widescreen != 1.0f) ir1 = (int64_t)(ir1 * g_gte_widescreen);
    int64_t sx = (int64_t)q * ir1 + OFX;
    int64_t sy = (int64_t)q * IR2 + OFY;
    mac0_check(sx);
    mac0_check(sy);
    g_gte.on_vertex(v, (int32_t)x, (int32_t)y, (int32_t)z, sx, sy, q);
    push_sxy((int32_t)(sx >> 16), (int32_t)(sy >> 16));
    {
        // the same projection without integer rounding: X = OFX + IR1 * H / SZ
        double zf = (double)(mac_check(3, z)) / 4096.0;
        double sc = shift ? 4096.0 : 1.0;
        double ir1p = (double)mac_check(1, x) / sc, ir2p = (double)mac_check(2, y) / sc;
        if (g_gte_widescreen != 1.0f) ir1p *= g_gte_widescreen;
        bool ok = zf >= 1.0 && !(FLAG & ((1u << 17) | (1u << 14) | (1u << 13) | (1u << 24) | (1u << 23)));
        PreciseVtx p{0, 0, 0};
        if (ok) {
            double hz = (double)(uint16_t)H / zf;
            p.x = (float)(OFX / 65536.0 + ir1p * hz);
            p.y = (float)(OFY / 65536.0 + ir2p * hz);
            p.w = (float)zf;
            int32_t ix = (int16_t)(d[14] & 0xffff), iy = (int16_t)(d[14] >> 16);
            ok = std::fabs(p.x - ix) < 1.5f && std::fabs(p.y - iy) < 1.5f;   // agrees with the integer result
        }
        push_precise(ok, p);
    }
    MAC0 = (int32_t)sy;
    if (last) {
        int64_t dq = (int64_t)q * DQA + DQB;
        set_mac0(dq);
        set_ir0((int32_t)(dq >> 12));
    }
}

// matrix * vector (+ translation), sets MAC1-3 and IR1-3
static void mvmva(int m, int vsel, int tsel, int shift, bool lm) {
    int32_t vx, vy, vz;
    if (vsel == 3) { vx = (int16_t)IR1; vy = (int16_t)IR2; vz = (int16_t)IR3; }
    else { vx = VX(vsel); vy = VY(vsel); vz = VZ(vsel); }
    int16_t mm[3][3];
    if (m == 3) {
        // garbage matrix
        int16_t r = (int16_t)((d[6] & 0xff) << 4);
        mm[0][0] = -r; mm[0][1] = r; mm[0][2] = (int16_t)IR0;
        mm[1][0] = M(0, 0, 2); mm[1][1] = M(0, 0, 2); mm[1][2] = M(0, 0, 2);
        mm[2][0] = M(0, 1, 1); mm[2][1] = M(0, 1, 1); mm[2][2] = M(0, 1, 1);
    } else {
        for (int i = 0; i < 3; i++) for (int j = 0; j < 3; j++) mm[i][j] = M(m, i, j);
    }
    for (int i = 0; i < 3; i++) {
        int64_t t = 0;
        if (tsel == 0) t = (int64_t)TR(i) << 12;
        else if (tsel == 1) t = (int64_t)BK(i) << 12;
        else if (tsel == 2) t = (int64_t)FC(i) << 12;
        if (tsel == 2) {
            // far color bug: the first term only affects the flags
            int64_t f = mac_check(i + 1, t + (int64_t)mm[i][0] * vx);
            set_ir(i + 1, (int32_t)(f >> shift), false);
            int64_t v = (int64_t)mm[i][1] * vy + (int64_t)mm[i][2] * vz;
            set_mac_ir(i + 1, v, shift, lm);
        } else {
            int64_t v = mac_check(i + 1, t + (int64_t)mm[i][0] * vx);
            v = mac_check(i + 1, v + (int64_t)mm[i][1] * vy);
            v = v + (int64_t)mm[i][2] * vz;
            set_mac_ir(i + 1, v, shift, lm);
        }
    }
}

// depth cue: MAC = MAC + (FC - MAC) * IR0  (input MAC in "<< 12" units)
static void interpolate(int64_t m1, int64_t m2, int64_t m3, int shift, bool lm) {
    set_mac_ir(1, ((int64_t)FC(0) << 12) - m1, shift, false);
    set_mac_ir(2, ((int64_t)FC(1) << 12) - m2, shift, false);
    set_mac_ir(3, ((int64_t)FC(2) << 12) - m3, shift, false);
    set_mac_ir(1, (int64_t)IR1 * IR0 + m1, shift, lm);
    set_mac_ir(2, (int64_t)IR2 * IR0 + m2, shift, lm);
    set_mac_ir(3, (int64_t)IR3 * IR0 + m3, shift, lm);
}

static void nc_light(int v, int shift, bool lm) {
    mvmva(1, v, 3, shift, lm);       // LLM * V
    mvmva(2, 3, 1, shift, lm);       // BK + LCM * IR
}

static void color_mul(int shift, bool lm, bool depth) {
    uint8_t r = d[6] & 0xff, g = (d[6] >> 8) & 0xff, b = (d[6] >> 16) & 0xff;
    int64_t m1 = ((int64_t)r * IR1) << 4, m2 = ((int64_t)g * IR2) << 4, m3 = ((int64_t)b * IR3) << 4;
    if (depth) interpolate(m1, m2, m3, shift, lm);
    else { set_mac_ir(1, m1, shift, lm); set_mac_ir(2, m2, shift, lm); set_mac_ir(3, m3, shift, lm); }
    push_color();
}

void gte_command(uint32_t op) {
    if (!g_init) gte_init_tables();
    FLAG = 0;
    int shift = (op & (1 << 19)) ? 12 : 0;
    bool lm = (op & (1 << 10)) != 0;
    switch (op & 0x3f) {
        case 0x01: rtp(0, shift, lm, true); break;                                    // RTPS
        case 0x30: rtp(0, shift, lm, false); rtp(1, shift, lm, false); rtp(2, shift, lm, true); break;   // RTPT
        case 0x06: {                                                                  // NCLIP
            int32_t x0 = (int16_t)d[12], y0 = (int16_t)(d[12] >> 16);
            int32_t x1 = (int16_t)d[13], y1 = (int16_t)(d[13] >> 16);
            int32_t x2 = (int16_t)d[14], y2 = (int16_t)(d[14] >> 16);
            int64_t v = (int64_t)x0 * y1 + (int64_t)x1 * y2 + (int64_t)x2 * y0 - (int64_t)x0 * y2 - (int64_t)x1 * y0 - (int64_t)x2 * y1;
            set_mac0(v);
            break;
        }
        case 0x0c: {                                                                  // OP
            int32_t d1 = M(0, 0, 0), d2 = M(0, 1, 1), d3 = M(0, 2, 2);
            int64_t a = (int64_t)IR3 * d2 - (int64_t)IR2 * d3;
            int64_t b = (int64_t)IR1 * d3 - (int64_t)IR3 * d1;
            int64_t c = (int64_t)IR2 * d1 - (int64_t)IR1 * d2;
            set_mac_ir(1, a, shift, lm); set_mac_ir(2, b, shift, lm); set_mac_ir(3, c, shift, lm);
            break;
        }
        case 0x10: {                                                                  // DPCS
            uint8_t r = d[6] & 0xff, g = (d[6] >> 8) & 0xff, b = (d[6] >> 16) & 0xff;
            interpolate((int64_t)r << 16, (int64_t)g << 16, (int64_t)b << 16, shift, lm);
            push_color();
            break;
        }
        case 0x11:                                                                    // INTPL
            interpolate((int64_t)IR1 << 12, (int64_t)IR2 << 12, (int64_t)IR3 << 12, shift, lm);
            push_color();
            break;
        case 0x12:                                                                    // MVMVA
            mvmva((op >> 17) & 3, (op >> 15) & 3, (op >> 13) & 3, shift, lm);
            break;
        case 0x13: nc_light(0, shift, lm); color_mul(shift, lm, true); break;         // NCDS
        case 0x14: mvmva(2, 3, 1, shift, lm); color_mul(shift, lm, true); break;      // CDP
        case 0x16: for (int v = 0; v < 3; v++) { nc_light(v, shift, lm); color_mul(shift, lm, true); } break;   // NCDT
        case 0x1b: nc_light(0, shift, lm); color_mul(shift, lm, false); break;        // NCCS
        case 0x1c: mvmva(2, 3, 1, shift, lm); color_mul(shift, lm, false); break;     // CC
        case 0x1e: nc_light(0, shift, lm); push_color(); break;                       // NCS
        case 0x20: for (int v = 0; v < 3; v++) { nc_light(v, shift, lm); push_color(); } break;   // NCT
        case 0x28:                                                                    // SQR
            set_mac_ir(1, (int64_t)IR1 * IR1, shift, lm);
            set_mac_ir(2, (int64_t)IR2 * IR2, shift, lm);
            set_mac_ir(3, (int64_t)IR3 * IR3, shift, lm);
            break;
        case 0x29: color_mul(shift, lm, true); break;                                 // DCPL
        case 0x2a:                                                                    // DPCT
            for (int i = 0; i < 3; i++) {
                uint32_t c = (uint32_t)d[20];
                interpolate((int64_t)(c & 0xff) << 16, (int64_t)((c >> 8) & 0xff) << 16, (int64_t)((c >> 16) & 0xff) << 16, shift, lm);
                push_color();
            }
            break;
        case 0x2d: {                                                                  // AVSZ3
            int64_t v = (int64_t)ZSF3 * ((uint16_t)d[17] + (uint16_t)d[18] + (uint16_t)d[19]);
            set_mac0(v);
            int32_t o = (int32_t)(v >> 12);
            if (o < 0) { o = 0; FLAG |= 1u << 18; } else if (o > 0xffff) { o = 0xffff; FLAG |= 1u << 18; }
            d[7] = o;
            break;
        }
        case 0x2e: {                                                                  // AVSZ4
            int64_t v = (int64_t)ZSF4 * ((uint16_t)d[16] + (uint16_t)d[17] + (uint16_t)d[18] + (uint16_t)d[19]);
            set_mac0(v);
            int32_t o = (int32_t)(v >> 12);
            if (o < 0) { o = 0; FLAG |= 1u << 18; } else if (o > 0xffff) { o = 0xffff; FLAG |= 1u << 18; }
            d[7] = o;
            break;
        }
        case 0x3d:                                                                    // GPF
            set_mac_ir(1, (int64_t)IR0 * IR1, shift, lm);
            set_mac_ir(2, (int64_t)IR0 * IR2, shift, lm);
            set_mac_ir(3, (int64_t)IR0 * IR3, shift, lm);
            push_color();
            break;
        case 0x3e:                                                                    // GPL
            set_mac_ir(1, ((int64_t)MAC1 << shift) + (int64_t)IR0 * IR1, shift, lm);
            set_mac_ir(2, ((int64_t)MAC2 << shift) + (int64_t)IR0 * IR2, shift, lm);
            set_mac_ir(3, ((int64_t)MAC3 << shift) + (int64_t)IR0 * IR3, shift, lm);
            push_color();
            break;
        case 0x3f: for (int v = 0; v < 3; v++) { nc_light(v, shift, lm); color_mul(shift, lm, false); } break;   // NCCT
        default: break;
    }
    if (FLAG & 0x7f87e000u) FLAG |= 0x80000000u;
}

static inline int32_t clamp_rgb5(int32_t v) { return std::clamp(v >> 7, 0, 0x1f); }

void gte_store_sxy(uint32_t addr, int r) {
    uint32_t v = (uint32_t)d[r == 15 ? 14 : r];
    MEM_WW(addr, v);
    int i = r == 15 ? 2 : r - 12;
    if (g_pvalid[i]) precise_store(addr, v, g_pfifo[i]);
}

uint32_t gte_read_data(int r) {
    if (r >= 12 && r <= 15) {
        // mfc2 of a screen coordinate: the game moves it around in registers (swc2 has its own path)
        int i = r == 15 ? 2 : r - 12;
        if (g_pvalid[i]) precise_note((uint32_t)d[r == 15 ? 14 : r], g_pfifo[i]);
    }
    switch (r) {
        case 1: case 3: case 5: case 8: case 9: case 10: case 11:
            return (uint32_t)(int32_t)(int16_t)d[r];
        case 7: case 16: case 17: case 18: case 19:
            return (uint32_t)(uint16_t)d[r];
        case 15: return (uint32_t)d[14];
        case 28: case 29:
            return (uint32_t)(clamp_rgb5(IR1) | (clamp_rgb5(IR2) << 5) | (clamp_rgb5(IR3) << 10));
        default: return (uint32_t)d[r];
    }
}

void gte_write_data(int r, uint32_t v) {
    switch (r) {
        case 15: d[12] = d[13]; d[13] = d[14]; d[14] = (int32_t)v; push_precise(false, PreciseVtx{}); break;   // SXYP pushes
        case 12: case 13: case 14: d[r] = (int32_t)v; g_pvalid[r - 12] = false; break;
        case 28:
            IR1 = (int32_t)((v & 0x1f) << 7);
            IR2 = (int32_t)(((v >> 5) & 0x1f) << 7);
            IR3 = (int32_t)(((v >> 10) & 0x1f) << 7);
            break;
        case 29: break;   // ORGB read-only
        case 30: {
            d[30] = (int32_t)v;
            uint32_t x = (v & 0x80000000u) ? ~v : v;
            int n = 0;
            while (n < 32 && !(x & (0x80000000u >> n))) n++;
            d[31] = n;
            break;
        }
        case 31: break;   // LZCR read-only
        // the hardware registers are 16 bits wide: the upper half of the written word is dropped
        case 1: case 3: case 5: case 8: case 9: case 10: case 11:
            d[r] = (int32_t)(int16_t)v; break;
        case 7: case 16: case 17: case 18: case 19:
            d[r] = (int32_t)(uint16_t)v; break;
        default: d[r] = (int32_t)v; break;
    }
}

uint32_t gte_read_ctrl(int r) {
    switch (r) {
        case 4: case 12: case 20: case 26: case 27: case 29: case 30:
            return (uint32_t)(int32_t)(int16_t)cr[r];
        default: return (uint32_t)cr[r];
    }
}

void gte_write_ctrl(int r, uint32_t v) {
    switch (r) {
        case 4: case 12: case 20: case 26: case 27: case 29: case 30:
            cr[r] = (int32_t)(int16_t)v; break;
        case 31:
            cr[31] = (int32_t)(v & 0x7ffff000u);
            if (v & 0x7f87e000u) cr[31] |= (int32_t)0x80000000u;
            break;
        default: cr[r] = (int32_t)v; break;
    }
}

void gte_reset() {
    for (int i = 0; i < 32; i++) d[i] = cr[i] = 0;
}
