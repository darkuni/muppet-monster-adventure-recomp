// SPU: 24 ADPCM voices, ADSR/sweep envelopes, pitch modulation, noise, reverb, CD audio
// input, capture buffers, transfers and the SPU interrupt - following psx-spx. Runs at
// 44.1 kHz in batches from the scheduler and feeds the host audio ring.
#include "hw.h"
#include "log.h"
#include "rumble.h"
#include "spu_gauss.h"
#include "audio_ring.h"
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <vector>

void platform_audio_push(const int16_t* lr, int frames);

static uint8_t g_sram[512 * 1024];
static uint16_t g_regs[0x200];        // raw register file (0x1F801C00..0x1F801FFF / 2)

static const int CYCLES_PER_SAMPLE = 768;
static const int BATCH = 32;

static inline int16_t rd16(uint32_t a) { int16_t v; memcpy(&v, g_sram + (a & 0x7fffe), 2); return v; }
static inline void wr16(uint32_t a, int16_t v) { memcpy(g_sram + (a & 0x7fffe), &v, 2); }
static inline int32_t sat16(int32_t v) { return v < -32768 ? -32768 : v > 32767 ? 32767 : v; }

// ---------------------------------------------------------------- envelopes
struct Envelope {
    int32_t counter = 0;
    // computes one 44.1 kHz step; returns new level
    static int32_t step(int32_t level, int32_t& counter, bool exp, bool decrease, bool negative_phase, int shift, int stepv) {
        int32_t s = 7 - stepv;
        if (decrease != negative_phase) s = ~s;
        s <<= std::max(0, 11 - shift);
        int32_t inc = 0x8000 >> std::max(0, shift - 11);
        if (exp && !decrease && level > 0x6000) {
            if (shift < 10) s >>= 2;
            else if (shift >= 11) inc >>= 2;
            else { s >>= 1; inc >>= 1; }
        } else if (exp && decrease) {
            s = (int32_t)(((int64_t)s * level) >> 15);
        }
        if ((stepv | (shift << 2)) != 0x7f) inc = std::max(inc, 1);
        else return level;   // never steps
        counter += inc;
        if (!(counter & 0x8000)) return level;
        counter = 0;
        level += s;
        if (!decrease) level = std::clamp(level, -0x8000, 0x7fff);
        else if (negative_phase) level = std::clamp(level, -0x8000, 0);
        else level = std::max(level, 0);
        return level;
    }
};

struct Volume {   // a VOLL/VOLR register with sweep
    uint16_t reg = 0;
    int32_t level = 0;
    int32_t counter = 0;
    void write(uint16_t v) { reg = v; if (!(v & 0x8000)) level = (int16_t)(v << 1); counter = 0; }
    void tick() {
        if (!(reg & 0x8000)) return;
        bool exp = reg & 0x4000, dec = reg & 0x2000, neg = reg & 0x1000;
        int shift = (reg >> 2) & 0x1f, stepv = reg & 3;
        level = Envelope::step(level, counter, exp, dec, neg, shift, stepv);
    }
};

enum Phase { OFF, ATTACK, DECAY, SUSTAIN, RELEASE };

struct Voice {
    Volume vol_l, vol_r;
    uint16_t pitch = 0, start = 0, adsr1 = 0, adsr2 = 0, repeat = 0;
    int32_t env = 0, env_counter = 0;
    Phase phase = OFF;
    uint32_t addr = 0;                 // current block byte address
    uint32_t counter = 0;              // pitch counter (12.12)
    int16_t buf[3 + 28];               // [0..2] previous samples, [3..30] current block
    int32_t p1 = 0, p2 = 0;            // ADPCM filter history
    uint8_t flags = 0;
    bool block_valid = false;
    int32_t out = 0;                   // last output (for pitch modulation)
    bool ignore_loop_write = false;
};
static Voice g_v[24];
static uint32_t g_endx = 0;
static Volume g_main_l, g_main_r;
static uint32_t g_tsa = 0;             // current transfer address (bytes)
static int32_t g_noise_timer = 0, g_noise_level = 1;
static uint32_t g_reverb_addr = 0;
static int64_t g_last_cycles = 0;
static uint32_t g_capture_pos = 0;
static bool g_irq_flag = false;

