//==========================================
/// @file       renderer.h
/// @brief      
//==========================================

#pragma once

#ifndef RENDERER_H
#define RENDERER_H

#define GBA_KEY_A           (1u << 0)
#define GBA_KEY_B           (1u << 1)
#define GBA_KEY_SELECT      (1u << 2)
#define GBA_KEY_START       (1u << 3)
#define GBA_KEY_DPAD_RIGHT  (1u << 4)
#define GBA_KEY_DPAD_LEFT   (1u << 5)
#define GBA_KEY_DPAD_UP     (1u << 6)
#define GBA_KEY_DPAD_DOWN   (1u << 7)
#define GBA_KEY_R           (1u << 8)
#define GBA_KEY_L           (1u << 9)

#define WINDOW_NAME "yumemi - gba emulator"
#define DISPLAY_SCALE 4
#define GBA_FRAME_NS 16742706ull

#include "typedef.h"

typedef struct __frame_time {
    u32 speed_percent;
    u32 frameskip;
    bool auto_frameskip;
    u32 frame_counter;
    u32 consecutive_skips;
    u64 next_deadline_ns;
} frame_time_t;

void render_init(u32 width, u32 height);
void render_present(u32* buffer);
bool render_should_close();
void render_close();

void render_poll_events();
void render_update_events();

void render_handle_frame();

void render_set_close(bool state);

void render_set_frame_skip(u32 value);
void render_set_speed(u32 value);

#endif // RENDERER_H