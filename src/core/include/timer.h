//==========================================
/// @file       timer.h
/// @brief      
//==========================================

#pragma once

#ifndef TIMER_H
#define TIMER_H

#include "typedef.h"

#define TIMER_COUNT 4

#define TIMER_CONTROL_PRESCALER_MASK  0x0003
#define TIMER_CONTROL_CASCADE         (1u << 2)
#define TIMER_CONTROL_IRQ_ENABLE      (1u << 6)
#define TIMER_CONTROL_ENABLE          (1u << 7)

#define TIMER_CONTROL_MASK (TIMER_CONTROL_PRESCALER_MASK | TIMER_CONTROL_CASCADE | TIMER_CONTROL_IRQ_ENABLE | TIMER_CONTROL_ENABLE)

typedef struct __timer_base {
    u16 reload;
    u16 counter;
    u16 control;
    u32 accumulator;
} timer_base_t;

typedef struct __timer_state {
    timer_base_t timers[TIMER_COUNT];
} timer_state_t;

void timer_reset();
void timer_step(u32 cycles);

u16 timer_read_low(u32 index);
u16 timer_read_reload(u32 index);
u16 timer_read_high(u32 index);

void timer_write_low(u32 index, u16 value);
void timer_write_high(u32 index, u16 value);

#endif // TIMER_H