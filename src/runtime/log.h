// Category logging. Enable with the LOG environment variable: LOG=all, or LOG=bios,cd,gpu
#pragma once
#include <cstdio>
bool log_enabled(const char* cat);
void log_printf(const char* cat, const char* fmt, ...)
#ifdef __GNUC__
    __attribute__((format(printf, 2, 3)))
#endif
    ;
#define LOG(cat, ...) do { if (log_enabled(cat)) log_printf(cat, __VA_ARGS__); } while (0)
