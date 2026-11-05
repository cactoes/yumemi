//==========================================
/// @file       gba_io.h
/// @brief      
//==========================================

#pragma once

#ifndef GBA_IO_H
#define GBA_IO_H

#define IO_DISPCNT      0x000
#define IO_DISPSTAT     0x004
#define IO_VCOUNT       0x006

#define IO_DMA_BASE     0x0B0
#define IO_DMA_END      0x0E0
#define IO_DMA_STRIDE   12

#define IO_TIMER_BASE   0x100
#define IO_TIMER_END    0x10F

#define IO_KEYINPUT     0x130

#define IO_IE           0x200
#define IO_IF           0x202
#define IO_IME          0x208
#define IO_HALTCNT      0x301

#define IRQ_VBLANK      (1u << 0)
#define IRQ_HBLANK      (1u << 1)
#define IRQ_VCOUNT      (1u << 2)
#define IRQ_TIMER(i)    (1u << (3 + (i)))
#define IRQ_DMA(i)      (1u << (8 + (i)))
#define IRQ_KEYPAD      (1u << 12)

#include "typedef.h"

void io_reset();

u16 io_read_raw16(u32 offset);
void io_write_raw16(u32 offset, u16 value);

void io_request_irq(u16 bit);
void io_set_keys_pressed(u16 pressed_mask);
bool io_irq_wake_pending(void);

bool io_cpu_handle_irq(u32* cycles);
void io_cpu_halt();

u8 io_read8(u32 offset, u8 flags);
u16 io_read16(u32 offset, u8 flags);
u32 io_read32(u32 offset, u8 flags);

void io_write8(u32 offset, u8 value, u8 flags);
void io_write16(u32 offset, u16 value, u8 flags);
void io_write32(u32 offset, u32 value, u8 flags);

#endif // GBA_IO_H