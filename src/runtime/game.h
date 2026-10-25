// Muppet Monster Adventure (NTSC-U, SLUS-01238) - game-specific constants the runtime needs.
#pragma once
#define GAME_NAME "Muppet Monster Adventure"
#define GAME_INI "muppetmonster.ini"
#define GAME_EXE_NAME "SLUS_012.38"

// Main executable
static const unsigned EXE_TEXT = 0x80010000u;
// End of the executable's BSS; the level/boot code overlays (GAMEDATA\BOOT.BIN, WORLDS*\*.BIN)
// are all loaded here. Word 0 of every overlay is a unique id the runtime uses to pick the
// matching recompiled code.
static const unsigned OVL_BASE = 0x800DEBB4u;
static const unsigned OVL_END = 0x80100000u;

// SYSTEM.CNF: TCB = 4, EVENT = 16, STACK = 801FFF00
static const int SYS_TCBS = 4;
static const int SYS_EVENTS = 16;
static const unsigned SYS_STACK = 0x801FFF00u;
