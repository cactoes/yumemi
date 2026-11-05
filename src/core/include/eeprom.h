//==========================================
/// @file       eeprom.h
/// @brief      
//==========================================

#pragma once

#ifndef EEPROM_H
#define EEPROM_H

#define EEPROM_SMALL_SIZE       512u
#define EEPROM_LARGE_SIZE       8192u
#define EEPROM_MAX_COMMAND_BITS 81u
#define EEPROM_MAX_PATH         512u

#define EEPROM_PROGRAM_CYCLES   115000u

#define EEPROM_DEFAULT        (0u)
#define EEPROM_NOCOUNT        (1u << 0)
#define EEPROM_NOSIDE_EFFECT  (1u << 1)

#include "typedef.h"

typedef struct __eeprom {
    bool enabled;
    bool dirty;
    bool persistence_allowed;
    u64 rom_size;
    u32 size;
    u32 busy_cycles;
    u32 expected_bits;
    u32 received_bits;
    u32 read_position;
    bool reading;
    u8 command[EEPROM_MAX_COMMAND_BITS];
    u8 response[8];
    u8 data[EEPROM_LARGE_SIZE];
    char sav_path[EEPROM_MAX_PATH];
} eeprom_t;

void eeprom_reset(const char* rom_path, u8* rom, u64 rom_size);
bool eeprom_enabled();
bool is_eeprom_address(u32 address);
bool eeprom_is_dirty();

void eeprom_write16(u16 value, u8 flags);
u16 eeprom_read16(u8 flags);

void eeprom_begin_dma_write(u32 halfword_count);

void eeprom_step(u32 cycles);

bool eeprom_save();

#endif // EEPROM_H