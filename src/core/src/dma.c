#include <string.h>

#include "dma.h"
#include "memory_bus.h"
#include "gba_io.h"

// TODO @since 30/10/2026 -- 21:35
// proper dma cycle counts

static dma_t global_dma;

static
dma_channel_t* get_dma_channel(u32 index) {
    return &global_dma.channels[index & (DMA_CHANNEL_COUNT - 1)];
}

static
u32 dma_read_reg32(u32 offset) {
    return (u32)io_read_raw16(offset) | ((u32)io_read_raw16(offset + 2) << 16);
}

static
u32 dma_count(u32 ch) {
    u32 count = io_read_raw16(DMA_BASE(ch) + 8);

    if (ch == 3)
        return count ? count : 0x10000u;

    count &= 0x3FFFu;
    return count ? count : 0x4000u;
}

static
void dma_activate_channel(u32 ch) {
    dma_channel_t* dma_ch = get_dma_channel(ch);

    dma_ch->remaining = dma_count(ch);
    dma_ch->active = true;
}

static
void dma_trigger(u32 timing) {
    for (u32 ch = 0; ch < DMA_CHANNEL_COUNT; ch++) {
        dma_channel_t* dma_ch = get_dma_channel(ch);

        if (!dma_ch->enabled || dma_ch->active)
            continue;

        if (((dma_ch->control >> 12) & 3u) == timing)
            dma_activate_channel(ch);
    }
}

static
void dma_finish(u32 ch) {
    dma_channel_t* dma_ch = get_dma_channel(ch);
    u16 control = dma_ch->control;
    u32 timing = (control >> 12) & 3u;
    u32 destination_control = (control >> 5) & 3u;

    dma_ch->active = false;

    if ((control & DMA_REPEAT) && timing != 0) {
        if (destination_control == 3)
            dma_ch->destination = dma_ch->initial_destination;
    } else {
        dma_ch->enabled = false;
        dma_ch->control = (u16)(dma_ch->control & ~DMA_ENABLE);

        u32 offset = DMA_BASE(ch) + 10;
        io_write_raw16(offset, (u16)(io_read_raw16(offset) & ~DMA_ENABLE));
    }

    if (control & DMA_IRQ)
        io_request_irq(IRQ_DMA(ch));
}

void dma_reset(void) {
    memset(&global_dma, 0, sizeof(global_dma));
}

void dma_step(u32* cycles) {
    mbus_get_total_cycle_count(true);
    *cycles = 0;

    for (u32 ch = 0; ch < DMA_CHANNEL_COUNT; ch++) {
        dma_channel_t* dma_ch = get_dma_channel(ch);

        if (!dma_ch->active)
            continue;

        u16 control = dma_ch->control;
        u32 source = dma_ch->source;
        u32 destination = dma_ch->destination;
        bool word = (control & DMA_WORD) != 0;
        u32 step = word ? 4u : 2u;
        u32 source_control = (control >> 7) & 3u;
        u32 destination_control = (control >> 5) & 3u;

        if (word)
            mbus_write32(destination, mbus_read32(source, 0), MBUS_DEFAULT);
        else
            mbus_write16(destination, mbus_read16(source, 0), MBUS_DEFAULT);

        if (!dma_ch->active) {
            *cycles = mbus_get_total_cycle_count(true);
            return;
        }

        if (source_control == 0)
            dma_ch->source += step;
        else if (source_control == 1)
            dma_ch->source -= step;

        if (destination_control == 0 || destination_control == 3)
            dma_ch->destination += step;
        else if (destination_control == 1)
            dma_ch->destination -= step;

        dma_ch->source &= DMA_SRC_MASK(ch);
        dma_ch->destination &= DMA_DST_MASK(ch);
        dma_ch->remaining--;

        if (dma_ch->remaining == 0)
            dma_finish(ch);

        *cycles = mbus_get_total_cycle_count(true);
        return;
    }
}

bool dma_is_active() {
    for (u32 ch = 0; ch < DMA_CHANNEL_COUNT; ch++) {
        if (get_dma_channel(ch)->active)
            return true;
    }

    return false;
}

void dma_on_control_write(u32 channel, u16 old_control, u16 new_control) {
    if (channel >= DMA_CHANNEL_COUNT)
        return;

    dma_channel_t* dma_ch = get_dma_channel(channel);

    if (!(new_control & DMA_ENABLE)) {
        dma_ch->enabled = false;
        dma_ch->active = false;
        dma_ch->remaining = 0;
        return;
    }

    if (old_control & DMA_ENABLE) {
        dma_ch->control = new_control;
        return;
    }

    u32 base = DMA_BASE(channel);
    u32 alignment = (new_control & DMA_WORD) ? 3u : 1u;

    dma_ch->control = new_control;
    dma_ch->source = dma_read_reg32(base) & DMA_SRC_MASK(channel) & ~alignment;
    dma_ch->destination = dma_read_reg32(base + 4) & DMA_DST_MASK(channel) & ~alignment;
    dma_ch->initial_destination = dma_ch->destination;
    dma_ch->remaining = 0;
    dma_ch->enabled = true;
    dma_ch->active = false;

    if (((new_control >> 12) & 3u) == 0)
        dma_activate_channel(channel);
}

void dma_on_hblank() {
    dma_trigger(2);
}

void dma_on_vblank() {
    dma_trigger(1);
}