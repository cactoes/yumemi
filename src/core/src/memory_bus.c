#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#include "memory_bus.h"

static memory_bus_t global_memory_bus;

static const u8 non_seq_waitstates[4] = { 4, 3, 2, 8 };

static
memory_bus_t* get_memory_bus() {
    return &global_memory_bus;
}

static
u32 read_le32(u8* p) {
    return (u32)p[0] | ((u32)p[1] << 8) | ((u32)p[2] << 16) | ((u32)p[3] << 24);
}

static
u16 read_le16(u8* p) {
    return (u16)(p[0] | (p[1] << 8));
}

static
u8 read_le8(u8* p) {
    return p[0];
}

static
bool is_rom_region_or_mirror(u8 region) {
    return region == MEMORY_BUS_REGION_ROM ||
        region == MEMORY_BUS_REGION_ROM_MIRROR ||
        region == MEMORY_BUS_REGION_ROM_MIRROR_2 ||
        region == MEMORY_BUS_REGION_ROM_MIRROR_3 ||
        region == MEMORY_BUS_REGION_ROM_MIRROR_4 ||
        region == MEMORY_BUS_REGION_ROM_MIRROR_5;
}

static
bool is_flash_region_or_mirror(u8 region) {
    return region == MEMORY_BUS_REGION_FLASH ||
        region == MEMORY_BUS_REGION_FLASH_MIRROR;
}

static
u32 mbus_get_cycle_count(u32 address, u32 size, bool sequential) {
    switch (MEMORY_ADDRESS_TO_REGION(address)) {
        case MEMORY_BUS_REGION_EWRAM:
            return size == 4 ? 6 : 3;
        case MEMORY_BUS_REGION_PALETTE:
        case MEMORY_BUS_REGION_VRAM:
            return size == 4 ? 2 : 1;
        case MEMORY_BUS_REGION_ROM:
        case MEMORY_BUS_REGION_ROM_MIRROR:
        case MEMORY_BUS_REGION_ROM_MIRROR_2:
        case MEMORY_BUS_REGION_ROM_MIRROR_3:
        case MEMORY_BUS_REGION_ROM_MIRROR_4:
        case MEMORY_BUS_REGION_ROM_MIRROR_5: {
            u32 first = sequential ? 3 : 5;
            return size == 4 ? first + 3 : first;
        }
        default:
            return 1;
    }
}

static
void mbus_handle_access_cycles(u32 address, u32 size) {
    bool is_sequential = (address == global_memory_bus.last_address);
    global_memory_bus.cycle_counter += mbus_get_cycle_count(address, size, is_sequential);
    global_memory_bus.last_address = address + size;
}

static
u32 mbus_rom_read(u32 address, u32 size) {
    u32 offset = address & 0x01ffffffu;
    u32 value = 0;

    for (u32 i = 0; i < size; i++)
        value |= (u32)mbus_rom_byte(offset + i) << (8u * i);

    return value;
}

static
u8* memory_address_to_ram(u32 address) {
    switch (MEMORY_ADDRESS_TO_REGION(address)) {
        case MEMORY_BUS_REGION_EWRAM:
            return global_memory_bus.ram.ewr + (address & (MEMORY_BUS_EWRAM_SIZE - 1));
        case MEMORY_BUS_REGION_IWRAM:
            return global_memory_bus.ram.iwr + (address & (MEMORY_BUS_IWRAM_SIZE - 1));
        case MEMORY_BUS_REGION_OAM:
            return global_memory_bus.ram.oam + (address & (MEMORY_BUS_OAM_SIZE - 1));
        case MEMORY_BUS_REGION_PALETTE:
            return global_memory_bus.ram.palette + (address & (MEMORY_BUS_PALETTE_SIZE - 1));
        case MEMORY_BUS_REGION_IO: {
            u32 offset = address & 0x00ffffffu;
            return offset < MEMORY_BUS_IO_SIZE ? global_memory_bus.ram.io + offset : 0;
        }
        case MEMORY_BUS_REGION_VRAM: {
            u32 offset = address & 0x0001ffffu;
            if (offset >= MEMORY_BUS_VRAM_SIZE)
                offset -= 0x8000;

            return global_memory_bus.ram.video + offset;
        }
        default:
            return 0;
    }
}

void memory_bus_reset(u8* rom, u64 size) {
    memory_bus_t* mbus = get_memory_bus();

    mbus->rom.data = rom;
    mbus->rom.size = size;

    mbus->ram.iwr = calloc(1, MEMORY_BUS_OAM_SIZE);
    mbus->ram.ewr = calloc(1, MEMORY_BUS_OAM_SIZE);
    mbus->ram.io = calloc(1, MEMORY_BUS_OAM_SIZE);
    mbus->ram.palette = calloc(1, MEMORY_BUS_OAM_SIZE);
    mbus->ram.video = calloc(1, MEMORY_BUS_OAM_SIZE);
    mbus->ram.oam = calloc(1, MEMORY_BUS_OAM_SIZE);

    mbus->cycle_counter = 0;
    mbus->last_address = 0xffffffffu;
}

u32 mbus_read32(u32 address, bool count_cycles) {
    address &= ~3u;

    if (count_cycles == MBUS_COUNT)
        mbus_handle_access_cycles(address, 4);

    u8 region = mbus_address_to_region(address);

    // if (region == MEMORY_BUS_REGION_IO)
    //     return io_read32(address & 0x00ffffffu);

    // if (region == MEMORY_BUS_REGION_BIOS)
    //     return global_memory_bus.bios_open_bus;

    if (is_rom_region_or_mirror(region))
        return mbus_rom_read(address, 4);

    if (is_flash_region_or_mirror(region))
        return (u32)flash_read8(address) * 0x01010101u;

    const u8 *p = mbus_ram_ptr(address);
    if (!p) {
        // TODO @since 28/10/2026 -- 23:57
        // open bus
        printf("memory bus read32 unhandled region: 0x%08X\n", address);
        return 0;
    }

    return read_le32(p);
}

u16 mbus_read16(u32 address, bool count_cycles);
u8 mbus_read8(u32 address, bool count_cycles);