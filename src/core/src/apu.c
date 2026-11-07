#include <string.h>
#include <math.h>
#include <threads.h>
#include <raylib.h>
#undef bool

#include "apu.h"
#include "gba_io.h"
#include "dma.h"
#include "helper.h"

#define AUDIO_FADE_FRAMES 240u
#define AUDIO_MAX_CORRECTION 0.005

static apu_t global_apu;
static AudioStream stream;

typedef struct __host_audio {
    mtx_t mutex;
    float queue[AUDIO_CAPACITY][AUDIO_QUEUE_COUNT];
    u32 read;
    u32 count;
    u64 underruns;
    u64 dropped_frames;
    bool ready;
    bool muted;
    bool force_muted;
    bool buffering;
    float gain;
    float last[AUDIO_QUEUE_COUNT];
    double correction;
} host_audio_t;

typedef struct __audio_processing {
    double sum[AUDIO_QUEUE_COUNT];
    float previous_input[AUDIO_QUEUE_COUNT];
    float highpass[AUDIO_QUEUE_COUNT];
    float lowpass[AUDIO_QUEUE_COUNT];
    float previous_sample[AUDIO_QUEUE_COUNT];
    float highpass_decay;
    float lowpass_amount;
    bool has_previous;
    double resample_phase;
    double correction;
    double queue_error;
    double last_clock_update;
} audio_processing_t;

static host_audio_t host_audio;
static audio_processing_t processing;

static const u8 square_duty[4][8] = {
    { 0, 0, 0, 0, 0, 0, 0, 1 },
    { 1, 0, 0, 0, 0, 0, 0, 1 },
    { 1, 0, 0, 0, 0, 1, 1, 1 },
    { 0, 1, 1, 1, 1, 1, 1, 0 },
};

static
square_channel_t* get_square(u32 index) {
    return index == 0 ? &global_apu.square1 : &global_apu.square2;
}

static
u32 square_envelope_reg(u32 index) {
    return index == 0 ? SOUND1_H : SOUND2_L;
}

static
u32 square_frequency_reg(u32 index) {
    return index == 0 ? SOUND1_X : SOUND2_H;
}

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
bool square_dac_enabled(u32 index) {
    return (io_read_raw16(square_envelope_reg(index)) & 0xf800u) != 0;
}

static
u32 square_period(u32 index) {
    u32 frequency = io_read_raw16(square_frequency_reg(index)) & 0x07ffu;
    return (2048 - frequency) * 16;
}

static
void update_sound_status() {
    u16 value = io_read_raw16(SOUND_X) & 0x80u;

    if (global_apu.square1.enabled)
        value |= 0x01u;
    if (global_apu.square2.enabled)
        value |= 0x02u;

    io_write_raw16(SOUND_X, value);
}

static
u32 sweep_calculate() {
    sound_sweep_t* sweep = &global_apu.sweep;
    u16 control = io_read_raw16(SOUND1_L);
    u32 shift = control & 7u;
    u32 change = sweep->shadow_frequency >> shift;
    u32 frequency;

    if (control & 0x08u) {
        frequency = sweep->shadow_frequency - change;
        sweep->negate_used = true;
    } else {
        frequency = sweep->shadow_frequency + change;
    }

    if (frequency > 2047)
        global_apu.square1.enabled = false;

    return frequency;
}

static
void sweep_trigger() {
    sound_sweep_t* sweep = &global_apu.sweep;
    u16 control = io_read_raw16(SOUND1_L);
    u32 period = (control >> 4) & 7u;
    u32 shift = control & 7u;

    sweep->shadow_frequency = io_read_raw16(SOUND1_X) & 0x07ffu;
    sweep->timer = period != 0 ? period : 8;
    sweep->enabled = period != 0 || shift != 0;
    sweep->negate_used = false;

    if (shift != 0)
        (void)sweep_calculate();
}

