#pragma once
// Vibration for Einhander, which has none of its own: game events are recognised by the sound
// effect the game starts (each effect is a fixed ADPCM sample, identified by a hash of its first
// 64 bytes in SPU RAM, so it does not matter where a stage's sound bank puts it) and turned into
// motor pulses.
#include <cstdint>
void rumble_sample(uint32_t hash, int volume);   // a voice was keyed on (volume 0..0x3fff)
void rumble_frame();                              // once per frame: drive the motors
