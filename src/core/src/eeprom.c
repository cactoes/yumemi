#include <string.h>
#include <stdio.h>
#include "memory_bus.h"
#include "eeprom.h"
#include "helper.h"

static eeprom_t global_eeprom;

static
bool check_signature(u8* rom, u64 size) {
    static const char signature[] = "EEPROM_V";
    const u64 length = sizeof(signature) - 1u;

    if (!rom || size < length)
        return false;

    for (u64 offset = 0; offset <= size - length; offset++) {
        if (memcmp(rom + offset, signature, (u64)length) == 0)
            return true;
    }
    return false;
}

static
void eeprom_make_sav_path(const char* rom_path, char* out, u64 size) {
    snprintf(out, size, "%s", rom_path);

    char* slash = strrchr(out, '/');
    char* bslash = strrchr(out, '\\');
    if (bslash > slash)
        slash = bslash;

    char* dot = strrchr(out, '.');
    if (dot && (!slash || dot > slash))
        *dot = '\0';

    u64 len = strlen(out);
    if (len + 5 <= size)
        memcpy(out + len, ".sav", 5);
}

static
bool eeprom_load() {
    FILE* fp = fopen(global_eeprom.sav_path, "rb");
    if (!fp) {
        global_eeprom.persistence_allowed = false;
        return false;
    }

    if (fseek(fp, 0, SEEK_END) != 0) {
        fclose(fp);
        return false;
    }

    long size = ftell(fp);

    if (size != EEPROM_SMALL_SIZE &&
        size != EEPROM_LARGE_SIZE) {
        fclose(fp);
        global_eeprom.persistence_allowed = false;
        return false;
    }

    if (fseek(fp, 0, SEEK_SET) != 0) {
        fclose(fp);
        return false;
    }

    u64 n = fread(global_eeprom.data, 1, (u64)size, fp);
    bool okay = n == (u64)size && !ferror(fp);
    fclose(fp);

    if (!okay) {
        memset(global_eeprom.data, 0xff, EEPROM_LARGE_SIZE);
        global_eeprom.persistence_allowed = false;
        return false;
    }

    global_eeprom.size = (u32)size;
    printf("[EEPROM] loaded %ld bytes\n", size);
    return true;
}

static
void execute_command() {
    u32 count = global_eeprom.expected_bits;
    u32 address_bits = (count == 9 || count == 73) ? 6 : 14;
    bool read = global_eeprom.command[1] != 0;
    u32 required = 2 + address_bits + (read ? 0 : 64) + 1;

    global_eeprom.expected_bits = 0;
    global_eeprom.received_bits = 0;

    if (global_eeprom.command[0] != 1 || count != required)
        return;

    u32 block = 0;
    for (u32 i = 0; i < address_bits; i++)
        block = (block << 1) | global_eeprom.command[2 + i];

    block &= (global_eeprom.size / 8) - 1;
    u32 offset = block * 8;

    if (read) {
        memcpy(global_eeprom.response, global_eeprom.data + offset, sizeof(global_eeprom.response));
        global_eeprom.read_position = 0;
        global_eeprom.reading = true;
        return;
    }

    u8 payload[8] = {0};
    u32 start = 2 + address_bits;
    for (u32 bit = 0; bit < 64; bit++) {
        payload[bit >> 3] |=
            (u8)(global_eeprom.command[start + bit] << (7u - (bit & 7u)));
    }

    memcpy(global_eeprom.data + offset, payload, sizeof(payload));
    global_eeprom.dirty = true;
    global_eeprom.reading = false;
    global_eeprom.busy_cycles = EEPROM_PROGRAM_CYCLES;
}

void eeprom_reset(const char* rom_path, u8* rom, u64 rom_size) {
    memset(&global_eeprom, 0 , sizeof(eeprom_t));
    memset(global_eeprom.data, 0xff, EEPROM_LARGE_SIZE);

    global_eeprom.rom_size = rom_size;
    global_eeprom.size = EEPROM_SMALL_SIZE;
    global_eeprom.enabled = check_signature(rom, rom_size);
    if (!global_eeprom.enabled)
        return;

    eeprom_make_sav_path(rom_path, global_eeprom.sav_path, EEPROM_MAX_PATH);
    printf("eeprom save path: %s\n", global_eeprom.sav_path);
    global_eeprom.persistence_allowed = true;
    eeprom_load();
}

bool eeprom_enabled() {
    return global_eeprom.enabled;
}

bool is_eeprom_address(u32 address) {
    if (!global_eeprom.enabled || MEMORY_ADDRESS_TO_REGION(address) != MEMORY_BUS_REGION_ROM_MIRROR_5)
        return false;

    return global_eeprom.rom_size <= 0x01000000u ||  address >= 0x0dffff00u;
}

bool eeprom_is_dirty() {
    return global_eeprom.enabled && global_eeprom.dirty;
}

void eeprom_write16(u16 value, u8 flags) {
    if (!global_eeprom.enabled || (flags & MEM_ACCESS_NOSIDE_EFFECT) || global_eeprom.busy_cycles || global_eeprom.expected_bits == 0)
        return;

    global_eeprom.command[global_eeprom.received_bits++] = (u8)(value & 1u);
    if (global_eeprom.received_bits == global_eeprom.expected_bits)
        execute_command();
}

u16 eeprom_read16(u8 flags) {
    if (!global_eeprom.enabled)
        return 1;

    if (!global_eeprom.reading)
        return global_eeprom.busy_cycles ? 0 : 1;

    u32 position = global_eeprom.read_position;
    u16 value = 0;

    if (position >= 4) {
        u32 bit = position - 4;
        value = (u16)((global_eeprom.response[bit >> 3] >> (7u - (bit & 7u))) & 1u);
    }

    if (!(flags & MEM_ACCESS_NOSIDE_EFFECT)) {
        global_eeprom.read_position++;
        if (global_eeprom.read_position == 68) {
            global_eeprom.reading = false;
            global_eeprom.read_position = 0;
        }
    }

    return value;
}

void eeprom_begin_dma_write(u32 halfword_count) {
    if (!global_eeprom.enabled || global_eeprom.busy_cycles)
        return;

    global_eeprom.expected_bits = 0;
    global_eeprom.received_bits = 0;
    global_eeprom.reading = false;
    global_eeprom.read_position = 0;

    switch (halfword_count) {
        case 9:
        case 73:
            global_eeprom.size = EEPROM_SMALL_SIZE;
            break;
        case 17:
        case 81:
            global_eeprom.size = EEPROM_LARGE_SIZE;
            break;
        default:
            return;
    }

    global_eeprom.expected_bits = halfword_count;
}

void eeprom_step(u32 cycles) {
    if (cycles >= global_eeprom.busy_cycles)
        global_eeprom.busy_cycles = 0;
    else
        global_eeprom.busy_cycles -= cycles;
}

bool eeprom_save() {
    if (!global_eeprom.enabled || !global_eeprom.dirty)
        return true;

    if (!global_eeprom.persistence_allowed)
        return false;

    char tmp[512];
    snprintf(tmp, sizeof(tmp), "%s.tmp", global_eeprom.sav_path);

    FILE* fp = fopen(tmp, "wb");
    if (!fp)
        return false;

    u64 n = fwrite(global_eeprom.data, 1, global_eeprom.size, fp);
    fclose(fp);
    if (n != global_eeprom.size)
        return false;

    remove(global_eeprom.sav_path);

    if (rename(tmp, global_eeprom.sav_path) == 0) {
        global_eeprom.dirty = false;
        return true;
    }

    return false;
}