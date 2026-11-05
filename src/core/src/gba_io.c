#include <string.h>

#include "gba_io.h"
#include "helper.h"
#include "memory_bus.h"
#include "timer.h"
#include "dma.h"
#include "ppu.h"

static bool io_cpu_halted = false;
static u8* io_base = 0;

u16 io_read_raw16(u32 offset) {
    return (u16)(io_base[offset] | (io_base[offset + 1] << 8));
}

void io_write_raw16(u32 offset, u16 value) {
    io_base[offset] = value & 0xff;
    io_base[offset + 1] = value >> 8;
}

static inline
bool io_is_timer(u32 offset) {
    return (offset - IO_TIMER_BASE) <= (IO_TIMER_END - IO_TIMER_BASE);
}

static
u16 io_timer_read16(u32 offset) {
    u32 index = (offset - IO_TIMER_BASE) >> 2;
    return (offset & 2) ? timer_read_high(index) : timer_read_low(index);
}

static
u16 io_timer_write_base(u32 offset) {
    u32 index = (offset - IO_TIMER_BASE) >> 2;
    return (offset & 2) ? timer_read_high(index) : timer_read_reload(index);
}

static
void io_timer_write16(u32 offset, u16 value) {
    u32 index = (offset - IO_TIMER_BASE) >> 2;

    if (offset & 2)
        timer_write_high(index, value);
    else
        timer_write_low(index, value);
}

void io_request_irq(u16 bit) {
    io_write_raw16(IO_IF, io_read_raw16(IO_IF) | bit);
}

void io_set_keys_pressed(u16 pressed_mask) {
    io_write_raw16(IO_KEYINPUT, (u16)(~pressed_mask & 0x03ff));
}

bool io_irq_wake_pending() {
    return (io_read_raw16(IO_IE) & io_read_raw16(IO_IF)) != 0;
}

void io_reset() {
    io_base = mbus_get_region(MEMORY_BUS_REGION_IO);
    memset(io_base, 0, MEMORY_BUS_IO_SIZE);

    io_cpu_halted = false;
    dma_reset();

    io_write_raw16(IO_DISPCNT, 0x0080);
    io_write_raw16(IO_KEYINPUT, 0x03ff);

    for (u32 base = 0x020; base <= 0x030; base += 0x010) {
        io_write_raw16(base, 0x0100);
        io_write_raw16(base + 0x002, 0x0000);
        io_write_raw16(base + 0x004, 0x0000);
        io_write_raw16(base + 0x006, 0x0100);
    }
}

bool io_cpu_handle_irq(u32* cycles) {
    if (!io_cpu_halted)
        return false;

    if (io_irq_wake_pending())
        io_cpu_halted = false;

    *cycles = 8;

    return true;
}

void io_cpu_halt() {
    io_cpu_halted = true;
}

u8 io_read8(u32 offset, u8 flags) {
    (void)flags;

    if (io_is_timer(offset)) {
        u16 value = io_timer_read16(offset & ~1u);
        return (offset & 1) ? (u8)(value >> 8) : (u8)(value & 0xff);
    }

    if (offset >= MEMORY_BUS_IO_SIZE)
        return 0;

    return io_base[offset];
}

u16 io_read16(u32 offset, u8 flags) {
    if (io_is_timer(offset))
        return io_timer_read16(offset & ~1u);

    if (offset < MEMORY_BUS_IO_SIZE - 1u)
        return (u16)(io_base[offset] | ((u16)io_base[offset + 1u] << 8));

    return (u16)(io_read8(offset, flags) | (io_read8(offset + 1, flags) << 8));
}

u32 io_read32(u32 offset, u8 flags) {
    return (u32)io_read16(offset, flags) | ((u32)io_read16(offset + 2, flags) << 16);
}

static
bool io_is_dma_control_high(u32 offset) {
    return offset >= IO_DMA_BASE && offset < IO_DMA_END && (offset - IO_DMA_BASE) % IO_DMA_STRIDE == 11;
}

void io_write8(u32 offset, u8 value, u8 flags) {
    if (MASK_IS_SET(flags, IO_NOSIDE_EFFECT))
        return;

    if (offset >= MEMORY_BUS_IO_SIZE)
        return;

    if (io_is_timer(offset)) {
        u32 aligned = offset & ~1u;
        u16 current = io_timer_write_base(aligned);

        if (offset & 1)
            current = (current & 0x00ff) | ((u16)value << 8);
        else
            current = (current & 0xff00) | value;

        io_timer_write16(aligned, current);
        return;
    }

    switch (offset) {
        case IO_DISPSTAT:
            io_base[offset] = (io_base[offset] & 0x07) | (value & 0x38);
            return;
        case IO_DISPSTAT + 1:
            io_base[offset] = value;
            return;
        case IO_VCOUNT:
        case IO_VCOUNT + 1:
        case IO_KEYINPUT:
        case IO_KEYINPUT + 1:
            return;
        case IO_IE:
            io_base[offset] = value;
            return;
        case IO_IE + 1:
            io_base[offset] = value & 0x3f;
            return;
        case IO_IF:
        case IO_IF + 1:
            io_base[offset] &= (u8)~value;
            return;
        case IO_IME:
            io_base[offset] = value & 1;
            return;
        case IO_IME + 1:
        case IO_IME + 2:
        case IO_IME + 3:
            return;
        case IO_HALTCNT:
            io_cpu_halted = true;
            return;
        default:
            if (io_is_dma_control_high(offset)) {
                u32 channel = (offset - IO_DMA_BASE) / IO_DMA_STRIDE;
                u32 control_offset = IO_DMA_BASE + channel * IO_DMA_STRIDE + 10;
                u16 old_control = io_read_raw16(control_offset);

                io_base[offset] = value;

                dma_on_control_write(channel, old_control, io_read_raw16(control_offset));
            } else {
                io_base[offset] = value;

                if ((offset >= 0x28 && offset < 0x30) || (offset >= 0x38 && offset < 0x40))
                    ppu_on_io_write(offset);
            }
            return;
    }
}

void io_write16(u32 offset, u16 value, u8 flags) {
    if (MASK_IS_SET(flags, IO_NOSIDE_EFFECT))
        return;

    if (io_is_timer(offset)) {
        io_timer_write16(offset & ~1u, value);
        return;
    }

    io_write8(offset, value & 0xff, flags);
    io_write8(offset + 1, value >> 8, flags);
}

void io_write32(u32 offset, u32 value, u8 flags) {
    io_write16(offset, value & 0xffff, flags);
    io_write16(offset + 2, value >> 16, flags);
}