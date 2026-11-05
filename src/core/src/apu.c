#include <string.h>
#include <raylib.h>
#undef bool

#include "apu.h"
#include "gba_io.h"
#include "dma.h"
#include "helper.h"

static apu_t global_apu;
static AudioStream stream;

static const u8 square_duty[4][8] = {
    { 0, 0, 0, 0, 0, 0, 0, 1 },
    { 1, 0, 0, 0, 0, 0, 0, 1 },
    { 1, 0, 0, 0, 0, 1, 1, 1 },
    { 0, 1, 1, 1, 1, 1, 1, 0 },
};

static
void fifo_reset(u32 index) {
    memset(&global_apu.fifos[index], 0, sizeof(sound_fifo_t));
}

static
void fifo_push(u32 index, u8 value) {
    sound_fifo_t* fifo = &global_apu.fifos[index];

    if (fifo->count == 32)
        return;

    u32 write = (fifo->read + fifo->count) & 31u;
    fifo->bytes[write] = value;
    fifo->count++;
}

static
bool sound_enabled() {
    return (io_read_raw16(SOUND_X) & 0x80u) != 0;
}

static
bool square2_dac_enabled() {
    return (io_read_raw16(SOUND2_L) & 0xf800u) != 0;
}

static
u32 square2_period() {
    u32 frequency = io_read_raw16(SOUND2_H) & 0x07ffu;
    return (2048 - frequency) * 16;
}

static
void update_sound_status() {
    u16 value = io_read_raw16(SOUND_X) & 0x80u;

    if (global_apu.square2.enabled)
        value |= 0x02u;

    io_write_raw16(SOUND_X, value);
}

static
void square2_trigger() {
    u16 envelope = io_read_raw16(SOUND2_L);

    if (global_apu.square2.length == 0)
        global_apu.square2.length = 64;

    global_apu.square2.enabled = sound_enabled() && square2_dac_enabled();
    global_apu.square2.timer = square2_period();
    global_apu.square2.duty_step = 0;
    global_apu.square2.volume = (envelope >> 12) & 15u;

    u32 period = (envelope >> 8) & 7u;
    global_apu.square2.envelope_timer = period != 0 ? period : 8;
    global_apu.square2.envelope_running = true;

    update_sound_status();
}

static
void square2_clock_length() {
    if (!(io_read_raw16(SOUND2_H) & 0x4000u))
        return;

    if (global_apu.square2.length != 0) {
        global_apu.square2.length--;

        if (global_apu.square2.length == 0)
            global_apu.square2.enabled = false;
    }
}

static
void square2_clock_envelope() {
    if (!global_apu.square2.enabled || !global_apu.square2.envelope_running)
        return;

    u16 control = io_read_raw16(SOUND2_L);
    u32 period = (control >> 8) & 7u;

    if (period == 0)
        return;

    if (global_apu.square2.envelope_timer > 0)
        global_apu.square2.envelope_timer--;

    if (global_apu.square2.envelope_timer != 0)
        return;

    global_apu.square2.envelope_timer = period;

    if (control & 0x0800u) {
        if (global_apu.square2.volume < 15)
            global_apu.square2.volume++;
        else
            global_apu.square2.envelope_running = false;
    } else {
        if (global_apu.square2.volume > 0)
            global_apu.square2.volume--;
        else
            global_apu.square2.envelope_running = false;
    }
}

static
void clock_frame_sequencer() {
    if ((global_apu.frame_step & 1u) == 0)
        square2_clock_length();

    if (global_apu.frame_step == 7)
        square2_clock_envelope();

    global_apu.frame_step = (global_apu.frame_step + 1) & 7u;
    update_sound_status();
}

static
int square2_sample() {
    if (!global_apu.square2.enabled || !square2_dac_enabled())
        return 0;

    u32 duty = (io_read_raw16(SOUND2_L) >> 6) & 3u;
    int volume = (int)global_apu.square2.volume;

    return square_duty[duty][global_apu.square2.duty_step] ? volume : -volume;
}

