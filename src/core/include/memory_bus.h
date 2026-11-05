//==========================================
/// @file       memory_bus.h
/// @brief      
//==========================================

#pragma once

#ifndef MEMORY_BUS_H
#define MEMORY_BUS_H

#define MEMORY_BUS_BIOS_START       0x00000000u
#define MEMORY_BUS_BIOS_SIZE        0x00004000u

#define MEMORY_BUS_EWRAM_START      0x02000000u
#define MEMORY_BUS_EWRAM_SIZE       0x00040000u

#define MEMORY_BUS_IWRAM_START      0x03000000u
#define MEMORY_BUS_IWRAM_SIZE       0x00008000u

#define MEMORY_BUS_IO_START         0x04000000u
#define MEMORY_BUS_IO_SIZE          0x00000400u

#define MEMORY_BUS_PALETTE_START    0x05000000u
#define MEMORY_BUS_PALETTE_SIZE     0x00000400u

#define MEMORY_BUS_VRAM_START       0x06000000u
#define MEMORY_BUS_VRAM_SIZE        0x00018000u

#define MEMORY_BUS_OAM_START        0x07000000u
#define MEMORY_BUS_OAM_SIZE         0x00000400u

#define MEMORY_BUS_ROM_START        0x08000000u
#define MEMORY_BUS_ROM_SIZE         0x02000000u

#define MEMORY_BUS_FLASH_START      0x0e000000u
#define MEMORY_BUS_FLASH_SIZE       0x00010000u

#define MBUS_DEFAULT        (0u)
#define MBUS_NOCOUNT        (1u << 0)
#define MBUS_NOSIDE_EFFECT  (1u << 1)

#define MEMORY_ADDRESS_TO_REGION(x) ((u8)(x >> 24))
#define MEMORY_REGION_TO_ADDRESS(x) ((u32)(x << 24))

#define MEM_RESET_RAM_EWR       (1u << 0)
#define MEM_RESET_RAM_IWR       (1u << 1)
#define MEM_RESET_RAM_PALETTE   (1u << 2)
#define MEM_RESET_RAM_VRAM      (1u << 3)
#define MEM_RESET_RAM_OAM       (1u << 4)
#define MEM_RESET_RAM_SERIAL    (1u << 5)
#define MEM_RESET_RAM_SOUND     (1u << 6)
#define MEM_RESET_RAM_IO        (1u << 7)

#include "typedef.h"

typedef enum {
    MEMORY_BUS_REGION_BIOS = MEMORY_ADDRESS_TO_REGION(MEMORY_BUS_BIOS_START),
    MEMORY_BUS_REGION_EWRAM = MEMORY_ADDRESS_TO_REGION(MEMORY_BUS_EWRAM_START),
    MEMORY_BUS_REGION_IWRAM = MEMORY_ADDRESS_TO_REGION(MEMORY_BUS_IWRAM_START),
    MEMORY_BUS_REGION_IO = MEMORY_ADDRESS_TO_REGION(MEMORY_BUS_IO_START),
    MEMORY_BUS_REGION_PALETTE = MEMORY_ADDRESS_TO_REGION(MEMORY_BUS_PALETTE_START),
    MEMORY_BUS_REGION_VRAM = MEMORY_ADDRESS_TO_REGION(MEMORY_BUS_VRAM_START),
    MEMORY_BUS_REGION_OAM = MEMORY_ADDRESS_TO_REGION(MEMORY_BUS_OAM_START),
    MEMORY_BUS_REGION_ROM = MEMORY_ADDRESS_TO_REGION(MEMORY_BUS_ROM_START),
    MEMORY_BUS_REGION_ROM_MIRROR = MEMORY_ADDRESS_TO_REGION(MEMORY_BUS_ROM_START) + 1,
    MEMORY_BUS_REGION_ROM_MIRROR_2 = MEMORY_ADDRESS_TO_REGION(MEMORY_BUS_ROM_START) + 2,
    MEMORY_BUS_REGION_ROM_MIRROR_3 = MEMORY_ADDRESS_TO_REGION(MEMORY_BUS_ROM_START) + 3,
    MEMORY_BUS_REGION_ROM_MIRROR_4 = MEMORY_ADDRESS_TO_REGION(MEMORY_BUS_ROM_START) + 4,
    MEMORY_BUS_REGION_ROM_MIRROR_5 = MEMORY_ADDRESS_TO_REGION(MEMORY_BUS_ROM_START) + 5,
    MEMORY_BUS_REGION_FLASH = MEMORY_ADDRESS_TO_REGION(MEMORY_BUS_FLASH_START),
    MEMORY_BUS_REGION_FLASH_MIRROR = MEMORY_ADDRESS_TO_REGION(MEMORY_BUS_FLASH_START) + 1
} memory_bus_region_t;

typedef struct __memory_bus {
    struct __rom {
        u64 size;
        u8* data;
    } rom;

    struct __ram {
        u8* iwr;
        u8* ewr;
        u8* io;
        u8* palette;
        u8* video;
        u8* oam;
    } ram;

    u32 cycle_counter;
    u32 last_address;

    u32 cpu_open_bus;
    u32 bios_open_bus;
} memory_bus_t;

typedef enum {
    MBUS_OPEN_BUS_CPU,
    MBUS_OPEN_BUS_BIOS
} mbus_open_bus_source_t;

void memory_bus_reset(u8* rom, u64 size);

u32 mbus_read32(u32 address, u8 flags);
u16 mbus_read16(u32 address, u8 flags);
u8 mbus_read8(u32 address, u8 flags);

void mbus_write32(u32 address, u32 value, u8 flags);
void mbus_write16(u32 address, u16 value, u8 flags);
void mbus_write8(u32 address, u8 value, u8 flags);

void mbus_set_cpu_open_bus(u32 value);
void mbus_set_bios_open_bus(u32 value);

void mbus_add_cycles(u32 count);
void mbus_add_cycles_from_address(u32 address, u32 size);

u32 mbus_get_total_cycle_count(bool reset_count);

u8* mbus_get_region(u8 region);

void mbus_reset_ram(u32 flags);

#endif // MEMORY_BUS_H