static
void sweep_clock() {
    sound_sweep_t* sweep = &global_apu.sweep;

    if (!global_apu.square1.enabled)
        return;

    if (sweep->timer > 0)
        sweep->timer--;
    if (sweep->timer != 0)
        return;

    u16 control = io_read_raw16(SOUND1_L);
    u32 period = (control >> 4) & 7u;
    u32 shift = control & 7u;
    sweep->timer = period != 0 ? period : 8;

    if (!sweep->enabled || period == 0)
        return;

    u32 frequency = sweep_calculate();
    if (frequency > 2047 || shift == 0)
        return;

    sweep->shadow_frequency = frequency;
    u16 old = io_read_raw16(SOUND1_X);
    io_write_raw16(SOUND1_X, (u16)((old & 0x4000u) | frequency));
    (void)sweep_calculate();
}

static
void square_trigger(u32 index) {
    square_channel_t* square = get_square(index);
    u16 envelope = io_read_raw16(square_envelope_reg(index));

    if (square->length == 0)
        square->length = 64;

    square->enabled = sound_enabled() && square_dac_enabled(index);
    square->timer = square_period(index);
    square->duty_step = 0;
    square->volume = (envelope >> 12) & 15u;

    u32 period = (envelope >> 8) & 7u;
    square->envelope_timer = period != 0 ? period : 8;
    square->envelope_running = true;

    if (index == 0)
        sweep_trigger();

    update_sound_status();
}

static
void square_clock_length(u32 index) {
    square_channel_t* square = get_square(index);

    if (!(io_read_raw16(square_frequency_reg(index)) & 0x4000u))
        return;

    if (square->length != 0) {
        square->length--;
        if (square->length == 0)
            square->enabled = false;
    }
}

static
void square_clock_envelope(u32 index) {
    square_channel_t* square = get_square(index);

    if (!square->enabled || !square->envelope_running)
        return;

    u16 control = io_read_raw16(square_envelope_reg(index));
    u32 period = (control >> 8) & 7u;
    if (period == 0)
        return;

    if (square->envelope_timer > 0)
        square->envelope_timer--;
    if (square->envelope_timer != 0)
        return;

    square->envelope_timer = period;

    if (control & 0x0800u) {
        if (square->volume < 15)
            square->volume++;
        else
            square->envelope_running = false;
    } else {
        if (square->volume > 0)
            square->volume--;
        else
            square->envelope_running = false;
    }
}

static
void clock_frame_sequencer() {
    if ((global_apu.frame_step & 1u) == 0) {
        for (u32 i = 0; i < SQUARE_COUNT; i++)
            square_clock_length(i);
    }

    if (global_apu.frame_step == 2 || global_apu.frame_step == 6)
        sweep_clock();

    if (global_apu.frame_step == 7) {
        for (u32 i = 0; i < SQUARE_COUNT; i++)
            square_clock_envelope(i);
    }

    global_apu.frame_step = (global_apu.frame_step + 1) & 7u;
    update_sound_status();
}

static
int square_sample(u32 index) {
    square_channel_t* square = get_square(index);

    if (!square->enabled || !square_dac_enabled(index))
        return 0;

    u32 duty = (io_read_raw16(square_envelope_reg(index)) >> 6) & 3u;
    int volume = (int)square->volume;
    return square_duty[duty][square->duty_step] ? volume : -volume;
}

static
void mix_squares(int* left, int* right) {
    u16 routing = io_read_raw16(SOUND_L);
    u32 ratio = io_read_raw16(SOUND_H) & 3u;
    int divisor = ratio == 0 ? 4 : ratio == 1 ? 2 : 1;
    int right_volume = (int)(routing & 7u) + 1;
    int left_volume = (int)((routing >> 4) & 7u) + 1;

    for (u32 i = 0; i < SQUARE_COUNT; i++) {
        int sample = square_sample(i);
        if (routing & (1u << (8 + i)))
            *right += sample * right_volume / divisor;
        if (routing & (1u << (12 + i)))
            *left += sample * left_volume / divisor;
    }
}

