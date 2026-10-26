// GPU command processor: GP0 drawing/transfer commands, GP1 display control, GPUSTAT/GPUREAD,
// following psx-spx. Rendering itself is done by gpu_gl.cpp.
#include "gpu.h"
#include "hw.h"
#include "log.h"
#include <algorithm>
#include <cstring>
#include <vector>

extern int g_game_frames;
int64_t hw_vblank_time();

static std::vector<uint32_t> g_fifo;
static size_t g_need = 0;          // words needed for the current command (0 = waiting for a command)
static bool g_polyline = false;

// GP0 state
static uint32_t g_drawmode = 0;    // E1 bits 0-13
static uint32_t g_texwin = 0;      // E2
static int g_area_x1 = 0, g_area_y1 = 0, g_area_x2 = 0, g_area_y2 = 0;
static int g_off_x = 0, g_off_y = 0;
static bool g_set_mask = false, g_check_mask = false;
static bool g_irq = false;

// GP1 state
static bool g_display_off = true;
static uint32_t g_dma_dir = 0;
static int g_disp_x = 0, g_disp_y = 0;
static uint32_t g_hrange = 0xc60260, g_vrange = 0x3fc10;
static uint32_t g_dispmode = 0;
static uint32_t g_gpuread = 0;
static uint32_t g_info_latch = 0;

// VRAM transfers
static int g_up_x, g_up_y, g_up_w, g_up_h;
static size_t g_up_remaining = 0;      // halfwords still to receive
static std::vector<uint16_t> g_up_buf;
static std::vector<uint16_t> g_down_buf;
static size_t g_down_pos = 0;
static bool g_down_active = false;

static inline int sx11(int v) { return (int)((uint32_t)v << 21) >> 21; }

static void fill_state(GfxState& s, bool textured, bool raw, bool semi) {
    s.textured = textured;
    s.raw = raw;
    s.semi = semi;
    s.tpage_x = (g_drawmode & 0xf) * 64;
    s.tpage_y = ((g_drawmode >> 4) & 1) * 256;
    s.blend = (g_drawmode >> 5) & 3;
    s.tex_mode = std::min(2u, (g_drawmode >> 7) & 3);
    s.dither = (g_drawmode >> 9) & 1;
    s.win_mask_x = g_texwin & 0x1f;
    s.win_mask_y = (g_texwin >> 5) & 0x1f;
    s.win_off_x = (g_texwin >> 10) & 0x1f;
    s.win_off_y = (g_texwin >> 15) & 0x1f;
    s.set_mask = g_set_mask;
    s.check_mask = g_check_mask;
    s.clip_x1 = g_area_x1; s.clip_y1 = g_area_y1;
    s.clip_x2 = g_area_x2; s.clip_y2 = g_area_y2;
}

static void vertex(uint32_t w, GfxVertex& v) {
    v.x = (float)sx11(sx11((int)(w & 0x7ff)) + g_off_x);
    v.y = (float)sx11(sx11((int)((w >> 16) & 0x7ff)) + g_off_y);
}

static bool too_big(const GfxVertex* v, int n) {
    for (int i = 0; i < n; i++)
        for (int j = i + 1; j < n; j++)
            if (std::abs(v[i].x - v[j].x) > 1023 || std::abs(v[i].y - v[j].y) > 511) return true;
    return false;
}

static void draw_polygon(const uint32_t* c) {
    uint32_t op = c[0] >> 24;
    bool gouraud = op & 0x10, quad = op & 0x08, textured = op & 0x04, semi = op & 0x02, raw = op & 0x01;
    int n = quad ? 4 : 3;
    GfxVertex v[4];
    size_t i = 1;
    uint32_t clut = 0, page = 0;
    for (int k = 0; k < n; k++) {
        v[k].color = (k == 0 || !gouraud) ? (c[0] & 0xffffff) : (c[i++] & 0xffffff);
        vertex(c[i++], v[k]);
        if (textured) {
            uint32_t uv = c[i++];
            v[k].u = (float)(uv & 0xff);
            v[k].v = (float)((uv >> 8) & 0xff);
            if (k == 0) clut = uv >> 16;
            if (k == 1) page = uv >> 16;
        } else {
            v[k].u = v[k].v = 0;
        }
    }
    if (textured) g_drawmode = (g_drawmode & ~0x9ffu) | (page & 0x9ff);
    GfxState s;
    fill_state(s, textured, raw && textured, semi);
    s.clut_x = (clut & 0x3f) * 16;
    s.clut_y = (clut >> 6) & 0x1ff;
    if (!too_big(v, 3)) gfx_triangle(v, s);
    if (quad && !too_big(v + 1, 3)) gfx_triangle(v + 1, s);
}

