#pragma once
// Precise (sub-pixel) vertex positions, PGXP-style: the GTE's projected coordinates keep their
// fraction and depth; when the game stores SXY words to RAM (swc2) the precise values are
// remembered per RAM word, and the GPU uses them for polygon vertices read from those words
// (or, for words the game copied around, by value from this frame's projections).
#include <cstdint>
struct PreciseVtx { float x, y, w; };   // screen x/y (no drawing offset), view depth
void precise_store(uint32_t addr, uint32_t val, const PreciseVtx& p);
void precise_note(uint32_t val, const PreciseVtx& p);   // value read into a CPU register (mfc2)
bool precise_lookup(uint32_t addr, uint32_t val, PreciseVtx* p);
void precise_frame();                  // once per vblank: forget the by-value cache
