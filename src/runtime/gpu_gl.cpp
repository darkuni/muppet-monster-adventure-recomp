// OpenGL 3.3 renderer for the PlayStation GPU, drawing into an upscaled copy of VRAM.
//
// VRAM is an RGBA8 texture of (1024*S) x (512*S): RGB holds the colour (true colour, no
// 15-bit truncation), alpha holds the mask bit. Texture pages and CLUTs are read in the
// fragment shader from a second texture (a copy of VRAM, refreshed only where drawing has
// touched it since the last copy), so palettes, texture windows and render-to-texture work
// like the hardware. Semi-transparency uses dual-source blending (per-texel STP bit); the
// B-F mode needs two passes.
#include "gpu.h"
#include "gl_include.h"
#include "config.h"
#include "log.h"
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

void gl_platform_present(unsigned tex, int w, int h);
bool gl_platform_init(int w, int h);
void gl_platform_make_current();
void gl_platform_release_current();

static int S = 2;
static GLuint g_vram, g_vram_read, g_ds;            // textures / depth-stencil renderbuffer
static GLuint g_fbo, g_fbo_read;
static GLuint g_tmp_tex, g_tmp_fbo;                 // native-size scratch (uploads, readback, copies)
static int g_tmp_w = 0, g_tmp_h = 0;
static GLuint g_out_tex, g_out_fbo;                 // presented frame
static int g_out_w = 0, g_out_h = 0;
static GLuint g_prog, g_prog24, g_prog_stencil, g_vao, g_vbo;
static GLint u_vram, u_scale, u_blend, u_pass, u_filter, u_dither;
static GLint u24_vram, u24_scale, u24_rect;
static GLint us_vram, us_scale;
static bool g_ready = false;

struct Vtx {
    float x, y;
    uint32_t color;
    float u, v;
    int16_t tex[4];     // tpage x, tpage y, clut x, clut y
    int16_t info[4];    // flags, window mask, window offset, unused
    int16_t lim[4];     // texel bounds of the primitive (u min, v min, u max, v max) for filtering
    float w;            // perspective divisor (1 = affine, like the PS1)
};
static std::vector<Vtx> g_batch;
static int g_batch_blend = -1;                      // blend mode of the semi-transparent primitives in the batch
static bool g_batch_check = false;
static int g_clip[4] = {0, 0, 1023, 511};
static int g_dirty[4] = {1 << 30, 1 << 30, -1, -1};   // x1, y1, x2, y2 (exclusive) touched since last read copy
static bool g_mask_used = false, g_stencil_dirty = true;

int gfx_scale() { return S; }

static GLuint compile(GLenum type, const char* src) {
    GLuint s = glCreateShader(type);
    glShaderSource(s, 1, &src, nullptr);
    glCompileShader(s);
    GLint ok = 0;
    glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[4096];
        glGetShaderInfoLog(s, sizeof log, nullptr, log);
        fprintf(stderr, "[gl] shader error:\n%s\n", log);
        exit(1);
    }
    return s;
}
static GLuint link(const char* vs, const char* fs, bool dual = false) {
    GLuint p = glCreateProgram();
    glAttachShader(p, compile(GL_VERTEX_SHADER, vs));
    glAttachShader(p, compile(GL_FRAGMENT_SHADER, fs));
    if (dual) {
        glBindFragDataLocationIndexed(p, 0, 0, "o_color");
        glBindFragDataLocationIndexed(p, 0, 1, "o_blend");
    }
    glLinkProgram(p);
    GLint ok = 0;
    glGetProgramiv(p, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[4096];
        glGetProgramInfoLog(p, sizeof log, nullptr, log);
        fprintf(stderr, "[gl] link error:\n%s\n", log);
        exit(1);
    }
    return p;
}

static const char* kDrawVS = R"(#version 330 core
layout(location = 0) in vec2 a_pos;
layout(location = 1) in vec4 a_color;
layout(location = 2) in vec2 a_uv;
layout(location = 3) in ivec4 a_tex;
layout(location = 4) in ivec4 a_info;
layout(location = 5) in ivec4 a_lim;
layout(location = 6) in float a_w;
out vec4 v_color;
out vec2 v_uv;
flat out ivec4 v_tex;
flat out ivec4 v_info;
flat out ivec4 v_lim;
void main() {
    gl_Position = vec4((a_pos.x / 512.0 - 1.0) * a_w, (a_pos.y / 256.0 - 1.0) * a_w, 0.0, a_w);
    v_color = a_color;
    v_uv = a_uv;
    v_tex = a_tex;
    v_info = a_info;
    v_lim = a_lim;
}
)";

static const char* kDrawFS = R"(#version 330 core
in vec4 v_color;
in vec2 v_uv;
flat in ivec4 v_tex;
flat in ivec4 v_info;
flat in ivec4 v_lim;
uniform sampler2D u_vram;
uniform int u_scale;
uniform int u_blend;
uniform int u_pass;
uniform int u_filter;   // 0 nearest, 1 bilinear on polygons, 2 bilinear on everything
uniform int u_dither;   // 0 true colour, 1 original 15-bit colour with the GPU's dither pattern
layout(location = 0, index = 0) out vec4 o_color;
layout(location = 0, index = 1) out vec4 o_blend;