static void draw_rect(const uint32_t* c) {
    uint32_t op = c[0] >> 24;
    bool textured = op & 0x04, semi = op & 0x02, raw = op & 0x01;
    int size = (op >> 3) & 3;
    size_t i = 1;
    GfxVertex tl;
    vertex(c[i++], tl);
    uint32_t uv = 0;
    if (textured) uv = c[i++];
    int w, h;
    if (size == 0) { uint32_t wh = c[i++]; w = wh & 0x3ff; h = (wh >> 16) & 0x1ff; }
    else if (size == 1) w = h = 1;
    else if (size == 2) w = h = 8;
    else w = h = 16;
    if (w == 0 || h == 0) return;
    GfxState s;
    fill_state(s, textured, raw && textured, semi);
    s.clut_x = ((uv >> 16) & 0x3f) * 16;
    s.clut_y = (uv >> 22) & 0x1ff;
    uint32_t col = c[0] & 0xffffff;
    float u0 = (float)(uv & 0xff), v0 = (float)((uv >> 8) & 0xff);
    bool fx = (g_drawmode >> 12) & 1, fy = (g_drawmode >> 13) & 1;
    float ul = fx ? u0 + 1 : u0, ur = fx ? u0 + 1 - w : u0 + w;
    float vt = fy ? v0 + 1 : v0, vb = fy ? v0 + 1 - h : v0 + h;
    float x0 = tl.x, y0 = tl.y, x1 = tl.x + w, y1 = tl.y + h;
    GfxVertex q[4] = {{x0, y0, col, ul, vt}, {x1, y0, col, ur, vt}, {x0, y1, col, ul, vb}, {x1, y1, col, ur, vb}};
    gfx_triangle(q, s);
    gfx_triangle(q + 1, s);
}

static void draw_line_seg(GfxVertex a, GfxVertex b, const GfxState& s) {
    float dx = b.x - a.x, dy = b.y - a.y;
    if (std::abs(dx) > 1023 || std::abs(dy) > 511) return;
    // expand to a one-pixel-wide quad covering both endpoints
    GfxVertex q[4];
    if (std::abs(dx) >= std::abs(dy)) {
        if (dx < 0) std::swap(a, b);
        q[0] = a; q[1] = b; q[2] = a; q[3] = b;
        q[1].x += 1; q[3].x += 1;
        q[2].y += 1; q[3].y += 1;
    } else {
        if (dy < 0) std::swap(a, b);
        q[0] = a; q[1] = a; q[2] = b; q[3] = b;
        q[1].x += 1; q[3].x += 1;
        q[2].y += 1; q[3].y += 1;
    }
    gfx_triangle(q, s);
    gfx_triangle(q + 1, s);
}

static void draw_line(const uint32_t* c, size_t n) {
    uint32_t op = c[0] >> 24;
    bool gouraud = op & 0x10, semi = op & 0x02;
    GfxState s;
    fill_state(s, false, false, semi);
    std::vector<GfxVertex> pts;
    size_t i = 1;
    uint32_t color = c[0] & 0xffffff;
    bool first = true;
    while (i < n) {
        GfxVertex v{};
        if (!first && gouraud) { if ((c[i] & 0xf000f000u) == 0x50005000u) break; color = c[i++] & 0xffffff; }
        if (i >= n) break;
        if (!first && (c[i] & 0xf000f000u) == 0x50005000u && !gouraud) break;
        v.color = color;
        vertex(c[i++], v);
        pts.push_back(v);
        first = false;
    }
    for (size_t k = 1; k < pts.size(); k++) draw_line_seg(pts[k - 1], pts[k], s);
}