#define R(off) g_regs[(off) >> 1]
static uint16_t attr() { return R(0x1aa); }

static void check_irq(uint32_t byte_addr) {
    if (!(attr() & 0x8000) || !(attr() & 0x40) || g_irq_flag) return;
    uint32_t irqa = (uint32_t)R(0x1a4) * 8;
    if ((byte_addr & ~7u) == (irqa & ~7u) || (byte_addr & ~15u) == (irqa & ~15u)) {
        g_irq_flag = true;
        irq_raise(IRQ_SPU);
        LOG("spu", "IRQ at %05x", byte_addr);
    }
}

// ---------------------------------------------------------------- CD audio input FIFO
static std::vector<int16_t> g_cd_fifo;   // interleaved stereo at 44.1 kHz
static size_t g_cd_rd = 0;
void spu_cd_audio(const int16_t* lr, int frames, int rate) {
    (void)rate;
    if (g_cd_rd > 0 && g_cd_rd * 2 > g_cd_fifo.size()) {
        g_cd_fifo.erase(g_cd_fifo.begin(), g_cd_fifo.begin() + g_cd_rd);
        g_cd_rd = 0;
    }
    if (g_cd_fifo.size() - g_cd_rd > 44100 * 2) return;   // more than a second queued: drop
    g_cd_fifo.insert(g_cd_fifo.end(), lr, lr + frames * 2);
}

// ---------------------------------------------------------------- voices
static const int fk0[5] = {0, 60, 115, 98, 122};
static const int fk1[5] = {0, 0, -52, -55, -60};

static void decode_block(Voice& v) {
    check_irq(v.addr);
    const uint8_t* b = g_sram + (v.addr & 0x7fff0);
    int shift = b[0] & 15;
    if (shift > 12) shift = 9;
    int filt = std::min((b[0] >> 4) & 7, 4);
    v.flags = b[1];
    // keep the last three samples for interpolation
    v.buf[0] = v.buf[28]; v.buf[1] = v.buf[29]; v.buf[2] = v.buf[30];
    for (int i = 0; i < 28; i++) {
        int nib = (b[2 + i / 2] >> ((i & 1) * 4)) & 15;
        int32_t s = (int16_t)(nib << 12) >> shift;
        s += (v.p1 * fk0[filt] + v.p2 * fk1[filt] + 32) >> 6;
        s = sat16(s);
        v.p2 = v.p1; v.p1 = s;
        v.buf[3 + i] = (int16_t)s;
    }
    if (v.flags & 4) v.repeat = (uint16_t)(v.addr >> 3);
    v.block_valid = true;
}

extern unsigned g_frame_no;
static void key_on(int i) {
    Voice& v = g_v[i];
    {
        uint32_t h = 2166136261u, a = (uint32_t)v.start * 8;
        for (int k = 0; k < 64; k++) h = (h ^ g_sram[(a + k) & 0x7ffff]) * 16777619u;
        int vol = std::max(R(i * 16) & 0x8000 ? 0 : R(i * 16) & 0x7fff, R(i * 16 + 2) & 0x8000 ? 0 : R(i * 16 + 2) & 0x7fff);
        rumble_sample(h, vol);
    }
    if (log_enabled("kon")) {
        uint32_t h = 2166136261u, a = (uint32_t)v.start * 8;
        for (int k = 0; k < 64; k++) h = (h ^ g_sram[(a + k) & 0x7ffff]) * 16777619u;
        char hex[129];
        for (int k = 0; k < 64; k++) snprintf(hex + 2 * k, 3, "%02x", g_sram[(a + k) & 0x7ffff]);
        fprintf(stderr, "[kon] f%u v%d start %05x hash %08x pitch %04x vol %04x %04x data %s\n", g_frame_no, i, a, h, v.pitch, R(i * 16), R(i * 16 + 2), hex);
    }
    v.addr = (uint32_t)v.start * 8;
    v.counter = 0;
    v.p1 = v.p2 = 0;
    memset(v.buf, 0, sizeof v.buf);
    v.env = 0;
    v.env_counter = 0;
    v.phase = ATTACK;
    v.block_valid = false;
    g_endx &= ~(1u << i);
}
static void key_off(int i) {
    if (g_v[i].phase != OFF) { g_v[i].phase = RELEASE; g_v[i].env_counter = 0; }
}