uint pack16(vec4 c) {
    uvec4 q = uvec4(round(clamp(c, 0.0, 1.0) * vec4(31.0, 31.0, 31.0, 1.0)));
    return q.r | (q.g << 5u) | (q.b << 10u) | (q.a << 15u);
}
uint vram16(ivec2 p) {
    p &= ivec2(1023, 511);
    return pack16(texelFetch(u_vram, p * u_scale + ivec2(u_scale / 2), 0));
}
vec4 unpack16(uint v) {
    return vec4(float(v & 31u), float((v >> 5u) & 31u), float((v >> 10u) & 31u), float(v >> 15u) * 31.0) / 31.0;
}
// one texel at texture coordinate t (texture window applied); raw == 0 is transparent
vec4 fetch(ivec2 t, int mode, out uint raw) {
    t &= 255;
    ivec2 wm = ivec2(v_info.y & 31, (v_info.y >> 5) & 31) * 8;
    ivec2 wo = ivec2(v_info.z & 31, (v_info.z >> 5) & 31) * 8;
    t = (t & ~wm) | (wo & wm);
    if (mode == 0) {
        uint w = vram16(ivec2(v_tex.x + t.x / 4, v_tex.y + t.y));
        raw = vram16(ivec2(v_tex.z + int((w >> uint((t.x & 3) * 4)) & 15u), v_tex.w));
    } else if (mode == 1) {
        uint w = vram16(ivec2(v_tex.x + t.x / 2, v_tex.y + t.y));
        raw = vram16(ivec2(v_tex.z + int((w >> uint((t.x & 1) * 8)) & 255u), v_tex.w));
    } else {
        raw = vram16(ivec2(v_tex.x + t.x, v_tex.y + t.y));
    }
    return unpack16(raw);
}

const float kDither[16] = float[16](-4.0, 0.0, -3.0, 1.0, 2.0, -2.0, 3.0, -1.0, -3.0, 1.0, -4.0, 0.0, 3.0, -1.0, 2.0, -2.0);

void main() {
    int flags = v_info.x;
    bool textured = (flags & 1) != 0;
    vec3 col = v_color.rgb;
    float stp = 0.0;
    if (textured) {
        int mode = (flags >> 2) & 3;
        vec4 texel;
        uint raw;
        bool sprite = (flags & 64) != 0;
        if (u_filter == 2 || (u_filter == 1 && !sprite)) {
            // bilinear between the opaque neighbours (palette lookups first, so colour keys and
            // edges stay clean); the shape follows the nearest texel, the bounds keep neighbouring
            // atlas entries out
            fetch(ivec2(floor(v_uv)), mode, raw);
            if (raw == 0u) discard;
            vec2 p = v_uv - 0.5;
            ivec2 b = ivec2(floor(p));
            vec2 f = fract(p);
            ivec2 lo = v_lim.xy, hi = max(v_lim.zw, v_lim.xy);
            ivec2 t0 = clamp(b, lo, hi), t1 = clamp(b + 1, lo, hi);
            uint r00, r10, r01, r11;
            vec4 c00 = fetch(ivec2(t0.x, t0.y), mode, r00), c10 = fetch(ivec2(t1.x, t0.y), mode, r10);
            vec4 c01 = fetch(ivec2(t0.x, t1.y), mode, r01), c11 = fetch(ivec2(t1.x, t1.y), mode, r11);
            vec4 w = vec4((1.0 - f.x) * (1.0 - f.y), f.x * (1.0 - f.y), (1.0 - f.x) * f.y, f.x * f.y);
            w *= vec4(r00 != 0u, r10 != 0u, r01 != 0u, r11 != 0u);
            float ws = w.x + w.y + w.z + w.w;
            texel = unpack16(raw);
            if (ws > 0.0) texel.rgb = (c00.rgb * w.x + c10.rgb * w.y + c01.rgb * w.z + c11.rgb * w.w) / ws;
        } else if (mode < 2) {
            texel = fetch(ivec2(floor(v_uv)), mode, raw);
        } else {
            // 15-bit textures may be rendered at the internal resolution: sample the sub-texel
            ivec2 t = ivec2(floor(v_uv)) & 255;
            ivec2 wm = ivec2(v_info.y & 31, (v_info.y >> 5) & 31) * 8;
            ivec2 wo = ivec2(v_info.z & 31, (v_info.z >> 5) & 31) * 8;
            t = (t & ~wm) | (wo & wm);
            ivec2 base = ivec2(v_tex.x + t.x, v_tex.y + t.y) & ivec2(1023, 511);
            ivec2 sub = clamp(ivec2(floor(fract(v_uv) * float(u_scale))), ivec2(0), ivec2(u_scale - 1));
            texel = texelFetch(u_vram, base * u_scale + sub, 0);
            raw = pack16(texel);
        }
        if (raw == 0u) discard;
        stp = texel.a > 0.5 ? 1.0 : 0.0;
        if ((flags & 2) != 0) col = texel.rgb;
        else col = min(texel.rgb * v_color.rgb * (255.0 / 128.0), vec3(1.0));
    }
    if (u_dither != 0) {
        // the PS1 writes 15-bit colour, dithered (4x4 pattern, per original pixel) where enabled
        vec3 c8 = col * 255.0;
        if ((flags & 128) != 0) {
            ivec2 q = ivec2(gl_FragCoord.xy) / u_scale;
            c8 += kDither[(q.y & 3) * 4 + (q.x & 3)];
        }
        col = floor(clamp(c8, 0.0, 255.0) / 8.0) / 31.0;
    }
    bool semi = (flags & 16) != 0 && (!textured || stp > 0.5);
    if (u_pass == 1 && semi) discard;
    if (u_pass == 2 && !semi) discard;
    float mask = (flags & 32) != 0 ? 1.0 : stp;
    vec3 src = col;
    vec3 bf = vec3(0.0);
    if (semi) {
        if (u_blend == 0) { src = col * 0.5; bf = vec3(0.5); }
        else if (u_blend == 3) { src = col * 0.25; bf = vec3(1.0); }
        else bf = vec3(1.0);
    }
    o_color = vec4(src, mask);
    o_blend = vec4(bf, 0.0);
}
)";

