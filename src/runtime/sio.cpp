// SIO0 (controller port) with a DualShock controller in port 1: the byte-level protocol
// libpad speaks (reads, config mode, analog/LED, rumble mapping) and the /ACK interrupt.
// Memory cards are handled at BIOS level (bios.cpp/memcard.cpp), so port addressing
// for cards gets no answer here.
#include <cstdlib>
#include "hw.h"
#include "log.h"
#include "config.h"
#include "hostinput.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <deque>

void platform_rumble(int small_on, int large);

static uint16_t g_mode = 0, g_ctrl = 0, g_baud = 0x88;
static uint32_t g_stat = 0x5;   // TX ready, TX idle
static std::deque<uint8_t> g_rx;
static uint8_t g_tx_byte = 0;
static bool g_pending_ack = false;

// ---------------------------------------------------------------- DualShock
struct Pad {
    int pos = 0;               // byte index within the current transfer (0 = address)
    bool selected = false;     // addressed us this transfer
    uint8_t cmd = 0;
    bool config = false;
    bool analog = false;
    bool locked = false;
    bool enter_config = false, exit_config = false;
    uint8_t map[6] = {0xff, 0xff, 0xff, 0xff, 0xff, 0xff};
    uint8_t new_map[6];
    uint8_t reply[10];
    int len = 0;
    uint8_t motor_small = 0, motor_large = 0;
};
static Pad g_pad;

static uint16_t buttons() {
    uint32_t b = g_host_input.buttons;
    uint16_t v = 0;
    if (b & IN_BACK) v |= 1 << 0;
    if (b & IN_START) v |= 1 << 3;
    if (b & IN_UP) v |= 1 << 4;
    if (b & IN_RIGHT) v |= 1 << 5;
    if (b & IN_DOWN) v |= 1 << 6;
    if (b & IN_LEFT) v |= 1 << 7;
    if (b & IN_L2) v |= 1 << 8;
    if (b & IN_R2) v |= 1 << 9;
    if (b & IN_L1) v |= 1 << 10;
    if (b & IN_R1) v |= 1 << 11;
    if (b & IN_NORTH) v |= 1 << 12;
    if (b & IN_EAST) v |= 1 << 13;
    if (b & IN_SOUTH) v |= 1 << 14;
    if (b & IN_WEST) v |= 1 << 15;
    if (b & IN_L3) v |= 1 << 1;
    if (b & IN_R3) v |= 1 << 2;
    // Without analog mode the game only sees the d-pad: let the left stick drive it.
    if (!g_pad.analog) {
        if (g_host_input.ly > 0.5f) v |= 1 << 4;
        if (g_host_input.ly < -0.5f) v |= 1 << 6;
        if (g_host_input.lx > 0.5f) v |= 1 << 5;
        if (g_host_input.lx < -0.5f) v |= 1 << 7;
    }
    return (uint16_t)~v;
}
uint16_t sio_pad_buttons() { return buttons(); }   // active low, for the BIOS pad driver

static uint8_t axis(float v, bool invert) {
    if (invert) v = -v;
    int a = (int)std::lround(128 + v * 127.5f);
    return (uint8_t)std::clamp(a, 0, 255);
}

static void build_reply(Pad& p) {
    memset(p.reply, 0, sizeof p.reply);
    uint16_t b = buttons();
    if (p.config) {
        p.len = 9;
        switch (p.cmd) {
            case 0x42:
                p.reply[3] = b & 0xff; p.reply[4] = b >> 8;
                p.reply[5] = axis(g_host_input.rx, false); p.reply[6] = axis(g_host_input.ry, true);
                p.reply[7] = axis(g_host_input.lx, false); p.reply[8] = axis(g_host_input.ly, true);
                break;
            case 0x45: { uint8_t r[6] = {0x01, 0x02, (uint8_t)(p.analog ? 1 : 0), 0x02, 0x01, 0x00}; memcpy(p.reply + 3, r, 6); break; }
            case 0x47: { uint8_t r[6] = {0, 0, 2, 0, 1, 0}; memcpy(p.reply + 3, r, 6); break; }
            case 0x4d: memcpy(p.reply + 3, p.map, 6); break;
            default: break;   // 43, 44, 46, 4c filled in as parameters arrive; others zero
        }
    } else {
        p.len = p.analog ? 9 : 5;
        p.reply[3] = b & 0xff; p.reply[4] = b >> 8;
        if (p.analog) {
            p.reply[5] = axis(g_host_input.rx, false); p.reply[6] = axis(g_host_input.ry, true);
            p.reply[7] = axis(g_host_input.lx, false); p.reply[8] = axis(g_host_input.ly, true);
        }
    }
    p.reply[0] = 0xff;
    p.reply[1] = p.config ? 0xf3 : (p.analog ? 0x73 : 0x41);
    p.reply[2] = 0x5a;
}