static void env_tick(Voice& v) {
    switch (v.phase) {
        case ATTACK: {
            bool exp = v.adsr1 & 0x8000;
            int shift = (v.adsr1 >> 10) & 0x1f, st = (v.adsr1 >> 8) & 3;
            v.env = Envelope::step(v.env, v.env_counter, exp, false, false, shift, st);
            if (v.env >= 0x7fff) { v.phase = DECAY; v.env_counter = 0; }
            break;
        }
        case DECAY: {
            int shift = (v.adsr1 >> 4) & 0xf;
            v.env = Envelope::step(v.env, v.env_counter, true, true, false, shift, 0);
            int32_t sl = ((v.adsr1 & 0xf) + 1) * 0x800;
            if (v.env <= sl) { v.phase = SUSTAIN; v.env_counter = 0; }
            break;
        }
        case SUSTAIN: {
            bool exp = v.adsr2 & 0x8000, dec = v.adsr2 & 0x4000;
            int shift = (v.adsr2 >> 8) & 0x1f, st = (v.adsr2 >> 6) & 3;
            v.env = Envelope::step(v.env, v.env_counter, exp, dec, false, shift, st);
            break;
        }
        case RELEASE: {
            bool exp = v.adsr2 & 0x20;
            int shift = v.adsr2 & 0x1f;
            v.env = Envelope::step(v.env, v.env_counter, exp, true, false, shift, 0);
            if (v.env <= 0) { v.env = 0; v.phase = OFF; }
            break;
        }
        default: break;
    }
}

static int32_t voice_sample(int i, int32_t noise) {
    Voice& v = g_v[i];
    if (!v.block_valid) decode_block(v);
    int idx = (int)(v.counter >> 12);
    int gi = (v.counter >> 4) & 0xff;
    int32_t s;
    if ((R(0x194) | ((uint32_t)R(0x196) << 16)) & (1u << i)) {
        s = noise;
    } else {
        s = ((kGauss[0xff - gi] * v.buf[idx]) >> 15) + ((kGauss[0x1ff - gi] * v.buf[idx + 1]) >> 15) +
            ((kGauss[0x100 + gi] * v.buf[idx + 2]) >> 15) + ((kGauss[gi] * v.buf[idx + 3]) >> 15);
    }
    int32_t out = (int32_t)(((int64_t)s * v.env) >> 15);
    // advance pitch counter
    uint32_t step = v.pitch;
    uint32_t pmon = R(0x190) | ((uint32_t)R(0x192) << 16);
    if (i > 0 && (pmon & (1u << i))) {
        int32_t factor = g_v[i - 1].out + 0x8000;
        int32_t st = (int16_t)step;
        st = (int32_t)(((int64_t)st * factor) >> 15);
        step = (uint32_t)st & 0xffff;
    }
    if (step > 0x3fff) step = 0x4000;
    v.counter += step;
    while ((v.counter >> 12) >= 28) {
        v.counter -= 28 << 12;
        // finished this block: handle flags, move on
        if (v.flags & 1) {
            g_endx |= 1u << i;
            v.addr = (uint32_t)v.repeat * 8;
            if (!(v.flags & 2)) {
                v.phase = OFF;
                v.env = 0;
            }
        } else {
            v.addr = (v.addr + 16) & 0x7fff0;
        }
        decode_block(v);
    }
    v.out = out;
    env_tick(v);
    v.vol_l.tick();
    v.vol_r.tick();
    return out;
}