static const char* kQuadVS = R"(#version 330 core
uniform vec4 u_rect;   // x, y, w, h in clip space units (0..1)
out vec2 v_p;
void main() {
    vec2 p = vec2(gl_VertexID & 1, gl_VertexID >> 1);
    v_p = p;
    vec2 q = u_rect.xy + p * u_rect.zw;
    gl_Position = vec4(q * 2.0 - 1.0, 0.0, 1.0);
}
)";

// 24-bit display: VRAM holds packed RGB888 pixels
static const char* k24FS = R"(#version 330 core
in vec2 v_p;
uniform sampler2D u_vram;
uniform int u_scale;
uniform ivec4 u_disp;  // x, y, w, h (VRAM)
out vec4 o;
uint vram16(ivec2 p) {
    p &= ivec2(1023, 511);
    vec4 c = texelFetch(u_vram, p * u_scale + ivec2(u_scale / 2), 0);
    uvec4 q = uvec4(round(clamp(c, 0.0, 1.0) * vec4(31.0, 31.0, 31.0, 1.0)));
    return q.r | (q.g << 5u) | (q.b << 10u) | (q.a << 15u);
}
void main() {
    ivec2 px = ivec2(floor(v_p * vec2(u_disp.zw)));
    int byte_off = px.x * 3;
    int y = u_disp.y + px.y;
    int x0 = u_disp.x + byte_off / 2;
    uint w0 = vram16(ivec2(x0, y)), w1 = vram16(ivec2(x0 + 1, y));
    uint pk = w0 | (w1 << 16u);
    uint rgb = (byte_off & 1) != 0 ? (pk >> 8u) : pk;
    o = vec4(float(rgb & 255u), float((rgb >> 8u) & 255u), float((rgb >> 16u) & 255u), 255.0) / 255.0;
}
)";

// rebuild the stencil buffer (mask bits) from VRAM alpha
static const char* kStencilFS = R"(#version 330 core
in vec2 v_p;
uniform sampler2D u_vram;
out vec4 o;
void main() {
    if (texelFetch(u_vram, ivec2(gl_FragCoord.xy), 0).a < 0.5) discard;
    o = vec4(0.0);
}
)";

