//==========================================
/// @file       sram.h
/// @brief      
//==========================================

#pragma once

#ifndef SRAM_H
#define SRAM_H

#define SRAM_CAPACITY       (32 * 1024)
#define SRAM_MAX_PATH       512

#include "typedef.h"

typedef struct __sram {
    u8 data[SRAM_CAPACITY];
    bool enabled;
    bool dirty;
    bool persistence_allowed;
    char sav_path[SRAM_MAX_PATH];
} sram_t;

void sram_reset(const char* rom_path, u8* rom, u64 rom_size);
bool sram_is_enabled();
bool sram_is_dirty();

void sram_write8(u32 address, u8 value, u8 flags);
u8 sram_read8(u32 address, u8 flags);

bool sram_save();

#endif // SRAM_H