static
void square_write8(u32 index, u32 offset, u8 value) {
    if (!sound_enabled())
        return;

    square_channel_t* square = get_square(index);
    u32 envelope_reg = square_envelope_reg(index);
    u32 frequency_reg = square_frequency_reg(index);

    if (offset == envelope_reg) {
        u16 old = io_read_raw16(envelope_reg);
        io_write_raw16(envelope_reg, (u16)((old & 0xff00u) | value));
        square->length = 64 - (value & 63u);
    } else if (offset == envelope_reg + 1) {
        u16 old = io_read_raw16(envelope_reg);
        io_write_raw16(envelope_reg, (u16)((old & 0x00ffu) | ((u16)value << 8)));
        if (!square_dac_enabled(index))
            square->enabled = false;
        update_sound_status();
    } else if (offset == frequency_reg) {
        u16 old = io_read_raw16(frequency_reg);
        io_write_raw16(frequency_reg, (u16)((old & 0xff00u) | value));
    } else if (offset == frequency_reg + 1) {
        u16 old = io_read_raw16(frequency_reg);
        io_write_raw16(frequency_reg, (u16)((old & 0x00ffu) | ((u16)(value & 0x47u) << 8)));
        if (value & 0x80u)
            square_trigger(index);
    }
}

static
float dac_sample(int mixed) {
    int bias = (int)(io_read_raw16(SOUND_BIAS) & 0x03ffu);
    int value = bias + mixed;
    if (value < 0)
        value = 0;
    if (value > 1023)
        value = 1023;
    return (float)(value - 512) / 512.0f;
}

static
void reset_processing() {
    memset(&processing, 0, sizeof(audio_processing_t));
    processing.highpass_decay = expf(-6.2831853f * 20.0f / (float)SOURCE_RATE);
    processing.lowpass_amount = 1.0f - expf(-6.2831853f * 10000.0f / (float)SOURCE_RATE);
}

static
void clear_host_queue() {
    if (!host_audio.ready)
        return;

    mtx_lock(&host_audio.mutex);
    host_audio.read = 0;
    host_audio.count = 0;
    host_audio.buffering = true;
    host_audio.gain = 0;
    host_audio.correction = 0;
    mtx_unlock(&host_audio.mutex);
}

static
void queue_sample(float left, float right) {
    if (!host_audio.ready)
        return;

    mtx_lock(&host_audio.mutex);

    if (!host_audio.muted) {
        if (host_audio.count < AUDIO_CAPACITY) {
            u32 write = (host_audio.read + host_audio.count) % AUDIO_CAPACITY;
            host_audio.queue[write][0] = left;
            host_audio.queue[write][1] = right;
            host_audio.count++;
        } else {
            host_audio.dropped_frames++;
        }
    }

    mtx_unlock(&host_audio.mutex);
}

static
float filter_sample(u32 channel, float sample) {
    float highpass = processing.highpass_decay *
        (processing.highpass[channel] + sample - processing.previous_input[channel]);

    processing.previous_input[channel] = sample;
    processing.highpass[channel] = highpass;
    processing.lowpass[channel] += processing.lowpass_amount *
        (highpass - processing.lowpass[channel]);

    return processing.lowpass[channel];
}

static
void emit_sample() {
    float sample[AUDIO_QUEUE_COUNT];

    for (u32 i = 0; i < AUDIO_QUEUE_COUNT; i++) {
        float average = (float)(processing.sum[i] / (double)SAMPLE_CYCLES);
        processing.sum[i] = 0;
        sample[i] = filter_sample(i, average);
    }

    if (!processing.has_previous) {
        processing.previous_sample[0] = sample[0];
        processing.previous_sample[1] = sample[1];
        processing.has_previous = true;
    }

    double output_rate = (double)AUDIO_RATE * (1.0 + processing.correction);
    processing.resample_phase += output_rate;

    while (processing.resample_phase >= (double)SOURCE_RATE) {
        processing.resample_phase -= (double)SOURCE_RATE;
        float fraction = (float)(1.0 - processing.resample_phase / output_rate);
        float left = processing.previous_sample[0] +
            (sample[0] - processing.previous_sample[0]) * fraction;
        float right = processing.previous_sample[1] +
            (sample[1] - processing.previous_sample[1]) * fraction;
        queue_sample(left, right);
    }

    processing.previous_sample[0] = sample[0];
    processing.previous_sample[1] = sample[1];
}