static void ensure_tmp(int w, int h) {
    if (w <= g_tmp_w && h <= g_tmp_h) return;
    g_tmp_w = std::max(w, g_tmp_w);
    g_tmp_h = std::max(h, g_tmp_h);
    glBindTexture(GL_TEXTURE_2D, g_tmp_tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, g_tmp_w, g_tmp_h, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    glBindFramebuffer(GL_FRAMEBUFFER, g_tmp_fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, g_tmp_tex, 0);
}

static GLuint make_tex(int w, int h) {
    GLuint t;
    glGenTextures(1, &t);
    glBindTexture(GL_TEXTURE_2D, t);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    return t;
}

void gfx_init() {
    S = std::max(1, std::min(8, g_cfg.resolution_scale));
    if (!gl_platform_init(1024, 512)) { fprintf(stderr, "[gl] cannot create an OpenGL 3.3 context\n"); exit(1); }
    g_vram = make_tex(1024 * S, 512 * S);
    g_vram_read = make_tex(1024 * S, 512 * S);
    glGenRenderbuffers(1, &g_ds);
    glBindRenderbuffer(GL_RENDERBUFFER, g_ds);
    glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH24_STENCIL8, 1024 * S, 512 * S);
    glGenFramebuffers(1, &g_fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, g_fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, g_vram, 0);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_RENDERBUFFER, g_ds);
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) { fprintf(stderr, "[gl] VRAM framebuffer incomplete\n"); exit(1); }
    glClearColor(0, 0, 0, 0);
    glClearStencil(0);
    glClear(GL_COLOR_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);
    glGenFramebuffers(1, &g_fbo_read);
    glBindFramebuffer(GL_FRAMEBUFFER, g_fbo_read);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, g_vram_read, 0);
    glClear(GL_COLOR_BUFFER_BIT);
    g_tmp_tex = make_tex(16, 16);
    glGenFramebuffers(1, &g_tmp_fbo);
    ensure_tmp(1024, 512);
    g_out_tex = make_tex(16, 16);
    glGenFramebuffers(1, &g_out_fbo);

    g_prog = link(kDrawVS, kDrawFS, true);
    u_vram = glGetUniformLocation(g_prog, "u_vram");
    u_scale = glGetUniformLocation(g_prog, "u_scale");
    u_blend = glGetUniformLocation(g_prog, "u_blend");
    u_pass = glGetUniformLocation(g_prog, "u_pass");
    u_filter = glGetUniformLocation(g_prog, "u_filter");
    u_dither = glGetUniformLocation(g_prog, "u_dither");
    g_prog24 = link(kQuadVS, k24FS);
    u24_vram = glGetUniformLocation(g_prog24, "u_vram");
    u24_scale = glGetUniformLocation(g_prog24, "u_scale");
    u24_rect = glGetUniformLocation(g_prog24, "u_disp");
    g_prog_stencil = link(kQuadVS, kStencilFS);
    us_vram = glGetUniformLocation(g_prog_stencil, "u_vram");

    glGenVertexArrays(1, &g_vao);
    glBindVertexArray(g_vao);
    glGenBuffers(1, &g_vbo);
    glBindBuffer(GL_ARRAY_BUFFER, g_vbo);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, sizeof(Vtx), (void*)offsetof(Vtx, x));
    glVertexAttribPointer(1, 4, GL_UNSIGNED_BYTE, GL_TRUE, sizeof(Vtx), (void*)offsetof(Vtx, color));
    glVertexAttribPointer(2, 2, GL_FLOAT, GL_FALSE, sizeof(Vtx), (void*)offsetof(Vtx, u));
    glVertexAttribIPointer(3, 4, GL_SHORT, sizeof(Vtx), (void*)offsetof(Vtx, tex));
    glVertexAttribIPointer(4, 4, GL_SHORT, sizeof(Vtx), (void*)offsetof(Vtx, info));
    glVertexAttribIPointer(5, 4, GL_SHORT, sizeof(Vtx), (void*)offsetof(Vtx, lim));
    glVertexAttribPointer(6, 1, GL_FLOAT, GL_FALSE, sizeof(Vtx), (void*)offsetof(Vtx, w));
    for (int i = 0; i < 7; i++) glEnableVertexAttribArray(i);
    g_ready = true;
    fprintf(stderr, "[gpu] internal resolution %dx (%dx%d VRAM)\n", S, 1024 * S, 512 * S);
}

// ---------------------------------------------------------------- batching
static void add_dirty(int x1, int y1, int x2, int y2) {
    g_dirty[0] = std::min(g_dirty[0], std::max(0, x1));
    g_dirty[1] = std::min(g_dirty[1], std::max(0, y1));
    g_dirty[2] = std::max(g_dirty[2], std::min(1024, x2));
    g_dirty[3] = std::max(g_dirty[3], std::min(512, y2));
    g_stencil_dirty = true;
}
static bool dirty_overlaps(int x1, int y1, int x2, int y2) {
    return g_dirty[2] > g_dirty[0] && x1 < g_dirty[2] && x2 > g_dirty[0] && y1 < g_dirty[3] && y2 > g_dirty[1];
}

static void set_blend(int mode, int pass) {
    if (mode < 0) { glDisable(GL_BLEND); return; }
    glEnable(GL_BLEND);
    if (mode == 2 && pass == 2) glBlendEquationSeparate(GL_FUNC_REVERSE_SUBTRACT, GL_FUNC_ADD);
    else glBlendEquationSeparate(GL_FUNC_ADD, GL_FUNC_ADD);
    glBlendFuncSeparate(GL_ONE, GL_SRC1_COLOR, GL_ONE, GL_ZERO);
}