static
void mix_square2(int* left, int* right) {
    u16 routing = io_read_raw16(SOUND_L);
    u32 ratio = io_read_raw16(SOUND_H) & 3u;
    int sample = square2_sample();

    int divisor = ratio == 0 ? 4 : ratio == 1 ? 2 : 1;
    int right_volume = (int)(routing & 7u) + 1;
    int left_volume = (int)((routing >> 4) & 7u) + 1;

    if (routing & (1u << 9))
        *right += sample * right_volume / divisor;

    if (routing & (1u << 13))
        *left += sample * left_volume / divisor;
}

static
i16 dac_sample(int mixed) {
    int bias = (int)(io_read_raw16(SOUND_BIAS) & 0x03ffu);
    int value = bias + mixed;

    if (value < 0)
        value = 0;

    if (value > 1023)
        value = 1023;

    return (i16)((value - 512) * 64);
}

static
void emit_sample() {
    int left = 0;
    int right = 0;

    bool enabled = sound_enabled();

    if (enabled) {
        u16 control = io_read_raw16(SOUND_H);

        for (u32 i = 0; i < 2; i++) {
            u32 volume_bit = i == 0 ? 2 : 3;
            u32 right_bit = i == 0 ? 8 : 12;
            u32 left_bit = right_bit + 1;

            int volume = (control & (1u << volume_bit)) ? 2 : 1;
            int sample = global_apu.fifos[i].sample * volume;

            if (control & (1u << left_bit))
                left += sample;

            if (control & (1u << right_bit))
                right += sample;
        }

        mix_square2(&left, &right);
    }

    if (global_apu.audio_count == AUDIO_CAPACITY) {
        global_apu.audio_read = (global_apu.audio_read + 1) % AUDIO_CAPACITY;
        global_apu.audio_count--;
    }

    u32 write = (global_apu.audio_read + global_apu.audio_count) % AUDIO_CAPACITY;
    global_apu.audio_queue[write][0] = enabled && !global_apu.audio_muted ? dac_sample(left) : 0;
    global_apu.audio_queue[write][1] = enabled && !global_apu.audio_muted ? dac_sample(right) : 0;
    global_apu.audio_count++;
}

void apu_reset() {
    memset(&global_apu, 0, sizeof(apu_t));

    fifo_reset(0);
    fifo_reset(1);

    global_apu.sample_cycles = 0;
    global_apu.frame_cycles = 0;
    global_apu.frame_step = 0;
    global_apu.audio_read = 0;
    global_apu.audio_count = 0;

    io_write_raw16(SOUND2_L, 0);
    io_write_raw16(SOUND2_H, 0);
    io_write_raw16(SOUND_L, 0);
    io_write_raw16(SOUND_H, 0);
    io_write_raw16(SOUND_X, 0);
    io_write_raw16(SOUND_BIAS, 0x0200);
}

void apu_enable_audio() {
    InitAudioDevice();
    if (!IsAudioDeviceReady())
        return;

    SetAudioStreamBufferSizeDefault((int)AUDIO_BLOCK);
    stream = LoadAudioStream(AUDIO_RATE, 16, 2);

    if (!IsAudioStreamValid(stream)) {
        CloseAudioDevice();
        return;
    }

    global_apu.audio_read = 0;
    global_apu.audio_count = 0;
    global_apu.audio_ready = true;
    PlayAudioStream(stream);
}