static int g_vbl_count = 0;
static void gp0_command(const uint32_t* c, size_t n) {
    uint32_t op = c[0] >> 24;
    static int dump_at = getenv("GP0_DUMP_AT") ? atoi(getenv("GP0_DUMP_AT")) : -1;
    if (g_vbl_count == dump_at && (op >> 5) >= 1 && (op >> 5) <= 3) {
        fprintf(stderr, "[gp0]");
        for (size_t i = 0; i < n; i++) fprintf(stderr, " %08x", c[i]);
        fprintf(stderr, "\n");
    }
    switch (op >> 5) {
        case 1: draw_polygon(c); return;
        case 2: draw_line(c, n); return;
        case 3: draw_rect(c); return;
        case 4: {   // VRAM -> VRAM
            int sx = c[1] & 0x3ff, sy = (c[1] >> 16) & 0x1ff, dx = c[2] & 0x3ff, dy = (c[2] >> 16) & 0x1ff;
            int w = (((c[3] & 0xffff) - 1) & 0x3ff) + 1, h = ((((c[3] >> 16) & 0xffff) - 1) & 0x1ff) + 1;
            gfx_vram_copy(sx, sy, dx, dy, w, h);
            return;
        }
        case 5: {   // CPU -> VRAM
            g_up_x = c[1] & 0x3ff; g_up_y = (c[1] >> 16) & 0x1ff;
            LOG("upload", "VRAM write %d,%d %08x", g_up_x, g_up_y, c[2]);
            g_up_w = (((c[2] & 0xffff) - 1) & 0x3ff) + 1; g_up_h = ((((c[2] >> 16) & 0xffff) - 1) & 0x1ff) + 1;
            g_up_remaining = (size_t)g_up_w * g_up_h;
            g_up_buf.clear();
            g_up_buf.reserve(g_up_remaining);
            return;
        }
        case 6: {   // VRAM -> CPU
            int x = c[1] & 0x3ff, y = (c[1] >> 16) & 0x1ff;
            int w = (((c[2] & 0xffff) - 1) & 0x3ff) + 1, h = ((((c[2] >> 16) & 0xffff) - 1) & 0x1ff) + 1;
            g_down_buf.assign((size_t)w * h + 1, 0);
            gfx_vram_read(x, y, w, h, g_down_buf.data());
            g_down_pos = 0;
            g_down_active = true;
            LOG("gpu", "VRAM read %d,%d %dx%d", x, y, w, h);
            return;
        }
        default: break;
    }
    switch (op) {
        case 0x02: {   // fill
            uint32_t col = c[0] & 0xffffff;
            int x = c[1] & 0x3f0, y = (c[1] >> 16) & 0x1ff;
            int w = ((c[2] & 0x3ff) + 0xf) & ~0xf, h = (c[2] >> 16) & 0x1ff;
            if (w && h) gfx_fill(x, y, w, h, col);
            return;
        }
        case 0x1f: g_irq = true; irq_raise(IRQ_GPU); return;
        case 0xe1: g_drawmode = c[0] & 0x3fff; return;
        case 0xe2: g_texwin = c[0] & 0xfffff; return;
        case 0xe3: g_area_x1 = c[0] & 0x3ff; g_area_y1 = (c[0] >> 10) & 0x1ff; return;
        case 0xe4: g_area_x2 = c[0] & 0x3ff; g_area_y2 = (c[0] >> 10) & 0x1ff; return;
        case 0xe5: g_off_x = sx11(c[0] & 0x7ff); g_off_y = sx11((c[0] >> 11) & 0x7ff); return;
        case 0xe6: g_set_mask = c[0] & 1; g_check_mask = (c[0] >> 1) & 1; return;
        default: return;   // NOPs, cache clear
    }
}

