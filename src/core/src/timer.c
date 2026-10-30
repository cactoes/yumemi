#include <stdlib.h>
#include <string.h>

#include "timer.h"
#include "gba_io.h"

static const u32 timer_shifts[TIMER_COUNT] = { 0, 6, 8, 10 };
static timer_state_t global_timer;

void timer_reset() {
    memset(&global_timer, 0, sizeof(global_timer));
}

static
timer_base_t* get_timer(u32 index) {
    return &global_timer.timers[index & (TIMER_COUNT - 1)];
}

static
void timer_increment(u32 index, u32 ticks) {
    timer_base_t* timer = get_timer(index);

    u32 value = timer->counter + ticks;

    if (value <= 0xffff) {
        timer->counter = (u16)value;
        return;
    }

    u32 period = 0x10000u - timer->reload;
    u32 over = value - 0x10000u;
    u32 overflows = 1 + over / period;

    timer->counter = (u16)(timer->reload + over % period);

    if (timer->control & TIMER_CONTROL_IRQ_ENABLE)
        io_request_irq(IRQ_TIMER(index));

    if (index < (TIMER_COUNT - 1)) {
        timer_base_t* next = get_timer(index + 1);
        if ((next->control & TIMER_CONTROL_ENABLE) && (next->control & TIMER_CONTROL_CASCADE))
            timer_increment(index + 1, overflows);
    }
}

void timer_step(u32 cycles) {
    for (u32 i = 0; i < TIMER_COUNT; i++) {
        timer_base_t* timer = get_timer(i);

        if (!(timer->control & TIMER_CONTROL_ENABLE))
            continue;

        if (i > 0 && (timer->control & TIMER_CONTROL_CASCADE))
            continue;

        u32 shift = timer_shifts[timer->control & TIMER_CONTROL_PRESCALER_MASK];

        timer->accumulator += cycles;
        u32 ticks = timer->accumulator >> shift;
        timer->accumulator &= (1u << shift) - 1;

        if (ticks)
            timer_increment(i, ticks);
    }
}

u16 timer_read_low(u32 index) {
    return get_timer(index)->counter;
}

u16 timer_read_reload(u32 index) {
    return get_timer(index)->reload;
}

u16 timer_read_high(u32 index) {
    return get_timer(index)->control;
}

void timer_write_low(u32 index, u16 value) {
    get_timer(index)->reload = value;
}

void timer_write_high(u32 index, u16 value) {
    timer_base_t* timer = get_timer(index);

    bool was_enabled = (timer->control & TIMER_CONTROL_ENABLE) != 0;
    timer->control = value & TIMER_CONTROL_MASK;

    if (!was_enabled && (value & TIMER_CONTROL_ENABLE)) {
        timer->counter = timer->reload;
        timer->accumulator = 0;
    }
}