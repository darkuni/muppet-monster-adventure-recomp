// MDEC macroblock decoder (FMV playback), following psx-spx: run-length/quantized DCT
// blocks, the IDCT with the MDEC(3) scale table, and YUV -> RGB at 4/8/15/24 bit depth.
#include "hw.h"
#include "log.h"
#include <algorithm>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <vector>

void dma_device_ready(int ch);

static const uint8_t kZigzag[64] = {
    0, 1, 5, 6, 14, 15, 27, 28, 2, 4, 7, 13, 16, 26, 29, 42, 3, 8, 12, 17, 25, 30, 41, 43, 9, 11, 18, 24, 31, 40, 44, 53,
    10, 19, 23, 32, 39, 45, 52, 54, 20, 22, 33, 38, 46, 51, 55, 60, 21, 34, 37, 47, 50, 56, 59, 61, 35, 36, 48, 49, 57, 58, 62, 63};
static uint8_t kZagzig[64];

static uint8_t g_iq_y[64], g_iq_uv[64];
static int16_t g_scale[64];
static uint32_t g_cmd = 0;
static int g_remaining = 0;              // parameter words still expected
static int g_cur_cmd = 0;
static std::vector<uint32_t> g_in;
static std::vector<uint32_t> g_out;
static size_t g_out_pos = 0;
static bool g_en_in = false, g_en_out = false;
static uint32_t g_status_bits = 0;       // bits 23-26 reflected from the command

static void idct(int16_t* blk) {
    int64_t tmp[64];
    int64_t src[64];
    for (int i = 0; i < 64; i++) src[i] = blk[i];
    for (int pass = 0; pass < 2; pass++) {
        for (int x = 0; x < 8; x++) {
            for (int y = 0; y < 8; y++) {
                int64_t sum = 0;
                for (int z = 0; z < 8; z++) sum += src[y + z * 8] * (g_scale[x + z * 8] >> 3);
                tmp[x + y * 8] = (sum + 0xfff) >> 13;
            }
        }
        memcpy(src, tmp, sizeof src);
    }
    for (int i = 0; i < 64; i++) blk[i] = (int16_t)std::clamp<int64_t>(src[i], -32768, 32767);
}

// returns false when the input ran out
static bool rl_decode(const uint16_t* data, size_t n, size_t& pos, int16_t* blk, const uint8_t* qt) {
    for (int i = 0; i < 64; i++) blk[i] = 0;
    uint16_t v;
    do {
        if (pos >= n) return false;
        v = data[pos++];
    } while (v == 0xfe00);
    int q_scale = (v >> 10) & 0x3f;
    int k = 0;
    auto s10 = [](uint16_t x) { return (int32_t)((int16_t)(x << 6) >> 6); };
    int32_t val = s10(v & 0x3ff) * qt[k];
    for (;;) {
        if (q_scale == 0) val = s10(v & 0x3ff) * 2;
        val = std::clamp(val, -0x400, 0x3ff);
        if (q_scale > 0) blk[kZagzig[k]] = (int16_t)val;
        else blk[k] = (int16_t)val;
        if (pos >= n) break;
        v = data[pos++];
        k += ((v >> 10) & 0x3f) + 1;
        if (k > 63) break;
        val = (s10(v & 0x3ff) * qt[k] * q_scale + 4) / 8;
    }
    idct(blk);
    return true;
}

