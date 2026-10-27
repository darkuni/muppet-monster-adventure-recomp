#include "discpack.h"
#include "miniz.h"
#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <mutex>
#include <thread>
#include <vector>

namespace fs = std::filesystem;

static const char kMagic[8] = {'M', 'M', 'A', 'P', 'A', 'C', 'K', 0};
static const uint32_t kVersion = 1;
static const int kBlock = 16;            // sectors per compressed block
static const int kHeader = 64;

enum : uint8_t { K_FORM1 = 0, K_FORM2 = 1, K_RAW = 2, K_AUDIO = 3, K_ZERO = 4 };
static int payload_size(uint8_t k) {
    switch (k) {
        case K_FORM1: return 4 + 2048;
        case K_FORM2: return 4 + 2324 + 4;
        case K_RAW: case K_AUDIO: return 2352;
        default: return 0;
    }
}

// ---------------------------------------------------------------- EDC / ECC (CD-ROM, ECMA-130)
static uint8_t ecc_f[256], ecc_b[256];
static uint32_t edc_lut[256];
static void tables_init() {
    static bool done = false;
    if (done) return;
    done = true;
    for (uint32_t i = 0; i < 256; i++) {
        uint32_t j = (i << 1) ^ (i & 0x80 ? 0x11d : 0);
        ecc_f[i] = (uint8_t)j;
        ecc_b[i ^ j] = (uint8_t)i;
        uint32_t e = i;
        for (int k = 0; k < 8; k++) e = (e >> 1) ^ (e & 1 ? 0xd8018001u : 0);
        edc_lut[i] = e;
    }
}
static uint32_t edc(const uint8_t* p, size_t n) {
    uint32_t e = 0;
    while (n--) e = (e >> 8) ^ edc_lut[(e ^ *p++) & 0xff];
    return e;
}
static void put32(uint8_t* p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24); }
static uint32_t get32(const uint8_t* p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }
static void ecc_block(const uint8_t* src, int major_n, int minor_n, int major_mult, int minor_inc, uint8_t* dst) {
    int size = major_n * minor_n;
    for (int major = 0; major < major_n; major++) {
        int idx = (major >> 1) * major_mult + (major & 1);
        uint8_t a = 0, b = 0;
        for (int minor = 0; minor < minor_n; minor++) {
            uint8_t t = src[idx];
            idx += minor_inc;
            if (idx >= size) idx -= size;
            a ^= t;
            b ^= t;
            a = ecc_f[a];
        }
        a = ecc_b[ecc_f[a] ^ b];
        dst[major] = a;
        dst[major + major_n] = a ^ b;
    }
}
// Mode 2 Form 1: P and Q parity are computed with the 4 header bytes taken as zero
static void ecc_mode2(uint8_t* s) {
    uint8_t save[4];
    memcpy(save, s + 12, 4);
    memset(s + 12, 0, 4);
    ecc_block(s + 0xc, 86, 24, 2, 86, s + 0x81c);
    ecc_block(s + 0xc, 52, 43, 86, 88, s + 0x8c8);
    memcpy(s + 12, save, 4);
}

static uint8_t bcd(int v) { return (uint8_t)(((v / 10) << 4) | (v % 10)); }
static void sync_header(uint8_t* s, int lba) {
    s[0] = 0;
    memset(s + 1, 0xff, 10);
    s[11] = 0;
    int a = lba + 150;
    s[12] = bcd(a / 4500);
    s[13] = bcd((a / 75) % 60);
    s[14] = bcd(a % 75);
    s[15] = 2;
}
static void decode_sector(uint8_t k, const uint8_t* p, int lba, uint8_t* s) {
    switch (k) {
        case K_FORM1:
            sync_header(s, lba);
            memcpy(s + 16, p, 4);
            memcpy(s + 20, p, 4);
            memcpy(s + 24, p + 4, 2048);
            put32(s + 0x818, edc(s + 16, 0x808));
            ecc_mode2(s);
            break;
        case K_FORM2:
            sync_header(s, lba);
            memcpy(s + 16, p, 4);
            memcpy(s + 20, p, 4);
            memcpy(s + 24, p + 4, 2324 + 4);
            break;
        case K_RAW: case K_AUDIO:
            memcpy(s, p, 2352);
            break;
        default:
            memset(s, 0, 2352);
    }
}
// Picks the smallest form that rebuilds the sector exactly
static uint8_t encode_sector(const uint8_t* s, bool audio, int lba, uint8_t* p) {
    static const uint8_t zero[2352] = {};
    if (!memcmp(s, zero, 2352)) return K_ZERO;
    if (audio) { memcpy(p, s, 2352); return K_AUDIO; }
    uint8_t k = (s[18] & 0x20) ? K_FORM2 : K_FORM1;
    if (k == K_FORM1) { memcpy(p, s + 16, 4); memcpy(p + 4, s + 24, 2048); }
    else { memcpy(p, s + 16, 4); memcpy(p + 4, s + 24, 2324 + 4); }
    uint8_t chk[2352];
    decode_sector(k, p, lba, chk);
    if (!memcmp(chk, s, 2352)) return k;
    memcpy(p, s, 2352);
    return K_RAW;
}

