#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "rom.h"

static
u8 generate_header_checksum(gba_cartridge_header_t* header) {
    u8 check = 0;
    for (u32 i = 0xa0; i <= 0xbc; i++)
        check -= ((u8*)header)[i];

    return check - 0x19;
}

static
u64 get_file_size(FILE* fp) {
    if (fseek(fp, 0, SEEK_END) != 0)
        return 0;

    i32 size = ftell(fp);
    rewind(fp);

    if (size == -1l)
        return 0;

    return (u64)size;
}

static
bool validate_rom_header(gba_cartridge_header_t* header) {
    return header->header_checksum == generate_header_checksum(header);
}

u32 load_rom_from_path(const char* path, u8** data_out, u64* size_out) {
    FILE* rom_fp = fopen(path, "rb");
    if (!rom_fp)
        return ROM_ERROR_FILE_FAILED;

    u64 rom_file_size = get_file_size(rom_fp);
    if (rom_file_size == 0) {
        fclose(rom_fp);
        return ROM_ERROR_FILE_FAILED;
    }

    u8* data = (u8*)malloc(rom_file_size);
    if (!data) {
        fclose(rom_fp);
        return ROM_ERROR_FILE_FAILED;
    }

    if (fread(data, sizeof(u8), rom_file_size, rom_fp) != rom_file_size) {
        free(data);
        fclose(rom_fp);
        return ROM_ERROR_FILE_FAILED;
    }

    fclose(rom_fp);

    if (!validate_rom_header((gba_cartridge_header_t*)data)) {
        free(data);
        return ROM_ERROR_INVALID_ROM;
    }

    if (rom_file_size > ROM_MAX_SIZE) {
        free(data);
        return ROM_ERROR_INVALID_SIZE;
    }

    *data_out = data;
    *size_out = rom_file_size;

    return ROM_OK;
}

void get_rom_title(u8* cartridge, char* buffer, u64 buffer_size) {
    if (buffer_size < 13)
        return;

    gba_cartridge_header_t* header = (gba_cartridge_header_t*)cartridge;

    memcpy(buffer, header->game_title, sizeof(header->game_title));
    buffer[12] = '\0';
}

const char* rom_status_to_text(u32 status) {
    switch (status) {
        case ROM_BUSY: return "ROM BUSY";
        case ROM_OK: return "ROM OK";
        case ROM_ERROR_FILE_FAILED: return "FAILED TO READ FILE";
        case ROM_ERROR_INVALID_ROM: return "INVALID ROM";
        case ROM_ERROR_INVALID_SIZE: return "INVALID ROM SIZE";
        default: return "UNKNOWN ROM ERROR";
    }
}