static size_t command_length(uint32_t w) {
    uint32_t op = w >> 24;
    switch (op >> 5) {
        case 1: {
            bool gouraud = op & 0x10, quad = op & 0x08, textured = op & 0x04;
            int n = quad ? 4 : 3;
            return 1 + n * (1 + (textured ? 1 : 0)) + (gouraud ? n - 1 : 0);
        }
        case 2: {
            bool gouraud = op & 0x10;
            if (op & 0x08) return 0;   // polyline: variable
            return gouraud ? 4 : 3;
        }
        case 3: {
            bool textured = op & 0x04;
            int size = (op >> 3) & 3;
            return 2 + (textured ? 1 : 0) + (size == 0 ? 1 : 0);
        }
        case 4: return 4;
        case 5: case 6: return 3;
        default: break;
    }
    if (op == 0x02) return 3;
    return 1;
}

static void gp0(uint32_t w) {
    if (g_up_remaining) {
        g_up_buf.push_back((uint16_t)w);
        if (--g_up_remaining && true) { g_up_buf.push_back((uint16_t)(w >> 16)); --g_up_remaining; }
        if (!g_up_remaining) {
            g_up_buf.resize((size_t)g_up_w * g_up_h);
            gfx_vram_write(g_up_x, g_up_y, g_up_w, g_up_h, g_up_buf.data(), g_set_mask, g_check_mask);
        }
        return;
    }
    if (g_fifo.empty()) {
        g_need = command_length(w);
        g_polyline = g_need == 0;
    }
    g_fifo.push_back(w);
    if (g_polyline) {
        // terminated by 5xxx5xxx (on a vertex/colour word), at least two vertices
        bool gouraud = (g_fifo[0] >> 24) & 0x10;
        size_t min = gouraud ? 4 : 3;
        if (g_fifo.size() > min && (w & 0xf000f000u) == 0x50005000u) {
            gp0_command(g_fifo.data(), g_fifo.size());
            g_fifo.clear();
        } else if (g_fifo.size() > 4096) {
            g_fifo.clear();
        }
        return;
    }
    if (g_fifo.size() >= g_need) {
        gp0_command(g_fifo.data(), g_fifo.size());
        g_fifo.clear();
    }
}

static void gp1(uint32_t v) {
    uint32_t cmd = (v >> 24) & 0x3f;
    switch (cmd) {
        case 0x00:
            g_fifo.clear(); g_up_remaining = 0; g_down_active = false;
            g_irq = false; g_display_off = true; g_dma_dir = 0;
            g_disp_x = g_disp_y = 0; g_hrange = 0xc60260; g_vrange = 0x3fc10; g_dispmode = 0;
            g_drawmode = 0; g_texwin = 0; g_area_x1 = g_area_y1 = g_area_x2 = g_area_y2 = 0; g_off_x = g_off_y = 0;
            g_set_mask = g_check_mask = false;
            break;
        case 0x01: g_fifo.clear(); g_up_remaining = 0; break;
        case 0x02: g_irq = false; break;
        case 0x03: g_display_off = v & 1; break;
        case 0x04: g_dma_dir = v & 3; break;
        case 0x05: {
            int x = v & 0x3fe, y = (v >> 10) & 0x1ff;
            if (x != g_disp_x || y != g_disp_y) g_game_frames++;
            g_disp_x = x; g_disp_y = y;
            break;
        }
        case 0x06: g_hrange = v & 0xffffff; break;
        case 0x07: g_vrange = v & 0xfffff; break;
        case 0x08: g_dispmode = v & 0xff; break;
        case 0x10: case 0x11: case 0x12: case 0x13: case 0x14: case 0x15: case 0x16: case 0x17:
        case 0x18: case 0x19: case 0x1a: case 0x1b: case 0x1c: case 0x1d: case 0x1e: case 0x1f:
            switch (v & 0xf) {
                case 2: g_info_latch = g_texwin; break;
                case 3: g_info_latch = (uint32_t)g_area_x1 | ((uint32_t)g_area_y1 << 10); break;
                case 4: g_info_latch = (uint32_t)g_area_x2 | ((uint32_t)g_area_y2 << 10); break;
                case 5: g_info_latch = ((uint32_t)g_off_x & 0x7ff) | (((uint32_t)g_off_y & 0x7ff) << 11); break;
                case 7: g_info_latch = 2; break;
                case 8: g_info_latch = 0; break;
                default: break;
            }
            g_gpuread = g_info_latch;
            break;
        default: break;
    }
}

