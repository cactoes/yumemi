#include <string.h>
#include <stdio.h>
#include "flash.h"
#include "helper.h"

static flash_t global_flash;

static
void flash_make_sav_path(const char* rom_path, char* out, size_t size) {
    snprintf(out, size, "%s", rom_path);

    char* slash = strrchr(out, '/');
    char* bslash = strrchr(out, '\\');
    if (bslash > slash)
        slash = bslash;

    char* dot = strrchr(out, '.');
    if (dot && (!slash || dot > slash))
        *dot = '\0';

    size_t len = strlen(out);
    if (len + 5 <= size)
        memcpy(out + len, ".sav", 5);
}

static
bool flash_load() {
    FILE* fp = fopen(global_flash.sav_path, "rb");
    if (!fp)
        return false;

    size_t n = fread(flash_get_data(), 1, FLASH_CAPACITY_128K, fp);
    fclose(fp);
    return n > 0;
}

bool flash_save() {
    char tmp[512];
    snprintf(tmp, sizeof(tmp), "%s.tmp", global_flash.sav_path);

    FILE* fp = fopen(tmp, "wb");
    if (!fp)
        return false;

    size_t n = fwrite(flash_get_data(), 1, FLASH_CAPACITY_128K, fp);
    fclose(fp);
    if (n != FLASH_CAPACITY_128K)
        return false;

    remove(global_flash.sav_path);
    return rename(tmp, global_flash.sav_path) == 0;
}

void flash_reset(const char* rom_path) {
    memset(&global_flash, 0, sizeof(global_flash));
    memset(global_flash.flash_data, 0xff, FLASH_CAPACITY_128K);

    flash_make_sav_path(rom_path, global_flash.sav_path, sizeof(global_flash.sav_path));
    printf("flash save path: %s\n", global_flash.sav_path);
    flash_load();
}

u8 flash_read8(u32 address, u8 flags) {
    flags;

    address &= 0xffff;

    if (global_flash.id_mode) {
        if (address == 0)
            return FLASH_MANUF_ID;
        if (address == 1)
            return FLASH_DEVICE_ID;
    }

    return global_flash.flash_data[(global_flash.bank << 16) | address];
}

void flash_write8(u32 address, u8 value, u8 flags) {
    if (MASK_IS_SET(flags, FLASH_NOSIDE_EFFECT))
        return;

    address &= 0xffff;

    switch (global_flash.state) {
        case FL_PROGRAM:
            global_flash.flash_data[(global_flash.bank << 16) | address] &= value;
            global_flash.dirty = true;
            global_flash.state = FL_READY;
            return;
        case FL_BANK:
            if (address == 0)
                global_flash.bank = value & 1;
            global_flash.state = FL_READY;
            return;
        case FL_READY:
            if (address == 0x5555 && value == 0xAA)
                global_flash.state = FL_CMD1;
            else if (value == 0xF0)
                global_flash.id_mode = false;
            return;
        case FL_CMD1:
            global_flash.state = (address == 0x2AAA && value == 0x55) ? FL_CMD2 : FL_READY;
            return;
        case FL_CMD2:
            global_flash.state = FL_READY;
            if (address != 0x5555)
                return;

            switch (value) {
                case 0x90: global_flash.id_mode = true; break;
                case 0xF0: global_flash.id_mode = false; break;
                case 0xA0: global_flash.state = FL_PROGRAM; break;
                case 0xB0: global_flash.state = FL_BANK; break;
                case 0x80: global_flash.state = FL_ERASE1; break;
            }
            return;
        case FL_ERASE1:
            global_flash.state = (address == 0x5555 && value == 0xAA) ? FL_ERASE2 : FL_READY;
            return;
        case FL_ERASE2:
            global_flash.state = (address == 0x2AAA && value == 0x55) ? FL_ERASE3 : FL_READY;
            return;
        case FL_ERASE3:
            global_flash.state = FL_READY;

            if (value == 0x30) {
                u32 base = ((global_flash.bank << 16) | address) & ~(FLASH_SECTOR_SIZE - 1);
                memset(&global_flash.flash_data[base], 0xff, FLASH_SECTOR_SIZE);
                global_flash.dirty = true;
            } else if (address == 0x5555 && value == 0x10) {
                memset(global_flash.flash_data, 0xff, FLASH_CAPACITY_128K);
                global_flash.dirty = true;
            }
            return;
    }
}

u8* flash_get_data() {
    return global_flash.flash_data;
}

bool flash_is_dirty() {
    return global_flash.dirty;
}

void flash_mark_clean() {
    global_flash.dirty = false;
}