# Muppet Monster Adventure - native recompilation (private)

A static recompilation of **Muppet Monster Adventure** (PlayStation, NTSC-U, `SLUS-01238`,
Magenta Software / Midway, 2000) to native code. The game's MIPS executable and its 35 code
overlays are translated to C++ and run on Windows (and Linux) with a runtime that replaces the
PlayStation hardware and BIOS. Graphics are re-rendered with OpenGL at higher resolution.

**No game data is included.** You need your own copy of the game as a `.cue`/`.bin` disc image.

## Status

- Plays through the intro movies, title screen, new game (Hub tutorial with Pepe), level select
  and the castle level; saving and loading on the memory card work.
- Verified on the headless Linux test build against a reference emulator. The Windows build is
  produced by CI.

See [NOTES.md](NOTES.md) for the technical write-up.

## Running (Windows)

1. Download the `muppetmonster-win64` artifact from the latest **Build Windows** run (Actions tab).
2. Put your disc image (`Muppet Monster Adventure.cue` + its `.bin` file(s)) in the `game` folder
   next to `muppetmonster.exe`. One `.bin` per track and single-`.bin` rips both work.
3. Run `muppetmonster.exe` (or drag the `.cue` onto it).

Saves go to `memcard1.mcd` next to the exe - a standard 128 KB PlayStation memory card image, so
saves can be copied to and from emulators. Settings are in `muppetmonster.ini`.

### In-game settings (F1 or Esc, Start+Select on a controller)

| Setting | Options |
|---|---|
| Resolution | 1x (original) to 6x internal resolution (restart) |
| Widescreen 16:9 | wider view of the 3D world |
| VSync, Fullscreen (F11 / Alt+Enter) | |
| Scaling | sharp or smooth |
| Swap A/B and X/Y | face buttons by position |
| Vibration | DualShock rumble on your controller |
| Fast CD loading | levels load faster; music and movies stay real-time |
| Game speed | 1x-8x fast-forward (hold Tab for a burst) |
| Frame rate display | |
| Keyboard keys | rebind every PlayStation button (or reset to defaults) |

### Controls

Any XInput/SDL gamepad works as a DualShock (analog sticks and rumble when the game enables them).
Keyboard defaults: arrows = d-pad, X = Cross, Z = Circle, A = Square, S = Triangle, Q/W = L1/R1,
1/2 = L2/R2, Enter = Start, Backspace = Select, I/J/K/L = right stick, Tab = fast-forward (hold).
Change them in the F1 menu under **Keyboard keys** (saved in `muppetmonster.ini`).

## Layout

| Path | What it is |
|---|---|
| `src/recomp.py`, `src/mips.py` | Recompiler: executable + overlays -> `src/gen/*.cpp` (one C++ function per MIPS function) |
| `src/gen/` | Generated code (committed so CI can build without the disc) |
| `src/runtime/` | Runtime: memory map, HLE BIOS (events, threads, interrupts, memory cards), GTE, GPU -> OpenGL, SPU, CD-ROM (XA/CD-DA), MDEC, controllers, timers, DMA |
| `config/functions.txt` | Extra entry points and functions that run in the interpreter |
| `config/hle.txt` | Guest functions replaced by host code |
| `config/psyq.txt` | PsyQ library names found by signature |
| `tools/` | Disc preparation, disassembler, signature matcher, reference STR/BS decoder |

## Regenerating the code

```sh
python3 tools/prepare_disc.py "Muppet Monster Adventure.cue" game   # executable + overlays
python3 src/recomp.py game config src/gen 300
```

## Building

Linux headless test build (EGL, screenshots): `cmake -S src -B build -G Ninja && ninja -C build`,
then `SHOT_DIR=shots SHOT_EVERY=60 FRAMES=3000 ./build/muppetmonster game.cue`.

Windows (cross-compiled with mingw-w64): see `.github/workflows/build-windows.yml`.

## Disclaimer

This is an unofficial fan project. It is **not affiliated with, endorsed by, or sponsored by** The Jim
Henson Company, Midway, Psygnosis, Magenta Software or Sony. *Muppet Monster Adventure*, the Muppets
and all related names and marks belong to their respective owners. This repository contains no game
data; you need your own legally obtained copy of the game.

## License

The project's own code is released under the [MIT License](LICENSE). Third-party components keep
their own licenses; see [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).
