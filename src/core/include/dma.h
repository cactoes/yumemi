//==========================================
/// @file       dma.h
/// @brief      
//==========================================

#ifndef DMA_H
#define DMA_H

#include "typedef.h"

#define DMA_CHANNEL_COUNT 4

#define DMA_BASE(ch)    (0xB0u + (ch) * 12u)
#define DMA_ENABLE      (1u << 15)
#define DMA_IRQ         (1u << 14)
#define DMA_REPEAT      (1u << 9)
#define DMA_WORD        (1u << 10)

#define DMA_SRC_MASK(ch) ((ch) == 0 ? 0x07FFFFFFu : 0x0FFFFFFFu)
#define DMA_DST_MASK(ch) ((ch) == 3 ? 0x0FFFFFFFu : 0x07FFFFFFu)

typedef struct __dma_channel_t {
    u32 source;
    u32 destination;
    u32 initial_destination;
    u32 count;
    u32 remaining;
    u16 control;
    bool enabled;
    bool active;
} dma_channel_t;

typedef struct __dma {
    dma_channel_t channels[DMA_CHANNEL_COUNT];
} dma_t;

void dma_reset();
void dma_step(u32* cycles);
bool dma_is_active();

void dma_on_control_write(u32 channel, u16 old_control, u16 new_control);
void dma_on_hblank();
void dma_on_vblank();

#endif // DMA_H