// returns the byte the pad sends back for `in`; *ack = pad pulls /ACK (more bytes follow)
static uint8_t pad_exchange(uint8_t in, bool* ack) {
    Pad& p = g_pad;
    *ack = false;
    if (p.pos == 0) {
        p.selected = in == 0x01;
        p.pos++;
        *ack = p.selected;
        return 0xff;
    }
    if (!p.selected) return 0xff;
    int i = p.pos++;
    if (i == 1) {
        p.cmd = in;
        p.enter_config = p.exit_config = false;
        if (p.cmd == 0x4d) memcpy(p.new_map, p.map, 6);
        build_reply(p);
        if (!p.config && p.cmd != 0x42 && p.cmd != 0x43) { p.selected = false; return 0xff; }
        *ack = true;
        return p.reply[1];
    }
    // parameter bytes
    if (p.config) {
        switch (p.cmd) {
            case 0x43: if (i == 3) p.exit_config = in == 0x00; break;
            case 0x44:
                if (i == 3 && in <= 1) p.analog = in == 1 && !getenv("PAD_DIGITAL");
                if (i == 4) p.locked = (in & 3) == 3;
                break;
            case 0x46:
                if (i == 3) {
                    uint8_t r0[4] = {0x01, 0x02, 0x00, 0x0a}, r1[4] = {0x01, 0x01, 0x01, 0x14};
                    if (in == 0) memcpy(p.reply + 5, r0, 4);
                    else if (in == 1) memcpy(p.reply + 5, r1, 4);
                }
                break;
            case 0x4c: if (i == 3) p.reply[6] = in == 0 ? 0x04 : in == 1 ? 0x07 : 0x00; break;
            case 0x4d: if (i >= 3 && i <= 8) p.new_map[i - 3] = in; break;
            default: break;
        }
    } else {
        if (p.cmd == 0x43 && i == 3) p.enter_config = in == 0x01;
        if (p.cmd == 0x42 && i >= 3 && i <= 8) {
            // rumble: bytes mapped by command 4Dh (or the old single-motor method)
            int k = i - 3;
            if (p.map[k] == 0x00) p.motor_small = in & 1;
            else if (p.map[k] == 0x01) p.motor_large = in;
            if (k == 1 && p.map[0] == 0xff && p.map[1] == 0xff) p.motor_small = (in & 1) ? p.motor_small : p.motor_small;
        }
    }
    uint8_t out = i < 10 ? p.reply[i] : 0;
    if (i + 1 < p.len) *ack = true;
    else {
        // end of transfer
        if (p.enter_config) p.config = true;
        if (p.exit_config) p.config = false;
        if (p.cmd == 0x4d) memcpy(p.map, p.new_map, 6);
        if (p.cmd == 0x42) platform_rumble(p.motor_small, p.motor_large);
    }
    return out;
}

// ---------------------------------------------------------------- SIO0 registers
static int64_t byte_cycles() {
    int factor = (g_mode & 3) == 2 ? 16 : (g_mode & 3) == 3 ? 64 : 1;
    int64_t bit = std::max(1, (g_baud * factor) & ~1);
    return bit * 8;
}

void sio_event() {
    sched_cancel(EV_SIO0);
    if (g_pending_ack) {
        // the device pulled /ACK
        g_pending_ack = false;
        g_stat |= 0x80;
        if (g_ctrl & 0x1000) { g_stat |= 0x200; irq_raise(IRQ_SIO0); }
        return;
    }
}

static void transfer(uint8_t v) {
    bool ack = false;
    uint8_t rx = 0xff;
    if ((g_ctrl & 2) && !(g_ctrl & 0x2000)) rx = pad_exchange(v, &ack);   // port 1 only
    g_rx.push_back(rx);
    if (g_rx.size() > 8) g_rx.pop_front();
    g_stat |= 0x7;
    g_stat &= ~0x80u;
    if (ack) {
        g_pending_ack = true;
        sched_in(EV_SIO0, byte_cycles() + 170);
    }
    LOG("sio", "tx %02x rx %02x%s", v, rx, ack ? " ack" : "");
}

uint32_t sio_read(uint32_t off, int size) {
    if (off >= 0x50) {   // SIO1: nothing connected
        if ((off & 0xf) == 4) return 0x5;
        return 0;
    }
    switch (off & 0xf) {
        case 0x0: {
            uint32_t v = 0xff;
            if (!g_rx.empty()) { v = g_rx.front(); g_rx.pop_front(); }
            if (g_rx.empty()) g_stat &= ~2u;
            if (size > 1) v |= 0xffffff00u & (0xffu << 8);
            return v;
        }
        case 0x4: {
            uint32_t s = g_stat;
            if (!g_rx.empty()) s |= 2;
            return s;
        }
        case 0x8: return g_mode;
        case 0xa: return g_ctrl;
        case 0xe: return g_baud;
    }
    return 0;
}

void sio_write(uint32_t off, uint32_t v, int size) {
    if (off >= 0x50) return;
    switch (off & 0xf) {
        case 0x0: g_tx_byte = (uint8_t)v; transfer(g_tx_byte); break;
        case 0x8: g_mode = (uint16_t)v; break;
        case 0xa:
            if (v & 0x40) {   // reset
                g_ctrl = 0; g_stat = 0x5; g_rx.clear(); g_pad.pos = 0; g_pending_ack = false;
                sched_cancel(EV_SIO0);
                return;
            }
            if (v & 0x10) { g_stat &= ~(0x200u | 0x8u); }   // acknowledge
            if (!(v & 2) || ((v ^ g_ctrl) & 0x2000)) { g_pad.pos = 0; g_pad.selected = false; }   // /CS released
            g_ctrl = (uint16_t)(v & ~0x50u);
            if (!(g_ctrl & 2)) g_stat &= ~0x80u;
            break;
        case 0xe: g_baud = (uint16_t)v; break;
    }
    (void)size;
}

void sio_init() {}
