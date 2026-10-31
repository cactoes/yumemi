//==========================================
/// @file       flash.h
/// @brief      
//==========================================

#pragma once

#ifndef FLASH_H
#define FLASH_H

#define FLASH_CAPACITY_64K  (64u * 1024u)
#define FLASH_CAPACITY_128K (128u * 1024u)
#define FLASH_SECTOR_SIZE   4096u

#define FLASH_MANUF_ID  0xC2
#define FLASH_DEVICE_ID 0x09

#define FLASH_DEFAULT        (0u)
#define FLASH_NOCOUNT        (1u << 0)
#define FLASH_NOSIDE_EFFECT  (1u << 1)

#define FLASH_PATH_MAX 512

#include "typedef.h"

typedef enum {
    FL_READY,
    FL_CMD1,
    FL_CMD2,
    FL_PROGRAM,
    FL_BANK,
    FL_ERASE1,
    FL_ERASE2,
    FL_ERASE3
} flash_state_t;

typedef struct __flash {
    u8 flash_data[FLASH_CAPACITY_128K];
    flash_state_t state;
    bool id_mode;
    u8 bank;
    bool dirty;
    char sav_path[FLASH_PATH_MAX];
} flash_t;

void flash_reset(const char* rom_path);
u8 flash_read8(u32 address, u8 flags);
void flash_write8(u32 address, u8 value, u8 flags);

u8* flash_get_data();
bool flash_is_dirty();
void flash_mark_clean();

bool flash_save();

#endif // FLASH_H