static void ensure_stencil() {
    if (!g_stencil_dirty) return;
    // stencil = 1 where the mask bit (alpha) is set
    glBindFramebuffer(GL_FRAMEBUFFER, g_fbo);
    glViewport(0, 0, 1024 * S, 512 * S);
    glDisable(GL_SCISSOR_TEST);
    glDisable(GL_BLEND);
    glClear(GL_STENCIL_BUFFER_BIT);
    // source must not be the bound target: refresh the read copy first
    glBindFramebuffer(GL_READ_FRAMEBUFFER, g_fbo);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, g_fbo_read);
    glBlitFramebuffer(0, 0, 1024 * S, 512 * S, 0, 0, 1024 * S, 512 * S, GL_COLOR_BUFFER_BIT, GL_NEAREST);
    g_dirty[0] = g_dirty[1] = 1 << 30; g_dirty[2] = g_dirty[3] = -1;
    glBindFramebuffer(GL_FRAMEBUFFER, g_fbo);
    glEnable(GL_STENCIL_TEST);
    glStencilFunc(GL_ALWAYS, 1, 1);
    glStencilOp(GL_KEEP, GL_KEEP, GL_REPLACE);
    glStencilMask(1);
    glColorMask(GL_FALSE, GL_FALSE, GL_FALSE, GL_FALSE);
    glUseProgram(g_prog_stencil);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, g_vram_read);
    glUniform1i(us_vram, 0);
    glUniform4f(glGetUniformLocation(g_prog_stencil, "u_rect"), 0, 0, 1, 1);
    glBindVertexArray(g_vao);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glDisable(GL_STENCIL_TEST);
    g_stencil_dirty = false;
}

extern int g_stat_flushes, g_stat_tris;
void gfx_flush() {
    if (g_batch.empty()) return;
    g_stat_flushes++; g_stat_tris += (int)g_batch.size() / 3;
    if (g_batch_check) ensure_stencil();
    glBindFramebuffer(GL_FRAMEBUFFER, g_fbo);
    glViewport(0, 0, 1024 * S, 512 * S);
    glEnable(GL_SCISSOR_TEST);
    glScissor(g_clip[0] * S, g_clip[1] * S, (g_clip[2] - g_clip[0] + 1) * S, (g_clip[3] - g_clip[1] + 1) * S);
    if (g_batch_check) {
        glEnable(GL_STENCIL_TEST);
        glStencilFunc(GL_EQUAL, 0, 1);
        glStencilOp(GL_KEEP, GL_KEEP, GL_KEEP);
    }
    glUseProgram(g_prog);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, g_vram_read);
    glUniform1i(u_vram, 0);
    glUniform1i(u_scale, S);
    glUniform1i(u_filter, g_cfg.texture_filter);
    glUniform1i(u_dither, g_cfg.dither);
    int mode = g_batch_blend < 0 ? 0 : g_batch_blend;
    glUniform1i(u_blend, mode);
    glBindVertexArray(g_vao);
    glBindBuffer(GL_ARRAY_BUFFER, g_vbo);
    glBufferData(GL_ARRAY_BUFFER, g_batch.size() * sizeof(Vtx), g_batch.data(), GL_STREAM_DRAW);
    if (mode == 2) {
        set_blend(2, 1);
        glUniform1i(u_pass, 1);
        glDrawArrays(GL_TRIANGLES, 0, (GLsizei)g_batch.size());
        set_blend(2, 2);
        glUniform1i(u_pass, 2);
        glDrawArrays(GL_TRIANGLES, 0, (GLsizei)g_batch.size());
    } else {
        set_blend(mode, 0);
        glUniform1i(u_pass, 0);
        glDrawArrays(GL_TRIANGLES, 0, (GLsizei)g_batch.size());
    }
    glDisable(GL_BLEND);
    glDisable(GL_STENCIL_TEST);
    glDisable(GL_SCISSOR_TEST);
    g_batch.clear();
    g_batch_blend = -1;
}

int g_stat_copies = 0, g_stat_flushes = 0, g_stat_tris = 0; int64_t g_stat_copy_px = 0;
static void sync_read_copy() {
    if (g_dirty[2] <= g_dirty[0] || g_dirty[3] <= g_dirty[1]) return;
    gfx_flush();
    int x1 = g_dirty[0] * S, y1 = g_dirty[1] * S, x2 = g_dirty[2] * S, y2 = g_dirty[3] * S;
    glBindFramebuffer(GL_READ_FRAMEBUFFER, g_fbo);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, g_fbo_read);
    glDisable(GL_SCISSOR_TEST);
    glBlitFramebuffer(x1, y1, x2, y2, x1, y1, x2, y2, GL_COLOR_BUFFER_BIT, GL_NEAREST);
    g_stat_copies++; g_stat_copy_px += (int64_t)(x2 - x1) * (y2 - y1);
    g_dirty[0] = g_dirty[1] = 1 << 30;
    g_dirty[2] = g_dirty[3] = -1;
}

