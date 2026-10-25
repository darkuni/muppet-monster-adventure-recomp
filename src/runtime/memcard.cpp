// Memory cards as standard 128 KB raw images (memcard1.mcd / memcard2.mcd next to the
// executable), so saves can be moved to and from emulators. Implements the sector access and
// the BIOS file system (bu00:/bu10:) on top of the card's directory frames.
#include "bios.h"
#include "log.h"
#include <cstdio>
#include <algorithm>
#include <cstring>
#include <string>
#include <vector>

static const int CARD_SIZE = 128 * 1024, FRAME = 128, BLOCK = 8192;
struct Card { std::string path; std::vector<uint8_t> data; bool present = false; };
static Card g_cards[2];

static void frame_checksum(uint8_t* f) {
    uint8_t x = 0;
    for (int i = 0; i < 127; i++) x ^= f[i];
    f[127] = x;
}

static void card_format(Card& c) {
    c.data.assign(CARD_SIZE, 0);
    uint8_t* d = c.data.data();
    d[0] = 'M'; d[1] = 'C';
    frame_checksum(d);
    for (int i = 1; i < 16; i++) {
        uint8_t* f = d + i * FRAME;
        f[0] = 0xa0;
        f[8] = 0xff; f[9] = 0xff;
        frame_checksum(f);
    }
    for (int i = 16; i < 36; i++) {
        uint8_t* f = d + i * FRAME;
        memset(f, 0xff, 4);
        f[8] = 0xff; f[9] = 0xff;
        frame_checksum(f);
    }
    memcpy(d + 63 * FRAME, d, FRAME);
}

static void card_flush(int slot) {
    Card& c = g_cards[slot];
    if (!c.present) return;
    FILE* f = fopen(c.path.c_str(), "wb");
    if (!f) { fprintf(stderr, "[card] cannot write %s\n", c.path.c_str()); return; }
    fwrite(c.data.data(), 1, c.data.size(), f);
    fclose(f);
}

void memcard_init(const char* dir) {
    for (int s = 0; s < 2; s++) {
        Card& c = g_cards[s];
        c.path = std::string(dir) + (s ? "memcard2.mcd" : "memcard1.mcd");
        FILE* f = fopen(c.path.c_str(), "rb");
        if (f) {
            c.data.assign(CARD_SIZE, 0);
            size_t n = fread(c.data.data(), 1, CARD_SIZE, f);
            fclose(f);
            c.present = n == (size_t)CARD_SIZE && c.data[0] == 'M' && c.data[1] == 'C';
            if (!c.present) fprintf(stderr, "[card] %s is not a 128 KB memory card image; ignoring it\n", c.path.c_str());
        } else if (s == 0) {
            // slot 1 always has a card; it is created on first use
            card_format(c);
            c.present = true;
            card_flush(0);
        }
        if (c.present) fprintf(stderr, "[card] slot %d: %s\n", s + 1, c.path.c_str());
    }
}

bool memcard_present(int slot) { return g_cards[slot & 1].present; }

bool memcard_read_sector(int slot, int sector, uint8_t* dst) {
    Card& c = g_cards[slot & 1];
    if (!c.present || sector < 0 || sector >= 1024) return false;
    memcpy(dst, c.data.data() + sector * FRAME, FRAME);
    return true;
}

bool memcard_write_sector(int slot, int sector, const uint8_t* src) {
    Card& c = g_cards[slot & 1];
    if (!c.present || sector < 0 || sector >= 1024) return false;
    memcpy(c.data.data() + sector * FRAME, src, FRAME);
    card_flush(slot & 1);
    return true;
}

// ---------------------------------------------------------------- file system
static uint8_t* dir_entry(Card& c, int i) { return c.data.data() + i * FRAME; }
static uint32_t rd32(const uint8_t* p) { uint32_t v; memcpy(&v, p, 4); return v; }
static void wr32(uint8_t* p, uint32_t v) { memcpy(p, &v, 4); }

static bool wild_match(const char* pat, const char* s) {
    if (!*pat) return !*s;
    if (*pat == '*') { for (;; s++) { if (wild_match(pat + 1, s)) return true; if (!*s) return false; } }
    if (!*s) return false;
    if (*pat == '?' || *pat == *s) return wild_match(pat + 1, s + 1);
    return false;
}

static int find_file(Card& c, const char* name) {
    for (int i = 1; i < 16; i++) {
        uint8_t* e = dir_entry(c, i);
        if (e[0] == 0x51 && strncmp((const char*)e + 10, name, 20) == 0) return i;
    }
    return -1;
}

struct Handle { bool used = false; int slot = 0, first = 0, blocks = 0, pos = 0; };
static Handle g_h[8];