static
void accumulate_sample(u32 cycles) {
    int left = 0;
    int right = 0;

    if (sound_enabled()) {
        u16 control = io_read_raw16(SOUND_H);

        for (u32 i = 0; i < FIFO_COUNT; i++) {
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

        mix_squares(&left, &right);
        processing.sum[0] += (double)dac_sample(left) * cycles;
        processing.sum[1] += (double)dac_sample(right) * cycles;
    }
}

static
void audio_callback(void* buffer, unsigned int frames) {
    float* output = (float*)buffer;
    bool starved = false;

    mtx_lock(&host_audio.mutex);

    if (!host_audio.muted && host_audio.buffering &&
        host_audio.count >= AUDIO_TARGET) {
        host_audio.buffering = false;
        host_audio.gain = 0;
    }

    for (u32 i = 0; i < frames; i++) {
        bool playing = !host_audio.muted && !host_audio.buffering;

        if (playing && host_audio.count == 0) {
            host_audio.buffering = true;
            host_audio.gain = 0;
            starved = true;
            playing = false;
        }

        if (playing) {
            if (host_audio.gain < 1.0f) {
                host_audio.gain += 1.0f / (float)AUDIO_FADE_FRAMES;
                if (host_audio.gain > 1.0f)
                    host_audio.gain = 1.0f;
            }

            for (u32 channel = 0; channel < AUDIO_QUEUE_COUNT; channel++) {
                float value = host_audio.queue[host_audio.read][channel] * host_audio.gain;
                if (value > 1.0f)
                    value = 1.0f;
                if (value < -1.0f)
                    value = -1.0f;
                host_audio.last[channel] = value;
            }

            host_audio.read = (host_audio.read + 1) % AUDIO_CAPACITY;
            host_audio.count--;
        } else {
            for (u32 channel = 0; channel < AUDIO_QUEUE_COUNT; channel++) {
                host_audio.last[channel] *= 0.98f;
                if (fabsf(host_audio.last[channel]) < 0.00001f)
                    host_audio.last[channel] = 0;
            }
        }

        output[i * 2] = host_audio.last[0];
        output[i * 2 + 1] = host_audio.last[1];
    }

    if (starved)
        host_audio.underruns++;

    mtx_unlock(&host_audio.mutex);
}

void apu_reset() {
    memset(&global_apu, 0, sizeof(apu_t));
    reset_processing();
    clear_host_queue();

    for (u32 offset = SOUND1_L; offset <= SOUND2_H + 2; offset += 2)
        io_write_raw16(offset, 0);

    io_write_raw16(SOUND_L, 0);
    io_write_raw16(SOUND_H, 0);
    io_write_raw16(SOUND_X, 0);
    io_write_raw16(SOUND_BIAS, 0x0200);
}

void apu_enable_audio() {
    if (host_audio.ready)
        return;

    if (mtx_init(&host_audio.mutex, mtx_plain) != thrd_success)
        return;

    InitAudioDevice();
    if (!IsAudioDeviceReady()) {
        mtx_destroy(&host_audio.mutex);
        return;
    }

    SetAudioStreamBufferSizeDefault((int)AUDIO_BLOCK);
    stream = LoadAudioStream(AUDIO_RATE, 32, 2);

    if (!IsAudioStreamValid(stream)) {
        CloseAudioDevice();
        mtx_destroy(&host_audio.mutex);
        return;
    }

    reset_processing();
    host_audio.read = 0;
    host_audio.count = 0;
    host_audio.underruns = 0;
    host_audio.dropped_frames = 0;
    host_audio.buffering = true;
    host_audio.gain = 0;
    host_audio.last[0] = 0;
    host_audio.last[1] = 0;
    host_audio.correction = 0;
    host_audio.ready = true;

    SetAudioStreamCallback(stream, audio_callback);
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

        for (u32 i = 0; i < SQUARE_COUNT; i++) {
            square_channel_t* square = get_square(i);
            if (square->enabled && square->timer < step)
                step = square->timer;
        }

        if (cycles < step)
            step = cycles;

        accumulate_sample(step);
        global_apu.sample_cycles += step;
        if (enabled)
            global_apu.frame_cycles += step;

        for (u32 i = 0; i < SQUARE_COUNT; i++) {
            square_channel_t* square = get_square(i);
            if (square->enabled)
                square->timer -= step;
        }

        cycles -= step;

        for (u32 i = 0; i < SQUARE_COUNT; i++) {
            square_channel_t* square = get_square(i);
            if (square->enabled && square->timer == 0) {
                square->duty_step = (square->duty_step + 1) & 7u;
                square->timer = square_period(i);
            }
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
    (void)flags;
    u32 aligned = offset & ~1u;
    u16 value = io_read_raw16(aligned);

    switch (aligned) {
        case SOUND1_L:
            value &= 0x007fu;
            break;
        case SOUND1_H:
        case SOUND2_L:
            value &= 0xffc0u;
            break;
        case SOUND1_X:
        case SOUND2_H:
            value &= 0x4000u;
            break;
        case SOUND1_X + 2:
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

    if (offset >= SOUND1_H && offset <= SOUND1_X + 1) {
        square_write8(0, offset, value);
        return;
    }

    if ((offset >= SOUND2_L && offset <= SOUND2_L + 1) ||
        (offset >= SOUND2_H && offset <= SOUND2_H + 1)) {
        square_write8(1, offset, value);
        return;
    }

    switch (offset) {
        case SOUND1_L: {
            if (!sound_enabled())
                return;
            if (global_apu.sweep.negate_used && !(value & 0x08u))
                global_apu.square1.enabled = false;
            io_write_raw16(SOUND1_L, (u16)(value & 0x7fu));
            update_sound_status();
            break;
        }
        case SOUND1_L + 1:
            break;
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
                memset(&global_apu.square1, 0, sizeof(square_channel_t));
                memset(&global_apu.square2, 0, sizeof(square_channel_t));
                memset(&global_apu.sweep, 0, sizeof(sound_sweep_t));
                for (u32 reg = SOUND1_L; reg <= SOUND2_H + 2; reg += 2)
                    io_write_raw16(reg, 0);
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
        case SOUND1_X + 2:
        case SOUND1_X + 3:
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
    for (u32 i = 0; i < FIFO_COUNT; i++) {
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
            dma_on_fifo_request(i == 0 ? 0x040000a0u : 0x040000a4u);
    }
}

u32 apu_cycles_until_sample() {
    return SAMPLE_CYCLES - global_apu.sample_cycles;
}

void apu_audio_pump() {
    if (!host_audio.ready)
        return;

    double now = GetTime();
    if (now - processing.last_clock_update < 0.1)
        return;
    processing.last_clock_update = now;

    mtx_lock(&host_audio.mutex);
    u32 queued = host_audio.count;
    bool buffering = host_audio.buffering || host_audio.muted;
    mtx_unlock(&host_audio.mutex);

    if (buffering) {
        processing.correction = 0;
        processing.queue_error = 0;
    } else {
        double error = (double)queued - (double)AUDIO_TARGET;
        processing.queue_error += (error - processing.queue_error) * 0.1;
        double correction = -processing.queue_error * 0.000002;

        if (correction > AUDIO_MAX_CORRECTION)
            correction = AUDIO_MAX_CORRECTION;
        if (correction < -AUDIO_MAX_CORRECTION)
            correction = -AUDIO_MAX_CORRECTION;

        processing.correction += (correction - processing.correction) * 0.1;
    }

    mtx_lock(&host_audio.mutex);
    host_audio.correction = processing.correction;
    mtx_unlock(&host_audio.mutex);
}

void apu_set_muted(bool muted, bool force) {
    if (!host_audio.ready) {
        if (force) {
            host_audio.force_muted = true;
            host_audio.muted = muted;
        } else if (!host_audio.force_muted) {
            host_audio.muted = muted;
        }
        return;
    }

    mtx_lock(&host_audio.mutex);

    if (!force && host_audio.force_muted) {
        mtx_unlock(&host_audio.mutex);
        return;
    }

    if (force)
        host_audio.force_muted = true;

    bool changed = host_audio.muted != muted;
    host_audio.muted = muted;
    mtx_unlock(&host_audio.mutex);

    if (changed) {
        processing.correction = 0;
        processing.queue_error = 0;
        processing.resample_phase = 0;
        processing.has_previous = false;
        clear_host_queue();
    }
}

void apu_audio_close() {
    if (!host_audio.ready)
        return;

    StopAudioStream(stream);
    UnloadAudioStream(stream);
    CloseAudioDevice();
    host_audio.ready = false;
    mtx_destroy(&host_audio.mutex);
}

bool apu_is_ready() {
    return host_audio.ready;
}