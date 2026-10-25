#pragma once
#include <cstdint>
// Hook for precise (sub-pixel) projected vertices; filled in by the renderer's vertex cache.
struct GteState {
    void (*vertex_hook)(int32_t sx16, int32_t sy16, uint32_t z) = nullptr;   // 16.16 screen coords
    inline void on_vertex(int, int32_t, int32_t, int32_t z, int64_t sx, int64_t sy, uint32_t) {
        if (vertex_hook) vertex_hook((int32_t)sx, (int32_t)sy, (uint32_t)(z >> 12));
    }
};
extern GteState g_gte;
extern float g_gte_widescreen;
void gte_reset();