void apu_step(u32 cycles) {
    while (cycles != 0u) {
        bool enabled = sound_enabled();
        u32 step = SAMPLE_CYCLES - global_apu.sample_cycles;

        if (enabled) {
            u32 remaining = FRAME_CYCLES - global_apu.frame_cycles;
            if (remaining < step)
                step = remaining;
        }

        if (global_apu.square2.enabled && global_apu.square2.timer < step)
            step = global_apu.square2.timer;
        if (cycles < step)
            step = cycles;

        global_apu.sample_cycles += step;
        if (enabled)
            global_apu.frame_cycles += step;
        if (global_apu.square2.enabled)
            global_apu.square2.timer -= step;
        cycles -= step;

        if (global_apu.square2.enabled && global_apu.square2.timer == 0) {
            global_apu.square2.duty_step = (global_apu.square2.duty_step + 1) & 7u;
            global_apu.square2.timer = square2_period();
        }

        if (enabled && global_apu.frame_cycles == FRAME_CYCLES) {
            global_apu.frame_cycles = 0;
            clock_frame_sequencer();
        }

        if (global_apu.sample_cycles == SAMPLE_CYCLES) {
            global_apu.sample_cycles = 0;
            emit_sample();
        }
    }
}

u8 apu_read8(u32 offset, u8 flags) {
    flags;
    u32 aligned = offset & ~1u;
    u16 value = io_read_raw16(aligned);

    switch (aligned) {
        case SOUND2_L:
            value &= 0xFFC0u;
            break;
        case SOUND2_H:
            value &= 0x4000u;
            break;
        case SOUND2_L + 2:
        case SOUND2_H + 2:
            value = 0;
            break;
    }

    return (offset & 1u) ? (u8)(value >> 8) : (u8)value;
}

void apu_write8(u32 offset, u8 value, u8 flags) {
    if (MASK_ANY_SET(flags, MEM_ACCESS_NOSIDE_EFFECT))
        return;

    if (offset >= FIFO_A && offset < FIFO_B + 4) {
        fifo_push((offset - FIFO_A) / 4, value);
        return;
    }

    switch (offset) {
        case SOUND2_L: {
            if (!sound_enabled())
                return;

            u16 old = io_read_raw16(SOUND2_L);
            io_write_raw16(SOUND2_L, (u16)((old & 0xff00u) | value));
            global_apu.square2.length = 64 - (value & 63u);
            break;
        }

        case SOUND2_L + 1: {
            if (!sound_enabled())
                return;

            u16 old = io_read_raw16(SOUND2_L);
            io_write_raw16(SOUND2_L, (u16)((old & 0x00ffu) | ((u16)value << 8)));

            if (!square2_dac_enabled())
                global_apu.square2.enabled = false;

            update_sound_status();
            break;
        }
        case SOUND2_H: {
            if (!sound_enabled())
                return;

            u16 old = io_read_raw16(SOUND2_H);
            io_write_raw16(SOUND2_H, (u16)((old & 0xff00u) | value));
            break;
        }
        case SOUND2_H + 1: {
            if (!sound_enabled())
                return;

            u16 old = io_read_raw16(SOUND2_H);
            io_write_raw16(SOUND2_H, (u16)((old & 0x00ffu) | ((u16)(value & 0x47u) << 8)));

            if (value & 0x80u)
                square2_trigger();
            break;
        }
        case SOUND_L: {
            if (!sound_enabled())
                return;

            u16 old = io_read_raw16(SOUND_L);
            io_write_raw16(SOUND_L, (u16)((old & 0xff00u) | (value & 0x77u)));
            break;
        }
        case SOUND_L + 1: {
            if (!sound_enabled())
                return;

            u16 old = io_read_raw16(SOUND_L);
            io_write_raw16(SOUND_L, (u16)((old & 0x00ffu) | ((u16)value << 8)));
            break;
        }
        case SOUND_H: {
            u16 old = io_read_raw16(SOUND_H);
            io_write_raw16(SOUND_H, (u16)((old & 0xff00u) | (value & 0x0fu)));
            break;
        }
        case SOUND_H + 1: {
            if (value & 0x08u)
                fifo_reset(0);
            if (value & 0x80u)
                fifo_reset(1);

            u16 old = io_read_raw16(SOUND_H);
            io_write_raw16(SOUND_H, (u16)((old & 0x00ffu) | ((u16)(value & 0x77u) << 8)));
            break;
        }
        case SOUND_X: {
            bool was_enabled = sound_enabled();
            bool enabled = (value & 0x80u) != 0;

            if (!enabled) {
                memset(&global_apu.square2, 0, sizeof(square_channel_t));
                io_write_raw16(SOUND2_L, 0);
                io_write_raw16(SOUND2_H, 0);
                io_write_raw16(SOUND_L, 0);
                global_apu.frame_cycles = 0;
                global_apu.frame_step = 0;
            } else if (!was_enabled) {
                global_apu.frame_cycles = 0;
                global_apu.frame_step = 0;
            }

            io_write_raw16(SOUND_X, enabled ? 0x80 : 0);
            update_sound_status();
            break;
        }
        case SOUND_X + 1:
            break;
        case SOUND_BIAS: {
            u16 old = io_read_raw16(SOUND_BIAS);
            io_write_raw16(SOUND_BIAS, (u16)((old & 0xff00u) | (value & 0xfeu)));
            break;
        }
        case SOUND_BIAS + 1: {
            u16 old = io_read_raw16(SOUND_BIAS);
            io_write_raw16(SOUND_BIAS, (u16)((old & 0x00ffu) | ((u16)(value & 0xc3u) << 8)));
            break;
        }
        case SOUND2_L + 2:
        case SOUND2_L + 3:
        case SOUND2_H + 2:
        case SOUND2_H + 3:
            break;
    }
}