// ---------------------------------------------------------------- building
struct Built { std::vector<uint8_t> z; uint32_t usize = 0, crc = 0; bool ok = true; };

bool pack_build(const std::string& out_path, const DiscLayout& lay, RawReader read, PackProgress progress, std::string* err) {
    tables_init();
    const int total = lay.t2_start + lay.t2_sectors;
    const int nblocks = (total + kBlock - 1) / kBlock;
    std::string tmp = out_path + ".part";
    FILE* f = fopen(tmp.c_str(), "wb");
    if (!f) { *err = "Cannot create " + out_path; return false; }
    uint8_t hdr[kHeader] = {};
    fwrite(hdr, 1, kHeader, f);

    std::vector<uint64_t> offs(nblocks);
    std::vector<uint32_t> csz(nblocks), usz(nblocks), crcs(nblocks);
    uint64_t pos = kHeader;
    int nthreads = std::max(1u, std::min(16u, std::thread::hardware_concurrency()));
    const int chunk = 256;   // blocks read, then compressed in parallel
    std::vector<uint8_t> raw((size_t)chunk * kBlock * 2352);
    std::vector<uint8_t> audio((size_t)chunk * kBlock);
    bool failed = false;
    for (int b0 = 0; b0 < nblocks && !failed; b0 += chunk) {
        int nb = std::min(chunk, nblocks - b0);
        for (int i = 0; i < nb * kBlock; i++) {
            int lba = b0 * kBlock + i;
            bool a = lba >= lay.t1_sectors;
            if (lba >= total) memset(&raw[(size_t)i * 2352], 0, 2352);
            else if (!read(lba, &raw[(size_t)i * 2352], &a) && lba < total) { *err = "Read error in the disc image at sector " + std::to_string(lba); failed = true; break; }
            audio[i] = a;
        }
        if (failed) break;
        std::vector<Built> out(nb);
        std::atomic<int> next{0};
        auto work = [&] {
            std::vector<uint8_t> u(kBlock + kBlock * 2352), chk(u.size());
            int j;
            while ((j = next++) < nb) {
                size_t n = kBlock;
                for (int i = 0; i < kBlock; i++) {
                    int lba = (b0 + j) * kBlock + i;
                    uint8_t k = encode_sector(&raw[(size_t)(j * kBlock + i) * 2352], audio[j * kBlock + i], lba, &u[n]);
                    u[i] = k;
                    n += payload_size(k);
                }
                Built& r = out[j];
                mz_ulong zl = mz_compressBound((mz_ulong)n);
                r.z.resize(zl);
                if (mz_compress2(r.z.data(), &zl, u.data(), (mz_ulong)n, 6) != MZ_OK) { r.ok = false; continue; }
                r.z.resize(zl);
                r.usize = (uint32_t)n;
                r.crc = (uint32_t)mz_crc32(MZ_CRC32_INIT, u.data(), n);
                // round trip: decompress and rebuild every sector
                mz_ulong ul = (mz_ulong)chk.size();
                if (mz_uncompress(chk.data(), &ul, r.z.data(), zl) != MZ_OK || ul != n || memcmp(chk.data(), u.data(), n)) { r.ok = false; continue; }
                size_t q = kBlock;
                for (int i = 0; i < kBlock; i++) {
                    uint8_t s[2352];
                    decode_sector(chk[i], &chk[q], (b0 + j) * kBlock + i, s);
                    q += payload_size(chk[i]);
                    if (memcmp(s, &raw[(size_t)(j * kBlock + i) * 2352], 2352)) r.ok = false;
                }
            }
        };
        std::vector<std::thread> th;
        for (int t = 1; t < nthreads; t++) th.emplace_back(work);
        work();
        for (auto& t : th) t.join();
        for (int j = 0; j < nb; j++) {
            if (!out[j].ok) { *err = "Verification failed while packing block " + std::to_string(b0 + j); failed = true; break; }
            offs[b0 + j] = pos;
            csz[b0 + j] = (uint32_t)out[j].z.size();
            usz[b0 + j] = out[j].usize;
            crcs[b0 + j] = out[j].crc;
            if (fwrite(out[j].z.data(), 1, out[j].z.size(), f) != out[j].z.size()) { *err = "Write error (disk full?)"; failed = true; break; }
            pos += out[j].z.size();
        }
        if (progress) progress(std::min(nblocks, b0 + nb), nblocks);
    }
    if (!failed) {
        std::vector<uint8_t> idx((size_t)nblocks * 20);
        for (int i = 0; i < nblocks; i++) {
            put32(&idx[i * 20], (uint32_t)offs[i]);
            put32(&idx[i * 20 + 4], (uint32_t)(offs[i] >> 32));
            put32(&idx[i * 20 + 8], csz[i]);
            put32(&idx[i * 20 + 12], usz[i]);
            put32(&idx[i * 20 + 16], crcs[i]);
        }
        fwrite(idx.data(), 1, idx.size(), f);
        memcpy(hdr, kMagic, 8);
        put32(hdr + 8, kVersion);
        put32(hdr + 12, lay.t1_sectors);
        put32(hdr + 16, lay.t2_start);
        put32(hdr + 20, lay.t2_sectors);
        put32(hdr + 24, total);
        put32(hdr + 28, kBlock);
        put32(hdr + 32, nblocks);
        put32(hdr + 36, (uint32_t)pos);
        put32(hdr + 40, (uint32_t)(pos >> 32));
        put32(hdr + 44, (uint32_t)mz_crc32(MZ_CRC32_INIT, idx.data(), idx.size()));
        fseek(f, 0, SEEK_SET);
        fwrite(hdr, 1, kHeader, f);
        if (fflush(f) != 0) { *err = "Write error (disk full?)"; failed = true; }
    }
    fclose(f);
    std::error_code ec;
    if (failed) { fs::remove(tmp, ec); return false; }
    fs::rename(tmp, out_path, ec);
    if (ec) { *err = "Cannot rename " + tmp + ": " + ec.message(); fs::remove(tmp, ec); return false; }
    return true;
}