// ---------------------------------------------------------------- reverb
static const int16_t kFir[39] = {-1, 0, 2, 0, -10, 0, 35, 0, -103, 0, 266, 0, -616, 0, 1332, 0, -2960, 0, 10246, 16384,
                                  10246, 0, -2960, 0, 1332, 0, -616, 0, 266, 0, -103, 0, 35, 0, -10, 0, 2, 0, -1};
static int16_t g_rin[2][64];     // reverb input history (44.1 kHz)
static int16_t g_rout[2][64];    // reverb output (zero-stuffed 44.1 kHz)
static int g_rpos = 0;
static bool g_odd = false;

static uint32_t rv_addr(int32_t rel) {
    uint32_t base = (uint32_t)R(0x1a2) * 8;
    int64_t a = (int64_t)g_reverb_addr + rel;
    uint32_t size = 0x80000 - base;
    if (size == 0) return base;
    while (a >= 0x80000) a -= size;
    while (a < (int64_t)base) a += size;
    return (uint32_t)a & 0x7fffe;
}
static int16_t rr(int32_t rel) { return rd16(rv_addr(rel)); }
static void rw(int32_t rel, int32_t v) {
    uint32_t a = rv_addr(rel);
    check_irq(a);
    if (attr() & 0x80) wr16(a, (int16_t)sat16(v));
}
static inline int32_t mul(int32_t a, int32_t b) { return (a * b) >> 15; }

static void reverb_compute(int32_t in_l, int32_t in_r, int32_t* out_l, int32_t* out_r) {
    int32_t dAPF1 = R(0x1c0) * 8, dAPF2 = R(0x1c2) * 8;
    int32_t vIIR = (int16_t)R(0x1c4), vCOMB1 = (int16_t)R(0x1c6), vCOMB2 = (int16_t)R(0x1c8), vCOMB3 = (int16_t)R(0x1ca),
            vCOMB4 = (int16_t)R(0x1cc), vWALL = (int16_t)R(0x1ce), vAPF1 = (int16_t)R(0x1d0), vAPF2 = (int16_t)R(0x1d2);
    int32_t mLSAME = R(0x1d4) * 8, mRSAME = R(0x1d6) * 8, mLCOMB1 = R(0x1d8) * 8, mRCOMB1 = R(0x1da) * 8,
            mLCOMB2 = R(0x1dc) * 8, mRCOMB2 = R(0x1de) * 8, dLSAME = R(0x1e0) * 8, dRSAME = R(0x1e2) * 8,
            mLDIFF = R(0x1e4) * 8, mRDIFF = R(0x1e6) * 8, mLCOMB3 = R(0x1e8) * 8, mRCOMB3 = R(0x1ea) * 8,
            mLCOMB4 = R(0x1ec) * 8, mRCOMB4 = R(0x1ee) * 8, dLDIFF = R(0x1f0) * 8, dRDIFF = R(0x1f2) * 8,
            mLAPF1 = R(0x1f4) * 8, mRAPF1 = R(0x1f6) * 8, mLAPF2 = R(0x1f8) * 8, mRAPF2 = R(0x1fa) * 8;
    int32_t vLIN = (int16_t)R(0x1fc), vRIN = (int16_t)R(0x1fe);
    int32_t Lin = mul(vLIN, in_l), Rin = mul(vRIN, in_r);
    rw(mLSAME, mul(Lin + mul(rr(dLSAME), vWALL) - rr(mLSAME - 2), vIIR) + rr(mLSAME - 2));
    rw(mRSAME, mul(Rin + mul(rr(dRSAME), vWALL) - rr(mRSAME - 2), vIIR) + rr(mRSAME - 2));
    rw(mLDIFF, mul(Lin + mul(rr(dRDIFF), vWALL) - rr(mLDIFF - 2), vIIR) + rr(mLDIFF - 2));
    rw(mRDIFF, mul(Rin + mul(rr(dLDIFF), vWALL) - rr(mRDIFF - 2), vIIR) + rr(mRDIFF - 2));
    int32_t Lout = mul(vCOMB1, rr(mLCOMB1)) + mul(vCOMB2, rr(mLCOMB2)) + mul(vCOMB3, rr(mLCOMB3)) + mul(vCOMB4, rr(mLCOMB4));
    int32_t Rout = mul(vCOMB1, rr(mRCOMB1)) + mul(vCOMB2, rr(mRCOMB2)) + mul(vCOMB3, rr(mRCOMB3)) + mul(vCOMB4, rr(mRCOMB4));
    Lout = sat16(Lout); Rout = sat16(Rout);
    Lout = sat16(Lout - mul(vAPF1, rr(mLAPF1 - dAPF1))); rw(mLAPF1, Lout); Lout = sat16(mul(Lout, vAPF1) + rr(mLAPF1 - dAPF1));
    Rout = sat16(Rout - mul(vAPF1, rr(mRAPF1 - dAPF1))); rw(mRAPF1, Rout); Rout = sat16(mul(Rout, vAPF1) + rr(mRAPF1 - dAPF1));
    Lout = sat16(Lout - mul(vAPF2, rr(mLAPF2 - dAPF2))); rw(mLAPF2, Lout); Lout = sat16(mul(Lout, vAPF2) + rr(mLAPF2 - dAPF2));
    Rout = sat16(Rout - mul(vAPF2, rr(mRAPF2 - dAPF2))); rw(mRAPF2, Rout); Rout = sat16(mul(Rout, vAPF2) + rr(mRAPF2 - dAPF2));
    *out_l = Lout; *out_r = Rout;
    uint32_t base = (uint32_t)R(0x1a2) * 8;
    g_reverb_addr = std::max(base, (g_reverb_addr + 2) & 0x7fffe);
}