void apu_timer_overflow(u32 timer) {
    if (timer > 1u || !sound_enabled())
        return;

    u16 control = io_read_raw16(SOUND_H);

    for (u32 i = 0; i < 2; i++) {
        u32 timer_bit = i == 0 ? 10 : 14;

        if (((control >> timer_bit) & 1u) != timer)
            continue;

        sound_fifo_t* fifo = &global_apu.fifos[i];

        if (fifo->count != 0) {
            u8 value = fifo->bytes[fifo->read];
            fifo->sample = value < 128 ? (int)value : (int)value - 256;
            fifo->read = (fifo->read + 1) & 31u;
            fifo->count--;
        }

        if (fifo->count <= 16)
            dma_on_fifo_request(i == 0u ? 0x040000A0u : 0x040000a4u);
    }
}

u32 apu_cycles_until_sample() {
    return SAMPLE_CYCLES - global_apu.sample_cycles;
}

void apu_audio_pump() {
    if (!global_apu.audio_ready)
        return;

    if (global_apu.audio_count > AUDIO_BLOCK * 2) {
        u32 discard = global_apu.audio_count - AUDIO_BLOCK * 2;
        global_apu.audio_read = (global_apu.audio_read + discard) % AUDIO_CAPACITY;
        global_apu.audio_count -= discard;
    }

    while (IsAudioStreamProcessed(stream) && global_apu.audio_count >= AUDIO_BLOCK) {
        i16 block[AUDIO_BLOCK * 2];

        for (u32 i = 0; i < AUDIO_BLOCK; i++) {
            block[i * 2] = global_apu.audio_queue[global_apu.audio_read][0];
            block[i * 2 + 1] = global_apu.audio_queue[global_apu.audio_read][1];
            global_apu.audio_read = (global_apu.audio_read + 1) % AUDIO_CAPACITY;
        }

        global_apu.audio_count -= AUDIO_BLOCK;
        UpdateAudioStream(stream, block, (int)AUDIO_BLOCK);
    }
}

void apu_set_muted(bool muted) {
    global_apu.audio_muted = muted;
    global_apu.audio_read = 0;
    global_apu.audio_count = 0;
}

void apu_audio_close() {
    if (!global_apu.audio_ready)
        return;

    StopAudioStream(stream);
    UnloadAudioStream(stream);
    CloseAudioDevice();
    global_apu.audio_ready = false;
}