// ---------------------------------------------------------------- reading
static FILE* g_pf = nullptr;
static int g_total = 0, g_nblocks = 0;
static std::vector<uint8_t> g_index;
static std::vector<uint8_t> g_cache, g_zbuf;
static int g_cached = -1;
static std::mutex g_mu;

bool pack_open(const std::string& path, DiscLayout* lay, std::string* err) {
    tables_init();
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) { *err = "Cannot open " + path; return false; }
    uint8_t h[kHeader];
    if (fread(h, 1, kHeader, f) != kHeader || memcmp(h, kMagic, 8) || get32(h + 8) != kVersion || get32(h + 28) != (uint32_t)kBlock) {
        fclose(f);
        *err = path + " is not a packed disc file made by this version.";
        return false;
    }
    int nb = (int)get32(h + 32);
    uint64_t io = get32(h + 36) | (uint64_t)get32(h + 40) << 32;
    std::vector<uint8_t> idx((size_t)nb * 20);
    if (fseek(f, (long)io, SEEK_SET) || fread(idx.data(), 1, idx.size(), f) != idx.size() ||
        (uint32_t)mz_crc32(MZ_CRC32_INIT, idx.data(), idx.size()) != get32(h + 44)) {
        fclose(f);
        *err = path + " is damaged or incomplete. Delete it and import the disc again.";
        return false;
    }
    lay->t1_sectors = (int)get32(h + 12);
    lay->t2_start = (int)get32(h + 16);
    lay->t2_sectors = (int)get32(h + 20);
    g_total = (int)get32(h + 24);
    g_nblocks = nb;
    g_index = std::move(idx);
    g_cache.resize(kBlock + kBlock * 2352);
    g_pf = f;
    return true;
}

bool pack_read(int lba, uint8_t* out) {
    std::lock_guard<std::mutex> lk(g_mu);
    if (!g_pf || lba < 0 || lba >= g_total) { memset(out, 0, 2352); return false; }
    int b = lba / kBlock;
    if (b != g_cached) {
        const uint8_t* e = &g_index[(size_t)b * 20];
        uint64_t off = get32(e) | (uint64_t)get32(e + 4) << 32;
        uint32_t cs = get32(e + 8), us = get32(e + 12), crc = get32(e + 16);
        g_zbuf.resize(cs);
        mz_ulong ul = (mz_ulong)g_cache.size();
        if (fseek(g_pf, (long)off, SEEK_SET) || fread(g_zbuf.data(), 1, cs, g_pf) != cs ||
            mz_uncompress(g_cache.data(), &ul, g_zbuf.data(), cs) != MZ_OK || ul != us ||
            (uint32_t)mz_crc32(MZ_CRC32_INIT, g_cache.data(), ul) != crc) {
            fprintf(stderr, "[pack] block %d is damaged\n", b);
            g_cached = -1;
            memset(out, 0, 2352);
            return false;
        }
        g_cached = b;
    }
    int i = lba % kBlock;
    size_t q = kBlock;
    for (int k = 0; k < i; k++) q += payload_size(g_cache[k]);
    decode_sector(g_cache[i], &g_cache[q], lba, out);
    return true;
}