void gfx_triangle(const GfxVertex v[3], const GfxState& s) {
    if (!g_ready) return;
    // texture footprint must be up to date in the read copy
    if (s.textured) {
        int pw = s.tex_mode == 0 ? 64 : s.tex_mode == 1 ? 128 : 256;
        bool hit = dirty_overlaps(s.tpage_x, s.tpage_y, s.tpage_x + pw, s.tpage_y + 256) ||
                   (s.tpage_x + pw > 1024 && dirty_overlaps(0, s.tpage_y, s.tpage_x + pw - 1024, s.tpage_y + 256));
        if (s.tex_mode < 2) {
            int cw = s.tex_mode == 0 ? 16 : 256;
            hit = hit || dirty_overlaps(s.clut_x, s.clut_y, s.clut_x + cw, s.clut_y + 1);
        }
        if (hit) sync_read_copy();
    }
    // batch state
    int clip[4] = {s.clip_x1, s.clip_y1, s.clip_x2, s.clip_y2};
    if (clip[2] < clip[0] || clip[3] < clip[1]) return;
    bool clip_changed = memcmp(clip, g_clip, sizeof clip) != 0;
    bool blend_conflict = s.semi && g_batch_blend >= 0 && g_batch_blend != s.blend;
    // Subtractive blending (B-F) draws a batch in two passes, opaque pixels first: keep the
    // submission order by never mixing semi-transparent and opaque triangles in such a batch
    // (otherwise shadows land on top of walls drawn after them).
    static bool last_semi = false;
    int batch_mode = s.semi ? s.blend : g_batch_blend;
    if (!g_batch.empty() && batch_mode == 2 && s.semi != last_semi) blend_conflict = true;
    last_semi = s.semi;
    if (s.check_mask) g_mask_used = true;
    if (clip_changed || blend_conflict || s.check_mask != g_batch_check || g_batch.size() > 60000) {
        gfx_flush();
        memcpy(g_clip, clip, sizeof clip);
        g_batch_check = s.check_mask;
    }
    if (s.semi) g_batch_blend = s.blend;
    int16_t flags = (int16_t)((s.textured ? 1 : 0) | (s.raw ? 2 : 0) | (s.tex_mode << 2) | (s.semi ? 16 : 0) | (s.set_mask ? 32 : 0) |
                              (s.sprite ? 64 : 0) | (s.dither ? 128 : 0));
    // texel bounds for filtering: a primitive's own texture area (sprites end one texel early)
    float umin = std::min(v[0].u, std::min(v[1].u, v[2].u)), umax = std::max(v[0].u, std::max(v[1].u, v[2].u));
    float vmin = std::min(v[0].v, std::min(v[1].v, v[2].v)), vmax = std::max(v[0].v, std::max(v[1].v, v[2].v));
    if (s.sprite) { umax -= 1; vmax -= 1; }
    int16_t wm = (int16_t)(s.win_mask_x | (s.win_mask_y << 5));
    int16_t wo = (int16_t)(s.win_off_x | (s.win_off_y << 5));
    float minx = 1e9f, miny = 1e9f, maxx = -1e9f, maxy = -1e9f;
    // perspective-correct texturing when the GTE depth of all three vertices is known
    bool persp = s.textured && v[0].w > 0 && v[1].w > 0 && v[2].w > 0;
    for (int i = 0; i < 3; i++) {
        Vtx t;
        t.w = persp ? v[i].w / 4096.0f : 1.0f;
        t.x = v[i].x; t.y = v[i].y;
        t.color = v[i].color | 0xff000000u;
        t.u = v[i].u; t.v = v[i].v;
        t.tex[0] = (int16_t)s.tpage_x; t.tex[1] = (int16_t)s.tpage_y; t.tex[2] = (int16_t)s.clut_x; t.tex[3] = (int16_t)s.clut_y;
        t.info[0] = flags; t.info[1] = wm; t.info[2] = wo; t.info[3] = 0;
        t.lim[0] = (int16_t)umin; t.lim[1] = (int16_t)vmin; t.lim[2] = (int16_t)std::max(umin, umax); t.lim[3] = (int16_t)std::max(vmin, vmax);
        g_batch.push_back(t);
        minx = std::min(minx, v[i].x); maxx = std::max(maxx, v[i].x);
        miny = std::min(miny, v[i].y); maxy = std::max(maxy, v[i].y);
    }
    add_dirty(std::max((int)minx, clip[0]), std::max((int)miny, clip[1]),
              std::min((int)maxx + 1, clip[2] + 1), std::min((int)maxy + 1, clip[3] + 1));
}

// ---------------------------------------------------------------- VRAM operations
void gfx_fill(int x, int y, int w, int h, uint32_t color) {
    if (!g_ready) return;
    gfx_flush();
    glBindFramebuffer(GL_FRAMEBUFFER, g_fbo);
    glEnable(GL_SCISSOR_TEST);
    glClearColor((color & 0xff) / 255.f, ((color >> 8) & 0xff) / 255.f, ((color >> 16) & 0xff) / 255.f, 0.f);
    // wraps around the VRAM edges
    for (int py = y; py < y + h; py = (py / 512 + 1) * 512) {
        int hh = std::min(y + h, (py / 512 + 1) * 512) - py;
        for (int px = x; px < x + w; px = (px / 1024 + 1) * 1024) {
            int ww = std::min(x + w, (px / 1024 + 1) * 1024) - px;
            int rx = px & 1023, ry = py & 511;
            glScissor(rx * S, ry * S, ww * S, hh * S);
            glClear(GL_COLOR_BUFFER_BIT);
            add_dirty(rx, ry, rx + ww, ry + hh);
        }
    }
    glDisable(GL_SCISSOR_TEST);
}