static void reverb_sample(int32_t in_l, int32_t in_r, int32_t* out_l, int32_t* out_r) {
    g_rpos = (g_rpos + 1) & 63;
    g_rin[0][g_rpos] = (int16_t)sat16(in_l);
    g_rin[1][g_rpos] = (int16_t)sat16(in_r);
    g_rout[0][g_rpos] = g_rout[1][g_rpos] = 0;
    if (g_odd) {
        int32_t dl = 0, dr = 0;
        for (int k = 0; k < 39; k++) {
            int p = (g_rpos - k) & 63;
            dl += kFir[k] * g_rin[0][p];
            dr += kFir[k] * g_rin[1][p];
        }
        int32_t ol, orr;
        reverb_compute(sat16(dl >> 15), sat16(dr >> 15), &ol, &orr);
        g_rout[0][g_rpos] = (int16_t)ol;
        g_rout[1][g_rpos] = (int16_t)orr;
    }
    g_odd = !g_odd;
    int32_t ul = 0, ur = 0;
    for (int k = 0; k < 39; k++) {
        int p = (g_rpos - k) & 63;
        ul += kFir[k] * g_rout[0][p];
        ur += kFir[k] * g_rout[1][p];
    }
    *out_l = sat16(ul >> 14);
    *out_r = sat16(ur >> 14);
}

// ---------------------------------------------------------------- mixer
static void noise_tick() {
    int shift = (attr() >> 10) & 15, st = ((attr() >> 8) & 3) + 4;
    g_noise_timer -= st;
    int parity = ((g_noise_level >> 15) ^ (g_noise_level >> 12) ^ (g_noise_level >> 11) ^ (g_noise_level >> 10) ^ 1) & 1;
    if (g_noise_timer < 0) {
        g_noise_level = (int16_t)((g_noise_level << 1) | parity);
        g_noise_timer += 0x20000 >> shift;
        if (g_noise_timer < 0) g_noise_timer += 0x20000 >> shift;
    }
}

