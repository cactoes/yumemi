//==========================================
/// @file       apu.h
/// @brief      
//==========================================

#pragma once

#ifndef APU_H
#define APU_H

#define SOUND1_L    0x060u
#define SOUND1_H    0x062u
#define SOUND1_X    0x064u
#define SOUND2_L    0x068u
#define SOUND2_H    0x06cu
#define SOUND_L     0x080u
#define SOUND_H     0x082u
#define SOUND_X     0x084u
#define SOUND_BIAS  0x088u
#define FIFO_A      0x0a0u
#define FIFO_B      0x0a4u

#define FRAME_CYCLES    32768u
#define SAMPLE_CYCLES   512u
#define SOURCE_RATE     32768u
#define AUDIO_RATE      48000u
#define AUDIO_BLOCK     512u
#define AUDIO_CAPACITY  8192u
#define AUDIO_TARGET    1536u

#define FIFO_COUNT 2
#define AUDIO_QUEUE_COUNT 2
#define SQUARE_COUNT 2

#include "typedef.h"

typedef struct __sound_fifo {
    u8 bytes[32];
    u32 read;
    u32 count;
    int sample;
} sound_fifo_t;

typedef struct {
    bool enabled;
    bool envelope_running;
    u32 timer;
    u32 duty_step;
    u32 length;
    u32 volume;
    u32 envelope_timer;
} square_channel_t;

typedef struct __sound_sweep {
    u32 shadow_frequency;
    u32 timer;
    bool enabled;
    bool negate_used;
} sound_sweep_t;

typedef struct __apu {
    sound_fifo_t fifos[FIFO_COUNT];
    square_channel_t square1;
    square_channel_t square2;
    sound_sweep_t sweep;
    u32 frame_cycles;
    u32 frame_step;
    u32 sample_cycles;
} apu_t;

void apu_reset();
void apu_enable_audio();
void apu_step(u32 cycles);
u8 apu_read8(u32 offset, u8 flags);
void apu_write8(u32 offset, u8 value, u8 flags);
void apu_timer_overflow(u32 timer);
u32 apu_cycles_until_sample();
void apu_audio_pump();
void apu_set_muted(bool muted, bool force);
void apu_audio_close();
bool apu_is_ready();

#endif // APU_H