// PlayStation hardware: scheduler, interrupt controller, timers, DMA, and the device interfaces.
#pragma once
#include <cstdint>
#include <string>
#include <vector>
#include "recomp.h"

// CPU clock and video timing (NTSC)
static const int64_t CPU_HZ = 33868800;
static const int64_t CYCLES_PER_LINE_FP = 2152916;   // 2152.916 cycles per scanline (x1000)
static const int LINES_PER_FRAME = 263;

// ---------------------------------------------------------------- scheduler
enum Event {
    EV_VBLANK, EV_TIMER0, EV_TIMER1, EV_TIMER2, EV_CDROM, EV_CDROM_READ, EV_CDROM_AUDIO,
    EV_DMA0, EV_DMA1, EV_DMA2, EV_DMA3, EV_DMA4, EV_DMA5, EV_DMA6,
    EV_SIO0, EV_SPU, EV_GPU, EV_COUNT
};
void sched_at(int ev, int64_t when);
void sched_in(int ev, int64_t cycles);
void sched_cancel(int ev);
bool sched_active(int ev);

// ---------------------------------------------------------------- interrupts
enum Irq { IRQ_VBLANK = 0, IRQ_GPU = 1, IRQ_CDROM = 2, IRQ_DMA = 3, IRQ_TIMER0 = 4, IRQ_TIMER1 = 5,
           IRQ_TIMER2 = 6, IRQ_SIO0 = 7, IRQ_SIO1 = 8, IRQ_SPU = 9, IRQ_LIGHTPEN = 10 };
void irq_raise(int irq);
bool irq_pending();          // I_STAT & I_MASK
void irq_update();           // re-evaluate whether an interrupt can be taken now
uint32_t irq_stat();
uint32_t irq_mask();

void hw_init();
void hw_frame_pacing();      // called at vblank: throttle to real time
extern bool g_in_exception;  // HLE exception handler running (interrupts not taken)

// ---------------------------------------------------------------- devices
void gpu_init();
uint32_t gpu_read(int reg);            // 0 GPUREAD, 1 GPUSTAT
void gpu_write(int reg, uint32_t v);   // 0 GP0, 1 GP1
void gpu_vblank();
bool gpu_interlaced_odd();
int gpu_dot_divider();                 // video clock divider for the current horizontal resolution

bool cd_init(const char* cue_or_dir);
bool cd_read_file(const char* name, std::vector<uint8_t>& out);
bool cd_init_pack(const char* path, std::string* err);                         // assets.dat
bool cd_make_pack(const char* out_path, void (*progress)(int, int), std::string* err);  // .cue/.bin -> assets.dat
void cd_close_image();
uint8_t cd_read(int reg);
void cd_write(int reg, uint8_t v);
uint32_t cd_dma_read();
void cd_event(int ev);

void spu_init();
uint16_t spu_read(uint32_t off);
void spu_write(uint32_t off, uint16_t v);
void spu_dma_write(uint32_t w);
uint32_t spu_dma_read();
void spu_event();
void spu_cd_audio(const int16_t* lr, int frames, int rate);   // CD-DA / XA audio into the SPU mixer

void mdec_init();
uint32_t mdec_read(int reg);
void mdec_write(int reg, uint32_t v);
void mdec_dma_in(uint32_t w);
uint32_t mdec_dma_out();
bool mdec_out_ready();

void sio_init();
uint32_t sio_read(uint32_t off, int size);
void sio_write(uint32_t off, uint32_t v, int size);
void sio_event();

void dma_write(uint32_t off, uint32_t v);
uint32_t dma_read(uint32_t off);
void dma_event(int ch);

uint32_t timer_read(uint32_t off);
void timer_write(uint32_t off, uint32_t v);
void timer_event(int t);