static void mix_sample(int16_t* out) {
    int32_t L = 0, Rr = 0, revL = 0, revR = 0;
    uint32_t eon = R(0x198) | ((uint32_t)R(0x19a) << 16);
    noise_tick();
    for (int i = 0; i < 24; i++) {
        Voice& v = g_v[i];
        if (v.phase == OFF && v.env == 0) {
            // voices keep reading memory (IRQ) even when silent
            if (v.block_valid) { v.out = 0; continue; }
            continue;
        }
        int32_t s = voice_sample(i, g_noise_level);
        int32_t l = (s * v.vol_l.level) >> 15, r = (s * v.vol_r.level) >> 15;
        L += l; Rr += r;
        if (eon & (1u << i)) { revL += l; revR += r; }
    }
    // CD audio
    int16_t cdl = 0, cdr = 0;
    if (g_cd_rd + 1 < g_cd_fifo.size()) { cdl = g_cd_fifo[g_cd_rd]; cdr = g_cd_fifo[g_cd_rd + 1]; g_cd_rd += 2; }
    // capture buffers (CD left/right, voice 1/3)
    wr16(0x000 + g_capture_pos * 2, cdl);
    wr16(0x400 + g_capture_pos * 2, cdr);
    wr16(0x800 + g_capture_pos * 2, (int16_t)sat16(g_v[1].out));
    wr16(0xc00 + g_capture_pos * 2, (int16_t)sat16(g_v[3].out));
    g_capture_pos = (g_capture_pos + 1) & 0x1ff;
    if (attr() & 1) {
        int32_t cl = (cdl * (int16_t)R(0x1b0)) >> 15, cr = (cdr * (int16_t)R(0x1b2)) >> 15;
        L += cl; Rr += cr;
        if (attr() & 4) { revL += cl; revR += cr; }
    }
    int32_t rl, rr2;
    reverb_sample(sat16(revL), sat16(revR), &rl, &rr2);
    L += (rl * (int16_t)R(0x184)) >> 15;
    Rr += (rr2 * (int16_t)R(0x186)) >> 15;
    g_main_l.tick(); g_main_r.tick();
    L = sat16((sat16(L) * g_main_l.level) >> 15);
    Rr = sat16((sat16(Rr) * g_main_r.level) >> 15);
    if (!(attr() & 0x4000) || !(attr() & 0x8000)) {   // muted / disabled: only CD audio passes
        L = (attr() & 1) ? sat16((cdl * (int16_t)R(0x1b0)) >> 15) : 0;
        Rr = (attr() & 1) ? sat16((cdr * (int16_t)R(0x1b2)) >> 15) : 0;
    }
    out[0] = (int16_t)L;
    out[1] = (int16_t)Rr;
}

static void spu_run() {
    int64_t now = g_cpu.cycles;
    int64_t n = (now - g_last_cycles) / CYCLES_PER_SAMPLE;
    if (n <= 0) return;
    g_last_cycles += n * CYCLES_PER_SAMPLE;
    static std::vector<int16_t> buf;
    buf.resize((size_t)n * 2);
    for (int64_t i = 0; i < n; i++) mix_sample(&buf[(size_t)i * 2]);
    platform_audio_push(buf.data(), (int)n);
}

void spu_event() {
    spu_run();
    sched_in(EV_SPU, BATCH * CYCLES_PER_SAMPLE);
}