static void upload_native(int x, int y, int w, int h, const uint32_t* rgba) {
    ensure_tmp(w, h);
    glBindTexture(GL_TEXTURE_2D, g_tmp_tex);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, g_tmp_fbo);
    glDisable(GL_SCISSOR_TEST);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, g_fbo);
    glBlitFramebuffer(0, 0, w, h, x * S, y * S, (x + w) * S, (y + h) * S, GL_COLOR_BUFFER_BIT, GL_NEAREST);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, g_fbo_read);
    glBlitFramebuffer(0, 0, w, h, x * S, y * S, (x + w) * S, (y + h) * S, GL_COLOR_BUFFER_BIT, GL_NEAREST);
    g_stencil_dirty = true;
}

static inline uint32_t c16_to_rgba(uint16_t c) {
    uint32_t r = c & 31, g = (c >> 5) & 31, b = (c >> 10) & 31;
    r = (r << 3) | (r >> 2); g = (g << 3) | (g >> 2); b = (b << 3) | (b >> 2);
    return r | (g << 8) | (b << 16) | ((c & 0x8000) ? 0xff000000u : 0u);
}

void gfx_vram_write(int x, int y, int w, int h, const uint16_t* data, bool set_mask, bool check_mask) {
    if (!g_ready) return;
    gfx_flush();
    if (check_mask) {
        static bool warned = false;
        if (!warned) { warned = true; LOG("gpu", "VRAM upload with mask check (ignored)"); }
    }
    // split at the VRAM edges (wrapping)
    std::vector<uint32_t> buf;
    for (int py = 0; py < h;) {
        int ry = (y + py) & 511;
        int hh = std::min(h - py, 512 - ry);
        for (int px = 0; px < w;) {
            int rx = (x + px) & 1023;
            int ww = std::min(w - px, 1024 - rx);
            buf.resize((size_t)ww * hh);
            for (int j = 0; j < hh; j++)
                for (int i = 0; i < ww; i++) {
                    uint16_t c = data[(size_t)(py + j) * w + px + i];
                    if (set_mask) c |= 0x8000;
                    buf[(size_t)j * ww + i] = c16_to_rgba(c);
                }
            // the read copy is updated too; keep the draw copy's dirty state as is
            upload_native(rx, ry, ww, hh, buf.data());
            px += ww;
        }
        py += hh;
    }
}

void gfx_vram_read(int x, int y, int w, int h, uint16_t* out) {
    if (!g_ready) { memset(out, 0, (size_t)w * h * 2); return; }
    gfx_flush();
    ensure_tmp(w, h);
    std::vector<uint32_t> px((size_t)w * h);
    for (int j = 0; j < h; j++) {
        // rows may wrap; read row by row through the scratch buffer when needed
        (void)j;
    }
    glBindFramebuffer(GL_READ_FRAMEBUFFER, g_fbo);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, g_tmp_fbo);
    glDisable(GL_SCISSOR_TEST);
    int ww = std::min(w, 1024 - x), hh = std::min(h, 512 - y);
    glBlitFramebuffer(x * S, y * S, (x + ww) * S, (y + hh) * S, 0, 0, ww, hh, GL_COLOR_BUFFER_BIT, GL_NEAREST);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, g_tmp_fbo);
    glPixelStorei(GL_PACK_ALIGNMENT, 4);
    glReadPixels(0, 0, ww, hh, GL_RGBA, GL_UNSIGNED_BYTE, px.data());
    for (int j = 0; j < h; j++)
        for (int i = 0; i < w; i++) {
            uint32_t c = (i < ww && j < hh) ? px[(size_t)j * ww + i] : 0;
            uint32_t r = ((c & 0xff) + 4) * 31 / 255, g = (((c >> 8) & 0xff) + 4) * 31 / 255, b = (((c >> 16) & 0xff) + 4) * 31 / 255;
            r = std::min(r, 31u); g = std::min(g, 31u); b = std::min(b, 31u);
            out[(size_t)j * w + i] = (uint16_t)(r | (g << 5) | (b << 10) | ((c >> 31) ? 0x8000 : 0));
        }
}

void gfx_vram_copy(int sx, int sy, int dx, int dy, int w, int h) {
    if (!g_ready) return;
    gfx_flush();
    w = std::min(w, std::min(1024 - sx, 1024 - dx));
    h = std::min(h, std::min(512 - sy, 512 - dy));
    if (w <= 0 || h <= 0) return;
    // via a scratch texture at full resolution (source and destination may overlap)
    static GLuint big = 0, big_fbo = 0;
    if (!big) {
        big = make_tex(1024 * S, 512 * S);
        glGenFramebuffers(1, &big_fbo);
        glBindFramebuffer(GL_FRAMEBUFFER, big_fbo);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, big, 0);
    }
    glDisable(GL_SCISSOR_TEST);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, g_fbo);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, big_fbo);
    glBlitFramebuffer(sx * S, sy * S, (sx + w) * S, (sy + h) * S, 0, 0, w * S, h * S, GL_COLOR_BUFFER_BIT, GL_NEAREST);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, big_fbo);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, g_fbo);
    glBlitFramebuffer(0, 0, w * S, h * S, dx * S, dy * S, (dx + w) * S, (dy + h) * S, GL_COLOR_BUFFER_BIT, GL_NEAREST);
    add_dirty(dx, dy, dx + w, dy + h);
}