static int chain_block(Card& c, int first, int n) {
    int b = first;
    for (int i = 0; i < n && b > 0 && b < 16; i++) {
        uint16_t next;
        memcpy(&next, dir_entry(c, b) + 8, 2);
        b = next == 0xffff ? -1 : next + 1;
    }
    return b;
}

int memcard_open(int slot, const char* name, uint32_t mode) {
    Card& c = g_cards[slot & 1];
    if (!c.present) return -1;
    int i = find_file(c, name);
    if (mode & 0x200) {   // create
        if (i >= 0) return -1;
        int blocks = (int)(mode >> 16);
        if (blocks < 1) blocks = 1;
        std::vector<int> fr;
        for (int b = 1; b < 16 && (int)fr.size() < blocks; b++) {
            uint8_t s = dir_entry(c, b)[0];
            if (s == 0xa0 || s == 0xa1 || s == 0xa2 || s == 0xa3) fr.push_back(b);
        }
        if ((int)fr.size() < blocks) return -1;
        for (size_t k = 0; k < fr.size(); k++) {
            uint8_t* e = dir_entry(c, fr[k]);
            memset(e, 0, FRAME);
            e[0] = k == 0 ? 0x51 : (k + 1 == fr.size() ? 0x53 : 0x52);
            if (fr.size() == 1) e[0] = 0x51;
            if (k == 0) { wr32(e + 4, (uint32_t)(blocks * BLOCK)); strncpy((char*)e + 10, name, 20); }
            uint16_t next = k + 1 < fr.size() ? (uint16_t)(fr[k + 1] - 1) : 0xffff;
            memcpy(e + 8, &next, 2);
            frame_checksum(e);
            memset(c.data.data() + fr[k] * BLOCK, 0, BLOCK);
        }
        card_flush(slot & 1);
        i = fr[0];
        LOG("card", "created %s (%d blocks)", name, blocks);
    }
    if (i < 0) return -1;
    for (int h = 0; h < 8; h++) {
        if (g_h[h].used) continue;
        g_h[h] = Handle{true, slot & 1, i, (int)(rd32(dir_entry(c, i) + 4) / BLOCK), 0};
        return h + 2;   // BIOS file handles start above stdin/stdout
    }
    return -1;
}

int memcard_rw(int fh, uint8_t* buf, int len, bool write) {
    int h = fh - 2;
    if (h < 0 || h >= 8 || !g_h[h].used) return -1;
    Handle& f = g_h[h];
    Card& c = g_cards[f.slot];
    int done = 0;
    while (done < len) {
        int bi = f.pos / BLOCK, off = f.pos % BLOCK;
        if (bi >= f.blocks) break;
        int b = chain_block(c, f.first, bi);
        if (b < 1) break;
        int n = std::min(len - done, BLOCK - off);
        uint8_t* p = c.data.data() + b * BLOCK + off;
        if (write) memcpy(p, buf + done, n); else memcpy(buf + done, p, n);
        done += n;
        f.pos += n;
    }
    if (write && done) card_flush(f.slot);
    return done;
}

int memcard_seek(int fh, int off, int whence) {
    int h = fh - 2;
    if (h < 0 || h >= 8 || !g_h[h].used) return -1;
    if (whence == 0) g_h[h].pos = off;
    else if (whence == 1) g_h[h].pos += off;
    return g_h[h].pos;
}

void memcard_close(int fh) {
    int h = fh - 2;
    if (h >= 0 && h < 8) g_h[h].used = false;
}

bool memcard_find(int slot, const char* pattern, int start, char* name, int* size, int* index) {
    Card& c = g_cards[slot & 1];
    if (!c.present) return false;
    for (int i = std::max(1, start); i < 16; i++) {
        uint8_t* e = dir_entry(c, i);
        if (e[0] != 0x51) continue;
        char n[21] = {};
        memcpy(n, e + 10, 20);
        if (!wild_match(pattern, n)) continue;
        strcpy(name, n);
        *size = (int)rd32(e + 4);
        *index = i;
        return true;
    }
    return false;
}

bool memcard_erase(int slot, const char* name) {
    Card& c = g_cards[slot & 1];
    int i = find_file(c, name);
    if (i < 0) return false;
    for (int b = i, guard = 0; b > 0 && b < 16 && guard < 16; guard++) {
        uint8_t* e = dir_entry(c, b);
        uint16_t next;
        memcpy(&next, e + 8, 2);
        e[0] = (uint8_t)((e[0] & 0x0f) | 0xa0);
        frame_checksum(e);
        b = next == 0xffff ? -1 : next + 1;
    }
    card_flush(slot & 1);
    return true;
}
