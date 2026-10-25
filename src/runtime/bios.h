// High-level emulation of the PlayStation BIOS (kernel calls, events, threads, interrupts).
#pragma once
#include "recomp.h"
#include <string_view>
#include <string>

bool bios_dispatch(Cpu* c, uint32_t addr);   // A0/B0/C0 vectors (and BIOS table entries); false if not BIOS
bool bios_is_vector(uint32_t addr);
void bios_syscall(Cpu* c, uint32_t pc);
void bios_exception(Cpu* c);                 // hardware interrupt
void bios_init(const char* save_dir);
[[noreturn]] void bios_run_exe(const uint8_t* exe, size_t size);

// Call guest code from the host with up to 4 arguments; returns v0.
uint32_t guest_call(Cpu* c, uint32_t addr, uint32_t a0 = 0, uint32_t a1 = 0, uint32_t a2 = 0, uint32_t a3 = 0);

// guest memory helpers
uint32_t gw(uint32_t a);
void sw_(uint32_t a, uint32_t v);
uint8_t* guest_ptr(uint32_t a);              // RAM only
void guest_strcpy_out(uint32_t a, const char* s);
std::string_view guest_str(uint32_t a);

// memory card (memcard.cpp)
void memcard_init(const char* dir);
bool memcard_present(int slot);
bool memcard_read_sector(int slot, int sector, uint8_t* dst);
bool memcard_write_sector(int slot, int sector, const uint8_t* src);
int memcard_open(int slot, const char* name, uint32_t mode);           // returns handle or -1
int memcard_rw(int h, uint8_t* buf, int len, bool write);
int memcard_seek(int h, int off, int whence);
void memcard_close(int h);
bool memcard_find(int slot, const char* pattern, int start, char* name, int* size, int* index);
bool memcard_erase(int slot, const char* name);
