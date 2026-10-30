#include <string.h>

#include "flash.h"
#include "helper.h"

static flash_t global_flash;

void flash_reset() {
    memset(global_flash.flash_data, 0xff, FLASH_CAPACITY_128K);
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