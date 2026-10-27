// Interface between the GPU command processor (gpu.cpp) and the OpenGL renderer (gpu_gl.cpp).
#pragma once
#include <cstdint>

struct GfxVertex {
    float x, y;          // VRAM pixel coordinates (drawing offset applied)
    uint32_t color;      // 0x00BBGGRR
    float u, v;          // texture coordinates (texels)
    float w = 0;         // view depth from the GTE (0 = unknown: affine texturing)
};

struct GfxState {
    // texture
    bool textured = false, raw = false;
    int tex_mode = 0;              // 0 = 4bit, 1 = 8bit, 2 = 15bit
    int tpage_x = 0, tpage_y = 0;  // halfwords / lines
    int clut_x = 0, clut_y = 0;
    int win_mask_x = 0, win_mask_y = 0, win_off_x = 0, win_off_y = 0;   // in 8-texel units
    // blending / mask
    bool semi = false;
    int blend = 0;                 // 0 B/2+F/2, 1 B+F, 2 B-F, 3 B+F/4
    bool set_mask = false, check_mask = false;
    bool dither = false;
    bool sprite = false;           // rectangle (2D) rather than polygon
    // clipping (inclusive)
    int clip_x1 = 0, clip_y1 = 0, clip_x2 = 1023, clip_y2 = 511;
};

struct DisplayInfo {
    int x, y, w, h;      // display area in VRAM
    int top = 0;         // first picture line within the visible window (vertical display range start)
    int window_h = 240;  // visible lines on a standard TV (240 NTSC, 480 interlaced)
    bool rgb24;
    bool enabled;
};

void gfx_init();
void gfx_triangle(const GfxVertex v[3], const GfxState& s);
void gfx_fill(int x, int y, int w, int h, uint32_t color);
void gfx_vram_write(int x, int y, int w, int h, const uint16_t* data, bool set_mask, bool check_mask);
void gfx_vram_read(int x, int y, int w, int h, uint16_t* out);
void gfx_vram_copy(int sx, int sy, int dx, int dy, int w, int h);
void gfx_present(const DisplayInfo& d);
void gfx_flush();
void gfx_release_context();
void gfx_acquire_context();
int gfx_scale();
