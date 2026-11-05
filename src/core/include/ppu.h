//==========================================
/// @file       ppu.h
/// @brief      
//==========================================

#pragma once

#ifndef PPU_H
#define PPU_H

#define HDRAW_CYCLES       1008u
#define SCANLINE_CYCLES    1232u
#define VISIBLE_LINES      160u
#define TOTAL_LINES        228u

#define PPU_WIDTH          240u
#define PPU_HEIGHT         160u
#define PPU_BG_COUNT         4u

#define PPU_MODE5_WIDTH    160u
#define PPU_MODE5_HEIGHT   128u

#define DISPCNT_FRAME_SELECT    (1u << 4)
#define DISPCNT_FORCED_BLANK    (1u << 7)

#define PPU_DISPCNT_MODE(value)       ((value) & 7u)
#define PPU_DISPCNT_BG_ENABLE(bg)     (0x100u << (bg))

#define DISPSTAT_VBLANK_FLAG    (1u << 0)
#define DISPSTAT_HBLANK_FLAG    (1u << 1)
#define DISPSTAT_VCOUNT_FLAG    (1u << 2)
#define DISPSTAT_VBLANK_IRQ     (1u << 3)
#define DISPSTAT_HBLANK_IRQ     (1u << 4)
#define DISPSTAT_VCOUNT_IRQ     (1u << 5)

#define PPU_CHAR_BLOCK_SIZE      0x4000u
#define PPU_SCREEN_BLOCK_SIZE    0x0800u
#define PPU_BITMAP_FRAME_SIZE    0xA000u

#define BGCNT_PRIORITY(value)    ((value) & 3u)
#define BGCNT_CHAR_BASE(value)   ((((value) >> 2) & 3u) * PPU_CHAR_BLOCK_SIZE)
#define BGCNT_COLOR256(value)    (((value) & 0x0080u) != 0)
#define BGCNT_MAP_BASE(value)    ((((value) >> 8) & 31u) * PPU_SCREEN_BLOCK_SIZE)
#define BGCNT_SIZE(value)        (((value) >> 14) & 3u)

#define TILE_ENTRY_TILE(value)      ((value) & 0x03FFu)
#define TILE_ENTRY_HFLIP(value)     (((value) & 0x0400u) != 0)
#define TILE_ENTRY_VFLIP(value)     (((value) & 0x0800u) != 0)
#define TILE_ENTRY_PALBANK(value)   (((value) >> 12) & 15u)

#define PIXEL_NONE       0xFFFFu
#define LAYER_OBJ        4u
#define LAYER_BACKDROP   5u

#include "typedef.h"

enum {
    PREG_DISPCNT = 0x00,
    PREG_DISPSTAT = 0x04,
    PREG_VCOUNT = 0x06,
    PREG_BG0CNT = 0x08,
    PREG_BG0HOFS = 0x10,
    PREG_BG2PA = 0x20,
    PREG_WIN0H = 0x40,
    PREG_WIN1H = 0x42,
    PREG_WIN0V = 0x44,
    PREG_WIN1V = 0x46,
    PREG_WININ = 0x48,
    PREG_MOSAIC = 0x4cu,
    PREG_WINOUT = 0x4A,
    PREG_BLDCNT = 0x50,
    PREG_BLDALPHA = 0x52,
    PREG_BLDY = 0x54
};

enum {
    DISPLAY_OBJ_1D = 1u << 6,
    DISPLAY_OBJ = 1u << 12,
    DISPLAY_WIN0 = 1u << 13,
    DISPLAY_WIN1 = 1u << 14,
    DISPLAY_OBJWIN = 1u << 15,
    DISPLAY_WINDOWS = DISPLAY_WIN0 | DISPLAY_WIN1 | DISPLAY_OBJWIN,
    WINDOW_OBJ = 1u << 4,
    WINDOW_EFFECTS = 1u << 5,
    OBJ_COUNT = 128,
    INVALID_ORDER = 0xFFFFu
};

typedef struct __ppu_reference {
    int x;
    int y;
} ppu_reference_t;

typedef struct __ppu_registers {
    u16 display;
    u16 bg_control[PPU_BG_COUNT];
    u16 bg_hofs[PPU_BG_COUNT];
    u16 bg_vofs[PPU_BG_COUNT];
    int pa[2];
    int pc[2];
    u16 window_h[2];
    u16 window_v[2];
    u16 window_in;
    u16 window_out;
    u16 blend;
    u32 eva;
    u32 evb;
    u32 evy;
} ppu_registers_t;

typedef struct __ppu_candidate {
    u16 color;
    u8 layer;
    u32 order;
    bool semi;
} ppu_candidate_t;

typedef struct __ppu_line {
    u16 bg_color[PPU_BG_COUNT][PPU_WIDTH];
    u8 bg_priority[PPU_BG_COUNT];
    u16 obj_color[PPU_WIDTH];
    u8 obj_priority[PPU_WIDTH];
    bool obj_semi[PPU_WIDTH];
    bool obj_window[PPU_WIDTH];
} ppu_line_t;

typedef struct __ppu {
    u32 line_cycles;
    u32 vcount;
    bool in_hblank;
    bool frame_ready;
    u32 framebuffer[PPU_WIDTH * PPU_HEIGHT];

    ppu_reference_t reference[2];
    ppu_registers_t registers;
    ppu_line_t line;

    bool render_enabled;
} ppu_t ;

void ppu_reset();
void ppu_step(u32 cycles);

bool ppu_take_frame_ready();

u32* ppu_get_framebuffer();

void ppu_on_io_write(u32 offset);

void ppu_set_render_enabled(bool enabled);
bool ppu_render_enabled();

#endif // PPU_H