static void decode() {
    int depth = (g_cmd >> 27) & 3;
    bool sign = (g_cmd >> 26) & 1;
    bool bit15 = (g_cmd >> 25) & 1;
    const uint16_t* hw = (const uint16_t*)g_in.data();
    size_t n = g_in.size() * 2, pos = 0;
    int16_t cr[64], cb[64], y[4][64];
    std::vector<uint8_t> bytes;
    while (pos < n) {
        if (depth >= 2) {
            if (!rl_decode(hw, n, pos, cr, g_iq_uv)) break;
            if (!rl_decode(hw, n, pos, cb, g_iq_uv)) break;
            bool ok = true;
            for (int b = 0; b < 4 && ok; b++) ok = rl_decode(hw, n, pos, y[b], g_iq_y);
            if (!ok) break;
            uint32_t px[256];
            for (int b = 0; b < 4; b++) {
                int xx = (b & 1) * 8, yy = (b >> 1) * 8;
                for (int yi = 0; yi < 8; yi++) for (int xi = 0; xi < 8; xi++) {
                    int ci = ((xi + xx) / 2) + ((yi + yy) / 2) * 8;
                    int R = cr[ci], B = cb[ci];
                    int G = (int)(-0.3437 * B - 0.7143 * R);
                    R = (int)(1.402 * R);
                    B = (int)(1.772 * B);
                    int Y = y[b][xi + yi * 8];
                    int r = std::clamp(Y + R, -128, 127), g = std::clamp(Y + G, -128, 127), bl = std::clamp(Y + B, -128, 127);
                    uint32_t bgr = (uint32_t)(r & 0xff) | ((uint32_t)(g & 0xff) << 8) | ((uint32_t)(bl & 0xff) << 16);
                    if (!sign) bgr ^= 0x808080;
                    px[(xi + xx) + (yi + yy) * 16] = bgr;
                }
            }
            if (depth == 2) {
                for (int i = 0; i < 256; i++) { bytes.push_back(px[i] & 0xff); bytes.push_back((px[i] >> 8) & 0xff); bytes.push_back((px[i] >> 16) & 0xff); }
            } else {
                for (int i = 0; i < 256; i++) {
                    uint32_t p = px[i];
                    uint16_t c = (uint16_t)(((p >> 3) & 0x1f) | (((p >> 11) & 0x1f) << 5) | (((p >> 19) & 0x1f) << 10) | (bit15 ? 0x8000 : 0));
                    bytes.push_back(c & 0xff); bytes.push_back(c >> 8);
                }
            }
        } else {
            if (!rl_decode(hw, n, pos, y[0], g_iq_y)) break;
            uint8_t px[64];
            for (int i = 0; i < 64; i++) {
                int Y = y[0][i] & 0x1ff;
                if (Y & 0x100) Y -= 0x200;
                Y = std::clamp(Y, -128, 127);
                if (!sign) Y ^= 0x80;
                px[i] = (uint8_t)Y;
            }
            if (depth == 1) bytes.insert(bytes.end(), px, px + 64);
            else for (int i = 0; i < 64; i += 2) bytes.push_back((uint8_t)((px[i] >> 4) | (px[i + 1] & 0xf0)));
        }
    }
    while (bytes.size() & 3) bytes.push_back(0);
    size_t base = g_out.size();
    g_out.resize(base + bytes.size() / 4);
    memcpy(g_out.data() + base, bytes.data(), bytes.size());
    LOG("mdec", "decoded %zu input words -> %zu output words (cmd %08x)", g_in.size(), bytes.size() / 4, g_cmd);
    if (getenv("MDEC_DUMP")) {
        static int k = 0;
        char fn[64]; snprintf(fn, sizeof fn, "mdec_in_%03d.bin", k++);
        FILE* f = fopen(fn, "wb"); if (f) { fwrite(&g_cmd, 4, 1, f); fwrite(g_in.data(), 4, g_in.size(), f); fclose(f); }
    }
    if (log_enabled("mdecdata")) {
        char b[512]; b[0] = 0;
        for (int i = 0; i < 24 && i < (int)n; i++) snprintf(b + strlen(b), 8, "%04x ", hw[i]);
        LOG("mdecdata", "%s", b);
    }
}

static void param(uint32_t w) {
    if (g_remaining <= 0) {
        // new command
        g_cmd = w;
        g_cur_cmd = w >> 29;
        g_status_bits = (w >> 2) & (0xfu << 23);
        g_in.clear();
        switch (g_cur_cmd) {
            case 1:
                g_remaining = w & 0xffff;
                if (g_out_pos < g_out.size()) LOG("mdec", "new decode drops %zu unread output words", g_out.size() - g_out_pos);
                g_out.clear(); g_out_pos = 0;
                break;
            case 2: g_remaining = (w & 1) ? 32 : 16; break;
            case 3: g_remaining = 32; break;
            default: g_remaining = 0; break;
        }
        return;
    }
    g_in.push_back(w);
    if (--g_remaining > 0) return;
    if (g_cur_cmd == 1) {
        decode();
        dma_device_ready(1);
    } else if (g_cur_cmd == 2) {
        const uint8_t* b = (const uint8_t*)g_in.data();
        memcpy(g_iq_y, b, 64);
        if (g_in.size() >= 32) memcpy(g_iq_uv, b + 64, 64);
    } else if (g_cur_cmd == 3) {
        memcpy(g_scale, g_in.data(), 128);
    }
}

void mdec_write(int reg, uint32_t v) {
    if (reg == 0) { param(v); return; }
    if (v & 0x80000000u) { g_remaining = 0; g_in.clear(); g_out.clear(); g_out_pos = 0; g_status_bits = 0; }
    g_en_in = (v >> 30) & 1;
    g_en_out = (v >> 29) & 1;
}

uint32_t mdec_read(int reg) {
    if (reg == 0) return mdec_dma_out();
    uint32_t s = 0;
    bool empty = g_out_pos >= g_out.size();
    if (empty) s |= 0x80000000u;
    if (g_remaining > 0) s |= 0x20000000u;
    if (g_en_in && g_remaining > 0) s |= 0x10000000u;
    if (g_en_out && !empty) s |= 0x08000000u;
    s |= g_status_bits;
    s |= (uint32_t)((g_remaining - 1) & 0xffff);
    s |= 4u << 16;
    return s;
}

void mdec_dma_in(uint32_t w) { param(w); }
uint32_t mdec_dma_out() { return g_out_pos < g_out.size() ? g_out[g_out_pos++] : 0; }
bool mdec_out_ready() { return g_out_pos < g_out.size(); }

void mdec_init() {
    for (int i = 0; i < 64; i++) kZagzig[kZigzag[i]] = (uint8_t)i;
}