// ---------------------------------------------------------------- presentation
static void write_screenshot(const char* path, int w, int h) {
    std::vector<uint8_t> px((size_t)w * h * 4);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, g_out_fbo);
    glPixelStorei(GL_PACK_ALIGNMENT, 4);
    glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, px.data());
    FILE* f = fopen(path, "wb");
    if (!f) return;
    fprintf(f, "P6\n%d %d\n255\n", w, h);
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) fwrite(&px[((size_t)y * w + x) * 4], 1, 3, f);
    fclose(f);
}

void gfx_present(const DisplayInfo& d) {
    if (!g_ready) return;
    gfx_flush();
    int ps = d.rgb24 ? 1 : S;     // 24-bit (movie) frames are presented at their native size
    int ow = d.w * ps, oh = d.window_h * ps;
    if (ow != g_out_w || oh != g_out_h) {
        glBindTexture(GL_TEXTURE_2D, g_out_tex);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, ow, oh, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
        glBindFramebuffer(GL_FRAMEBUFFER, g_out_fbo);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, g_out_tex, 0);
        g_out_w = ow; g_out_h = oh;
    }
    glBindFramebuffer(GL_FRAMEBUFFER, g_out_fbo);
    glViewport(0, 0, ow, oh);
    glDisable(GL_SCISSOR_TEST);
    glDisable(GL_BLEND);
    glClearColor(0, 0, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    if (!d.enabled || d.h <= 0) {
    } else if (d.rgb24) {
        sync_read_copy();
        glBindFramebuffer(GL_FRAMEBUFFER, g_out_fbo);
        glViewport(0, d.top, ow, d.h);
        glUseProgram(g_prog24);
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, g_vram_read);
        glUniform1i(u24_vram, 0);
        glUniform1i(u24_scale, S);
        glUniform4i(u24_rect, d.x, d.y, d.w, d.h);
        glUniform4f(glGetUniformLocation(g_prog24, "u_rect"), 0, 0, 1, 1);
        glBindVertexArray(g_vao);
        glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    } else {
        glBindFramebuffer(GL_READ_FRAMEBUFFER, g_fbo);
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, g_out_fbo);
        int w = std::min(d.w, 1024 - d.x), h = std::min(d.h, 512 - d.y);
        glBlitFramebuffer(d.x * S, d.y * S, (d.x + w) * S, (d.y + h) * S, 0, d.top * S, w * S, (d.top + h) * S,
                          GL_COLOR_BUFFER_BIT, GL_NEAREST);
    }
    static int frame = 0;
    frame++;
    if (log_enabled("gstat") && frame % 60 == 0) {
        fprintf(stderr, "[gstat] per frame: %d copies (%.2f Mpx), %d flushes, %d tris\n", g_stat_copies / 60,
                g_stat_copy_px / 60.0 / 1e6, g_stat_flushes / 60, g_stat_tris / 60);
        g_stat_copies = g_stat_flushes = g_stat_tris = 0; g_stat_copy_px = 0;
    }
    static const char* shot_dir = getenv("SHOT_DIR");
    static int shot_every = getenv("SHOT_EVERY") ? atoi(getenv("SHOT_EVERY")) : 0;
    static int shot_from = getenv("SHOT_FROM") ? atoi(getenv("SHOT_FROM")) : 0;
    if (shot_dir && shot_every > 0 && frame >= shot_from && frame % shot_every == 0) {
        char path[512];
        snprintf(path, sizeof path, "%s/f%06d.ppm", shot_dir, frame);
        write_screenshot(path, ow, oh);
    }
    if (getenv("VRAM_DUMP_AT") && frame == atoi(getenv("VRAM_DUMP_AT"))) {
        // full VRAM, native size, for debugging
        std::vector<uint16_t> v(1024 * 512);
        gfx_vram_read(0, 0, 1024, 512, v.data());
        FILE* f = fopen("vram.ppm", "wb");
        if (f) {
            fprintf(f, "P6\n1024 512\n255\n");
            for (uint16_t c : v) { uint8_t p[3] = {(uint8_t)((c & 31) << 3), (uint8_t)(((c >> 5) & 31) << 3), (uint8_t)(((c >> 10) & 31) << 3)}; fwrite(p, 1, 3, f); }
            fclose(f);
        }
    }
    gl_platform_present(g_out_tex, ow, oh);
}

void gfx_release_context() { gfx_flush(); glFlush(); gl_platform_release_current(); }
void gfx_acquire_context() { gl_platform_make_current(); }
