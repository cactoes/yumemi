#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#include "helper.h"
#include "memory_bus.h"
#include "flash.h"
#include "gba_io.h"
#include "timer.h"
#include "eeprom.h"
#include "sram.h"

static memory_bus_t global_memory_bus;
static const u8 non_seq_waitstates[4] = { 4, 3, 2, 8 };
static u8* region_base[256];

static
memory_bus_t* get_memory_bus() {
    return &global_memory_bus;
}

static
u32 read_le32(const u8* p) {
    return (u32)p[0] | ((u32)p[1] << 8) | ((u32)p[2] << 16) | ((u32)p[3] << 24);
}

static
u16 read_le16(const u8* p) {
    return (u16)(p[0] | (p[1] << 8));
}

static
u8 read_le8(const u8* p) {
    return p[0];
}

static
void write_le32(u8* p, u32 value) {
    p[0] = (u8)value;
    p[1] = (u8)(value >> 8);
    p[2] = (u8)(value >> 16);
    p[3] = (u8)(value >> 24);
}

static
void write_le16(u8* p, u16 value) {
    p[0] = (u8)value;
    p[1] = (u8)(value >> 8);
}

static
void write_le8(u8* p, u8 value) {
    p[0] = value;
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
u8 mbus_rom_byte(u32 offset) {
    if (offset < global_memory_bus.rom.size)
        return global_memory_bus.rom.data[offset];

    return (u8)(((offset >> 1) >> ((offset & 1u) * 8u)) & 0xffu);
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
u32 mbus_open_bus_read(u32 address, u32 size, mbus_open_bus_source_t source) {
    u32 value = source == MBUS_OPEN_BUS_BIOS
        ? global_memory_bus.bios_open_bus
        : global_memory_bus.cpu_open_bus;

    switch (size) {
        case 1: return (value >> ((address & 3u) * 8u)) & 0xffu;
        case 2: return (value >> ((address & 2u) * 8u)) & 0xffffu;
        case 4: return value;
        default: return 0;
    }
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

    mbus->ram.iwr = calloc(1, MEMORY_BUS_IWRAM_SIZE);
    mbus->ram.ewr = calloc(1, MEMORY_BUS_EWRAM_SIZE);
    mbus->ram.io = calloc(1, MEMORY_BUS_IO_SIZE);
    mbus->ram.palette = calloc(1, MEMORY_BUS_PALETTE_SIZE);
    mbus->ram.video = calloc(1, MEMORY_BUS_VRAM_SIZE);
    mbus->ram.oam = calloc(1, MEMORY_BUS_OAM_SIZE);

    mbus->cycle_counter = 0;
    mbus->last_address = 0xffffffffu;

    memset(region_base, 0, sizeof(region_base));
    region_base[MEMORY_BUS_REGION_EWRAM] = mbus->ram.ewr;
    region_base[MEMORY_BUS_REGION_IWRAM] = mbus->ram.iwr;
    region_base[MEMORY_BUS_REGION_OAM] = mbus->ram.oam;
    region_base[MEMORY_BUS_REGION_PALETTE] = mbus->ram.palette;
    region_base[MEMORY_BUS_REGION_IO] = mbus->ram.io;
    region_base[MEMORY_BUS_REGION_VRAM] = mbus->ram.video;
}

static
u8 mbus_save_read8(u32 address, u8 flags) {
    if (sram_is_enabled())
        return sram_read8(address, flags);
    return flash_read8(address, flags);
}

static
void mbus_save_write8(u32 address, u8 value, u8 flags) {
    if (sram_is_enabled())
        sram_write8(address, value, flags);
    else
        flash_write8(address, value, flags);
}

u32 mbus_read32(u32 address, u8 flags) {
    address &= ~3u;

    if (!(MASK_IS_SET(flags, MBUS_NOCOUNT)))
        mbus_handle_access_cycles(address, 4);

    const u8 region = MEMORY_ADDRESS_TO_REGION(address);

    if (region == MEMORY_BUS_REGION_IO)
        return io_read32(address & 0x00ffffffu, flags);

    if (region == MEMORY_BUS_REGION_BIOS) {
        if (address >= (MEMORY_BUS_BIOS_START + MEMORY_BUS_BIOS_SIZE))
            return mbus_open_bus_read(address, 4, MBUS_OPEN_BUS_BIOS);

        // TODO @since 30/10/2026 -- 20:25
        // better bios handling
        printf("mbus_read32 bios address out of range: 0x%08x (returning open bus)\n", address);
        return mbus_open_bus_read(address, 4, MBUS_OPEN_BUS_BIOS);
    }

    if (is_rom_region_or_mirror(region))
        return mbus_rom_read(address, 4);

    if (is_flash_region_or_mirror(region))
        return (u32)mbus_save_read8(address, flags) * 0x01010101u;

    const u8 *p = memory_address_to_ram(address);
    if (!p)
        return mbus_open_bus_read(address, 4, MBUS_OPEN_BUS_CPU);

    return read_le32(p);
}

u16 mbus_read16(u32 address, u8 flags) {
    address &= ~1u;

    if (!MASK_IS_SET(flags, MBUS_NOCOUNT))
        mbus_handle_access_cycles(address, 2);

    const u8 region = MEMORY_ADDRESS_TO_REGION(address);

    if (region == MEMORY_BUS_REGION_IO)
        return io_read16(address & 0x00ffffffu, flags);

    if (region == MEMORY_BUS_REGION_BIOS) {
        if (address >= (MEMORY_BUS_BIOS_START + MEMORY_BUS_BIOS_SIZE))
            return (u16)mbus_open_bus_read(address, 2, MBUS_OPEN_BUS_CPU);

        // TODO @since 30/10/2026 -- 20:25
        // better bios handling
        return (u16)mbus_open_bus_read(address, 2, MBUS_OPEN_BUS_BIOS);
    }

    if (is_eeprom_address(address))
        return eeprom_read16(flags);

    if (is_rom_region_or_mirror(region))
        return (u16)mbus_rom_read(address, 2);

    if (is_flash_region_or_mirror(region))
        return (u16)((u16)mbus_save_read8(address, flags) * 0x0101u);

    const u8 *p = memory_address_to_ram(address);
    if (!p)
        return (u16)mbus_open_bus_read(address, 2, MBUS_OPEN_BUS_CPU);

    return read_le16(p);
}

u8 mbus_read8(u32 address, u8 flags) {
    if (!MASK_IS_SET(flags, MBUS_NOCOUNT))
        mbus_handle_access_cycles(address, 1);

    const u8 region = MEMORY_ADDRESS_TO_REGION(address);

    if (region == MEMORY_BUS_REGION_IO)
        return io_read8(address & 0x00ffffffu, flags);

    if (region == MEMORY_BUS_REGION_BIOS) {
        if (address >= (MEMORY_BUS_BIOS_START + MEMORY_BUS_BIOS_SIZE))
            return (u8)mbus_open_bus_read(address, 1, MBUS_OPEN_BUS_CPU);

        // TODO @since 30/10/2026 -- 20:25
        // better bios handling
        return (u8)mbus_open_bus_read(address, 1, MBUS_OPEN_BUS_BIOS);
    }

    if (is_rom_region_or_mirror(region))
        return (u8)mbus_rom_read(address, 1);

    if (is_flash_region_or_mirror(region))
        return mbus_save_read8(address, flags);

    const u8 *p = memory_address_to_ram(address);
    if (!p)
        return (u8)mbus_open_bus_read(address, 1, MBUS_OPEN_BUS_CPU);

    return read_le8(p);
}

void mbus_write32(u32 address, u32 value, u8 flags) {
    if (!MASK_IS_SET(flags, MBUS_NOCOUNT))
        mbus_handle_access_cycles(address, 4);

    const u8 region = MEMORY_ADDRESS_TO_REGION(address);

    if (is_flash_region_or_mirror(region)) {
        u8 byte = (u8)(value >> ((address & 3u) * 8u));
        mbus_save_write8(address, byte, flags);
        return;
    }

    address &= ~3u;

    if (region == MEMORY_BUS_REGION_IO) {
        io_write32(address & 0x00ffffffu, value, flags);
        return;
    }

    if (is_rom_region_or_mirror(region))
        return;

    u8* p = memory_address_to_ram(address);
    if (!p)
        return;

    write_le32(p, value);
}

void mbus_write16(u32 address, u16 value, u8 flags) {
    if (!MASK_IS_SET(flags, MBUS_NOCOUNT))
        mbus_handle_access_cycles(address, 2);

    const u8 region = MEMORY_ADDRESS_TO_REGION(address);

    if (is_flash_region_or_mirror(region)) {
        u8 byte = (u8)(value >> ((address & 1u) * 8u));
        mbus_save_write8(address, byte, flags);
        return;
    }

    address &= ~1u;

    if (region == MEMORY_BUS_REGION_IO) {
        io_write16(address & 0x00ffffffu, value, flags);
        return;
    }

    if (region == MEMORY_BUS_REGION_BIOS)
        return;

    if (is_eeprom_address(address)) {
        eeprom_write16(value, flags);
        return;
    }

    if (is_rom_region_or_mirror(region))
        return;

    u8 *p = memory_address_to_ram(address);
    if (!p)
        return;

    write_le16(p, value);
}

void mbus_write8(u32 address, u8 value, u8 flags) {
    if (!MASK_IS_SET(flags, MBUS_NOCOUNT))
        mbus_handle_access_cycles(address, 1);

    const u8 region = MEMORY_ADDRESS_TO_REGION(address);

    if (region == MEMORY_BUS_REGION_IO) {
        io_write8(address & 0x00ffffffu, value, flags);
        return;
    }

    if (region == MEMORY_BUS_REGION_BIOS)
        return;

    if (is_rom_region_or_mirror(region))
        return;

    if (is_flash_region_or_mirror(region)) {
        mbus_save_write8(address, value, flags);
        return;
    }

    if (region == MEMORY_BUS_REGION_OAM)
        return;

    u8* p = memory_address_to_ram(address);
    if (!p)
        return;

    if (region == MEMORY_BUS_REGION_PALETTE) {
        u16 doubled = (u16)((u16)value | ((u16)value << 8));
        write_le16(p, doubled);
        return;
    }

    if (region == MEMORY_BUS_REGION_VRAM) {
        u32 offset = address & 0x0001ffffu;
        if (offset >= MEMORY_BUS_VRAM_SIZE)
            offset -= 0x8000u;

        u32 mode = read_le16(global_memory_bus.ram.io) & 7u;
        u32 bg_end = mode >= 3 ? 0x14000u : 0x10000u;

        if (offset >= bg_end)
            return;

        u16 doubled = (u16)((u16)value | ((u16)value << 8));
        write_le16(global_memory_bus.ram.video + (offset & ~1u), doubled);
        return;
    }

    write_le8(p, value);
}

void mbus_set_cpu_open_bus(u32 value) {
    global_memory_bus.cpu_open_bus = value;
}

void mbus_set_bios_open_bus(u32 value) {
    global_memory_bus.bios_open_bus = value;
}

void mbus_add_cycles(u32 count) {
    global_memory_bus.cycle_counter += count;
}

void mbus_add_cycles_from_address(u32 address, u32 size) {
    mbus_add_cycles(mbus_get_cycle_count(address, size, true));
}

u32 mbus_get_total_cycle_count(bool reset_count) {
    u32 cycles = global_memory_bus.cycle_counter;
    if (reset_count)
        global_memory_bus.cycle_counter = 0;
    return cycles;
}

u8* mbus_get_region(u8 region) {
    return region_base[region];
}

static
void mbus_reset_io_display() {
    io_write16(0x004, 0, IO_DEFAULT);

    for (u32 offset = 0x008; offset < 0x020; offset += 2)
        io_write16(offset, 0, IO_DEFAULT);

    for (u32 base = 0x020; base <= 0x030; base += 0x010) {
        io_write16(base, 0x0100, IO_DEFAULT);
        io_write16(base + 2, 0, IO_DEFAULT);
        io_write16(base + 4, 0, IO_DEFAULT);
        io_write16(base + 6, 0x0100, IO_DEFAULT);
    }

    for (u32 offset = 0x040; offset <= 0x04a; offset += 2)
        io_write16(offset, 0, IO_DEFAULT);

    io_write16(0x04c, 0, IO_DEFAULT);
    io_write16(0x050, 0, IO_DEFAULT);
    io_write16(0x052, 0, IO_DEFAULT);
    io_write16(0x054, 0, IO_DEFAULT);
}

static
void mbus_reset_io_dma_timers() {
    for (u32 channel = 0; channel < 4; channel++) {
        u32 base = 0x0b0 + channel * 0x0c;
        io_write16(base + 0x0a, 0, IO_DEFAULT);
    }

    for (u32 channel = 0; channel < 4; channel++) {
        u32 base = 0x0b0 + channel * 0x0c;
        io_write32(base, 0, IO_DEFAULT);
        io_write32(base + 4, 0, IO_DEFAULT);
        io_write16(base + 8, 0, IO_DEFAULT);
    }

    for (u32 timer = 0; timer < 4; timer++)
        io_write16(0x102 + timer * 4, 0, IO_DEFAULT);

    timer_reset();

    for (u32 timer = 0; timer < 4; timer++)
        io_write16(0x100 + timer * 4, 0, IO_DEFAULT);
}

void mbus_reset_ram(u32 flags) {
    io_write16(0x000, 0x0080, IO_DEFAULT);

    if (MASK_ANY_SET(flags, MEM_RESET_RAM_EWR))
        memset(global_memory_bus.ram.ewr, 0, MEMORY_BUS_EWRAM_SIZE);

    if (MASK_ANY_SET(flags, MEM_RESET_RAM_IWR))
        memset(global_memory_bus.ram.iwr, 0, MEMORY_BUS_IWRAM_SIZE - 0x200);
    
    if (MASK_ANY_SET(flags, MEM_RESET_RAM_PALETTE))
        memset(global_memory_bus.ram.palette, 0, MEMORY_BUS_PALETTE_SIZE);

    if (MASK_ANY_SET(flags, MEM_RESET_RAM_VRAM))
        memset(global_memory_bus.ram.video, 0, MEMORY_BUS_VRAM_SIZE);

    if (MASK_ANY_SET(flags, MEM_RESET_RAM_OAM))
        memset(global_memory_bus.ram.oam, 0, MEMORY_BUS_OAM_SIZE);

    if (MASK_ANY_SET(flags, MEM_RESET_RAM_SERIAL)) {
        io_write16(0x128, 0, IO_DEFAULT);
        io_write16(0x134, 0x8000, IO_DEFAULT);
        io_write16(0x12a, 0, IO_DEFAULT);
        io_write16(0x140, 0, IO_DEFAULT);
        io_write32(0x150, 0, IO_DEFAULT);
        io_write32(0x154, 0, IO_DEFAULT);
    }

    if (MASK_ANY_SET(flags, MEM_RESET_RAM_SOUND)) {
        io_write16(0x084, 0, IO_DEFAULT);
        io_write16(0x060, 0, IO_DEFAULT);
        io_write16(0x062, 0, IO_DEFAULT);
        io_write16(0x064, 0, IO_DEFAULT);
        io_write16(0x068, 0, IO_DEFAULT);
        io_write16(0x06c, 0, IO_DEFAULT);
        io_write16(0x070, 0, IO_DEFAULT);
        io_write16(0x072, 0, IO_DEFAULT);
        io_write16(0x074, 0, IO_DEFAULT);
        io_write16(0x078, 0, IO_DEFAULT);
        io_write16(0x07c, 0, IO_DEFAULT);
        io_write16(0x080, 0, IO_DEFAULT);
        io_write16(0x082, 0, IO_DEFAULT);
        io_write16(0x088, 0x0200, IO_DEFAULT);

        for (u32 offset = 0x090; offset < 0x0a0; offset += 2)
            io_write16(offset, 0, IO_DEFAULT);
    }

    if (MASK_ANY_SET(flags, MEM_RESET_RAM_IO)) {
        io_write16(0x208, 0, IO_DEFAULT);

        mbus_reset_io_display();
        mbus_reset_io_dma_timers();

        io_write16(0x132, 0, IO_DEFAULT);
        io_write16(0x200, 0, IO_DEFAULT);
        io_write16(0x202, 0xffff, IO_DEFAULT);
        io_write16(0x204, 0, IO_DEFAULT);
    }
}