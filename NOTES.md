# Technical notes

## Binary layout

- `SLUS_012.38`: PS-X EXE, text loaded at `0x80010000` (0xA8800 bytes), entry `0x8009C520`,
  BSS up to `0x800DEBB4`. Linked with PsyQ libraries (1997-98 era: libetc, libcd, libpad, libspu,
  libgpu, libgte, libcard/libmcrd).
- Code overlays all load at `0x800DEBB4` (the end of the BSS): `GAMEDATA\BOOT.BIN` (logos/movie
  player) and one `.BIN` per world/level (`WORLDS1..4\*.BIN`, 35 in total). Word 0 of each overlay
  is a unique id (BOOT = 09, FRONT1 = 08, CASTLE1 = 0C ... DEMOB = 2D); the runtime reads it to pick
  the matching recompiled code for any call into the overlay region.
- The game was developed by Magenta Software; movies are standard STR files with BS **v3** video.

## Recompiler (`src/recomp.py`)

- Function discovery: entry point, `jal` targets, address-taken constants (`lui`/`addiu`),
  data words pointing at plausible function starts, and a final sweep of unreferenced
  `... jr ra` runs between known code. Iterates to a fixed point (~5900 functions).
- One C++ function per guest function; branches become `goto`, calls become direct C calls,
  `jr ra` returns. Branch delay slots are emitted after the condition is evaluated. Jumps into
  the middle of a delay slot (hand-written asm loops) get a separate copy of the slot.
- R3000 load delay: a load followed by an instruction reading the same register is emitted with
  the old value visible to that instruction (the game's GCC code never needs it, but it is handled).
- Jump tables (`lui/addiu/addu/lw/jr`) become `switch`; other indirect jumps and `jalr` go through
  `rt_call`, which dispatches to recompiled code, HLE BIOS functions, or the interpreter.
- Cycle counting per basic block; `CHECK_EVENTS` at function entries and loop back-edges runs the
  hardware scheduler and takes interrupts.
- `config/functions.txt` `interp` entries run in the interpreter (`src/runtime/cpu.cpp`). Used for
  BOOT.BIN's movie VLC decoder, which uses `ra` as a continuation pointer (saves `ra` in its state
  block; its helper "returns" through it into the middle of the decoder), so `jr ra` is not a
  return there.

## Runtime

- **BIOS** (HLE, `bios.cpp`): A0/B0/C0 functions used by the game, events, root counter events,
  interrupt chains (`SysEnqIntRP`) and libetc's `SetCustomExitFromException` dispatcher
  (`ReturnFromException` unwinds back to the interrupt point with a C++ exception).
  Kernel tables live in guest RAM at the real locations (table of tables at 0x100), and the
  runtime follows those pointers because PsyQ's `SetConf` re-allocates the event/thread tables.
  `SetConf` also decodes a `lui/addiu` pair at `[0x474]` to find BIOS variables; a scratch stub
  is provided there.
- **Threads**: the game runs its loading screens in a second BIOS thread. Each guest thread runs
  on its own host thread; only one runs at a time (baton handoff), and the GL context follows.
  The game switches threads from its VSync interrupt by writing the PCB pointer directly, so the
  exception return resumes whatever TCB the PCB points at.
- **CD-ROM** (`cdrom.cpp`): register-level controller (libcd talks to it directly), 1x/2x sector
  timing, 0x800/0x924 sector modes, data FIFO + DMA3, XA-ADPCM decode with file/channel filter,
  CD-DA playback of track 2. The BIOS leaves CD interrupts enabled (IE = 0x1F) at boot; libcd
  relies on that.
- **MDEC**: decode per psx-spx (RLE/quant, scale-table IDCT, YUV->RGB at 4/8/15/24 bit). DMA1 waits
  for decoded data; a new decode command discards unread output.
- **GPU**: GP0/GP1 command processing in `gpu.cpp`; `gpu_gl.cpp` renders into a (1024*S)x(512*S)
  RGBA8 VRAM texture (true colour, mask bit in alpha). Texture pages and CLUTs are sampled in the
  shader from a copy of VRAM refreshed only where drawing touched it. Semi-transparency via
  dual-source blending (B-F mode in two passes). 24-bit display mode decoded in a shader for movies.
- **GTE**: full command set per psx-spx (UNR divide, flags, IR3 quirk, MVMVA far-colour bug).
- **SPU**: 24 voices, Gaussian interpolation (hardware table), ADSR/sweep envelopes, pitch
  modulation, noise, reverb with the 39-tap resampling FIR, CD audio input and capture buffers.
- **Controllers**: SIO0 with a DualShock (digital/analog/config mode, rumble mapping). Memory cards
  are handled at BIOS level (`memcard.cpp`) on standard 128 KB card images; libcard's BIOS patches
  are no-ops (`config/hle.txt`).

## Notable fixes (found by comparing against a reference emulator)

- Jump-table case labels must never become function entries (the data-pointer scan used to pick
  them up from `.rodata`); otherwise the dialogue state machine broke and the Hub cutscene hung.
- Overlays such as HUB1 are entered only through run-time pointers: the gap sweep covers the whole
  overlay image.
- GTE 16-bit data registers (IR0-3, VZ, SZ, OTZ) keep only the low half of the written word. The
  GTE was checked against an independent implementation on millions of random register states.
- Byte writes to DICR (libcd updates the enable byte with `sb`) must not acknowledge pending DMA
  IRQ flags; otherwise MDEC slice interrupts were lost and movies dropped frames.
- Memory cards: libmcrd looks up the "bu" device in the BIOS device table and hooks its
  `firstfile`; async file I/O returns 0 at once and completes by event from a later interrupt.
- BIOS string arguments can live in the scratchpad (the game's stack is there).
- BIOS threads: a retired host thread is told apart from its replacement by a generation number.

## Debugging aids

- `LOG=bios,cd,irq,thread,ovl,interp,dma,gpu,disp,mdec,...` category logging.
- `SHOT_DIR`, `SHOT_EVERY`, `SHOT_FROM`, `FRAMES`, `NO_THROTTLE`, `STOP_AT_FRAME` (SIGTRAP for gdb;
  generated code is built with line tables so host backtraces map to guest labels).
- `WATCH=800bb224,gp+178c` prints RAM words when they change; `RAM_DUMP_AT=frame:file` dumps RAM.
- Bisect build (`-DCMAKE_CXX_FLAGS=-DRECOMP_BISECT`): `BISECT=lo-hi` runs those functions in the
  interpreter (from `BISECT_FROM_SEC`), to find code generation bugs.
- `MDEC_DUMP=1` writes MDEC input per frame; `tools/bs_decode.py` decodes a frame of an STR file
  independently for comparison.
