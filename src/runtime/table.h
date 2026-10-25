// Lookup tables produced by the recompiler (gen/table.cpp)
#pragma once
#include "recomp.h"
typedef void (*GuestFn)(Cpu*);
struct FuncEntry { uint32_t addr; GuestFn fn; };
struct OverlayEntry { uint32_t id; const char* name; uint32_t size; const FuncEntry* funcs; int count; };
extern const FuncEntry g_exe_funcs[];
extern const size_t g_exe_funcs_count;
extern const OverlayEntry g_overlays[];
extern const size_t g_overlays_count;
