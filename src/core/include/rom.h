//==========================================
/// @file       rom.h
/// @brief      
//==========================================

#pragma once

#ifndef ROM_H
#define ROM_H

#define ROM_BUSY                (u32)-1
#define ROM_OK                  0
#define ROM_ERROR_FILE_FAILED   1
#define ROM_ERROR_INVALID_ROM   2
#define ROM_ERROR_INVALID_SIZE  3

#define ROM_SEARCH_MAX_COUNT    2048

#define ROM_MAX_SIZE 0x02000000u

#include "typedef.h"

typedef struct {
    u32 arm_entry;
    u8 nintendo_logo[156];
    u8 game_title[12];
    u8 game_code[4];
    u8 maker_code[2];
    u8 fixed_value;
    u8 main_unit_code;
    u8 device_type;
    u8 reserved1[7];
    u8 software_version;
    u8 header_checksum;
    u8 reserved2[2];
} gba_cartridge_header_t;

u32 load_rom_from_path(const char* path, u8** data_out, u64* size_out);
void get_rom_title(u8* cartridge, char* buffer, u64 buffer_size);

const char* rom_status_to_text(u32 status);
void rom_release();

#endif // ROM_H