static int hres() {
    if (g_dispmode & 0x40) return 368;
    static const int r[4] = {256, 320, 512, 640};
    return r[g_dispmode & 3];
}

int gpu_dot_divider() {
    if (g_dispmode & 0x40) return 7;
    static const int d[4] = {10, 8, 5, 4};
    return d[g_dispmode & 3];
}

static int current_line() {
    int64_t el = g_cpu.cycles - hw_vblank_time();
    int64_t line = (el * 1000 / CYCLES_PER_LINE_FP) + 240;
    return (int)(line % LINES_PER_FRAME);
}

static bool g_field = false;
bool gpu_interlaced_odd() { return g_field; }

uint32_t gpu_read(int reg) {
    if (reg == 0) {
        if (g_down_active) {
            uint32_t lo = g_down_pos < g_down_buf.size() ? g_down_buf[g_down_pos++] : 0;
            uint32_t hi = g_down_pos < g_down_buf.size() ? g_down_buf[g_down_pos++] : 0;
            if (g_down_pos + 1 >= g_down_buf.size()) g_down_active = false;
            g_gpuread = lo | (hi << 16);
        }
        return g_gpuread;
    }
    uint32_t s = g_drawmode & 0x7ff;
    s |= (g_drawmode & 0x800) << 4;          // tpage y base 2 -> bit 15
    if (g_set_mask) s |= 1u << 11;
    if (g_check_mask) s |= 1u << 12;
    bool interlace = (g_dispmode >> 5) & 1;
    if (!interlace || g_field) s |= 1u << 13;
    s |= ((g_dispmode >> 6) & 1) << 16;
    s |= (g_dispmode & 3) << 17;
    s |= ((g_dispmode >> 2) & 1) << 19;
    s |= ((g_dispmode >> 3) & 1) << 20;
    s |= ((g_dispmode >> 4) & 1) << 21;
    s |= (uint32_t)interlace << 22;
    if (g_display_off) s |= 1u << 23;
    if (g_irq) s |= 1u << 24;
    s |= 1u << 26;                           // ready for commands
    if (g_down_active) s |= 1u << 27;        // ready to send VRAM to CPU
    s |= 1u << 28;                           // ready for DMA blocks
    s |= g_dma_dir << 29;
    switch (g_dma_dir) {
        case 1: case 2: s |= 1u << 25; break;
        case 3: if (g_down_active) s |= 1u << 25; break;
    }
    int line = current_line();
    if (line < 240) {
        if (interlace && (g_dispmode & 4)) { if (g_field) s |= 1u << 31; }
        else if (line & 1) s |= 1u << 31;
    }
    return s;
}

void gpu_write(int reg, uint32_t v) {
    if (reg == 0) gp0(v);
    else gp1(v);
}

void gpu_vblank() {
    g_vbl_count++;
    g_field = !g_field;
    DisplayInfo d;
    d.x = g_disp_x;
    d.y = g_disp_y;
    d.w = hres();
    int y1 = g_vrange & 0x3ff, y2 = (g_vrange >> 10) & 0x3ff;
    int lines = y2 - y1;
    if (lines <= 0 || lines > 288) { lines = 240; y1 = 16; }
    // The TV shows lines 16..255 (NTSC); the picture sits where the vertical range puts it.
    int top = y1 - 16;
    if (top < 0) { d.y += -top; lines += top; top = 0; }
    if (top + lines > 240) lines = std::max(0, 240 - top);
    bool interlace480 = ((g_dispmode >> 5) & 1) && ((g_dispmode >> 2) & 1);
    d.h = interlace480 ? lines * 2 : lines;
    d.top = interlace480 ? top * 2 : top;
    d.window_h = interlace480 ? 480 : 240;
    d.rgb24 = (g_dispmode >> 4) & 1;
    d.enabled = !g_display_off;
    LOG("disp", "display %d,%d %dx%d rgb24=%d hrange=%06x vrange=%05x mode=%02x", d.x, d.y, d.w, d.h, d.rgb24, g_hrange, g_vrange, g_dispmode);
    gfx_present(d);
}

void gpu_init() { gfx_init(); }