// ---------------------------------------------------------------- registers
uint16_t spu_read(uint32_t off) {
    off &= 0x3fe;
    if (off < 0x180) {
        Voice& v = g_v[off >> 4];
        switch (off & 0xf) {
            case 0x0: return v.vol_l.reg;
            case 0x2: return v.vol_r.reg;
            case 0x4: return v.pitch;
            case 0x6: return v.start;
            case 0x8: return v.adsr1;
            case 0xa: return v.adsr2;
            case 0xc: return (uint16_t)v.env;
            case 0xe: return v.repeat;
        }
    }
    switch (off) {
        case 0x19c: return (uint16_t)g_endx;
        case 0x19e: return (uint16_t)(g_endx >> 16);
        case 0x1ae: {
            uint16_t s = attr() & 0x3f;
            if (g_irq_flag) s |= 0x40;
            if ((attr() & 0x30) == 0x20) s |= 0x80 | 0x100;
            if ((attr() & 0x30) == 0x30) s |= 0x80 | 0x200;
            if (g_capture_pos >= 0x100) s |= 0x800;
            return s;
        }
        case 0x1b8: return (uint16_t)g_main_l.level;
        case 0x1ba: return (uint16_t)g_main_r.level;
    }
    if (off >= 0x200 && off < 0x260) {   // current voice volumes
        int i = (off - 0x200) >> 2;
        return (uint16_t)((off & 2) ? g_v[i].vol_r.level : g_v[i].vol_l.level);
    }
    return R(off);
}

void spu_write(uint32_t off, uint16_t v) {
    off &= 0x3fe;
    spu_run();   // apply register changes at the right point in the sample stream
    R(off) = v;
    if (off < 0x180) {
        Voice& vc = g_v[off >> 4];
        switch (off & 0xf) {
            case 0x0: vc.vol_l.write(v); break;
            case 0x2: vc.vol_r.write(v); break;
            case 0x4: vc.pitch = v; break;
            case 0x6: vc.start = v; break;
            case 0x8: vc.adsr1 = v; break;
            case 0xa: vc.adsr2 = v; break;
            case 0xc: vc.env = (int16_t)v; break;
            case 0xe: vc.repeat = v; break;
        }
        return;
    }
    switch (off) {
        case 0x180: g_main_l.write(v); break;
        case 0x182: g_main_r.write(v); break;
        case 0x188: for (int i = 0; i < 16; i++) if (v & (1 << i)) key_on(i); break;
        case 0x18a: for (int i = 0; i < 8; i++) if (v & (1 << i)) key_on(16 + i); break;
        case 0x18c: for (int i = 0; i < 16; i++) if (v & (1 << i)) key_off(i); break;
        case 0x18e: for (int i = 0; i < 8; i++) if (v & (1 << i)) key_off(16 + i); break;
        case 0x1a2: g_reverb_addr = (uint32_t)v * 8; break;
        case 0x1a6: g_tsa = (uint32_t)v * 8; break;
        case 0x1a8:   // manual write FIFO
            check_irq(g_tsa);
            wr16(g_tsa, (int16_t)v);
            g_tsa = (g_tsa + 2) & 0x7fffe;
            break;
        case 0x1aa:
            if (!(v & 0x40)) g_irq_flag = false;
            break;
    }
}

void spu_dma_write(uint32_t w) {
    check_irq(g_tsa);
    wr16(g_tsa, (int16_t)w); g_tsa = (g_tsa + 2) & 0x7fffe;
    wr16(g_tsa, (int16_t)(w >> 16)); g_tsa = (g_tsa + 2) & 0x7fffe;
}
uint32_t spu_dma_read() {
    check_irq(g_tsa);
    uint32_t lo = (uint16_t)rd16(g_tsa); g_tsa = (g_tsa + 2) & 0x7fffe;
    uint32_t hi = (uint16_t)rd16(g_tsa); g_tsa = (g_tsa + 2) & 0x7fffe;
    return lo | (hi << 16);
}

void spu_init() {
    memset(g_sram, 0, sizeof g_sram);
    memset(g_regs, 0, sizeof g_regs);
    g_last_cycles = g_cpu.cycles;
    sched_in(EV_SPU, BATCH * CYCLES_PER_SAMPLE);
}
