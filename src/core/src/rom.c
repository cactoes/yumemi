#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <miniz.h>
#include <ctype.h>

#include "rom.h"
#include "memory_bus.h"

static
bool rom_has_extension(const char* path, const char* extension) {
    const char* dot = strrchr(path, '.');
    if (!dot || strlen(dot) != strlen(extension))
        return false;

    while (*dot) {
        if (tolower((unsigned char)*dot) != tolower((unsigned char)*extension)) {
            return false;
        }
        dot++;
        extension++;
    }

    return true;
}

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

static
u32 load_rom_from_zip(const char* path, u8** data_out, u64* size_out) {
    mz_zip_archive zip = { 0 };
    if (!mz_zip_reader_init_file(&zip, path, 0))
        return ROM_ERROR_FILE_FAILED;

    mz_uint file_count = mz_zip_reader_get_num_files(&zip);

    for (mz_uint i = 0; i < file_count && i < ROM_SEARCH_MAX_COUNT; i++) {
        mz_zip_archive_file_stat file;
        if (!mz_zip_reader_file_stat(&zip, i, &file)) {
            mz_zip_reader_end(&zip);
            return ROM_ERROR_FILE_FAILED;
        }

        if (file.m_is_directory)
            continue;

        if (mz_zip_reader_get_filename(&zip, i, NULL, 0) > sizeof(file.m_filename))
            continue;

        if (!rom_has_extension(file.m_filename, ".gba"))
            continue;

        u64 size = (u64)file.m_uncomp_size;
        if (size < sizeof(gba_cartridge_header_t) || size > ROM_MAX_SIZE) {
            mz_zip_reader_end(&zip);
            return ROM_ERROR_INVALID_SIZE;
        }

        u8* data = (u8*)malloc((u64)size);
        if (!data) {
            mz_zip_reader_end(&zip);
            return ROM_ERROR_FILE_FAILED;
        }

        bool extracted = (bool)mz_zip_reader_extract_to_mem(&zip, i, data, (u64)size, 0);
        mz_zip_reader_end(&zip);

        if (!extracted) {
            free(data);
            return ROM_ERROR_FILE_FAILED;
        }

        *data_out = data;
        *size_out = size;
        return ROM_OK;
    }

    mz_zip_reader_end(&zip);
    return ROM_ERROR_INVALID_ROM;
}

u32 load_rom_from_path(const char* path, u8** data_out, u64* size_out) {
    if (!path || !data_out || !size_out)
        return ROM_ERROR_FILE_FAILED;

    *data_out = NULL;
    *size_out = 0;

    u8* data = NULL;
    u64 size = 0;

    if (rom_has_extension(path, ".zip")) {
        u32 result = load_rom_from_zip(path, &data, &size);
        if (result != ROM_OK) {
            return result;
        }
    } else {
        FILE* rom_fp = fopen(path, "rb");
        if (!rom_fp) {
            return ROM_ERROR_FILE_FAILED;
        }

        size = get_file_size(rom_fp);
        if (size < sizeof(gba_cartridge_header_t) || size > ROM_MAX_SIZE) {
            fclose(rom_fp);
            return ROM_ERROR_INVALID_SIZE;
        }

        if (fseek(rom_fp, 0, SEEK_SET) != 0) {
            fclose(rom_fp);
            return ROM_ERROR_FILE_FAILED;
        }

        data = (u8*)malloc((u64)size);
        if (!data) {
            fclose(rom_fp);
            return ROM_ERROR_FILE_FAILED;
        }

        u64 bytes_read = fread(data, 1, (u64)size, rom_fp);
        fclose(rom_fp);

        if (bytes_read != (u64)size) {
            free(data);
            return ROM_ERROR_FILE_FAILED;
        }
    }

    if (!validate_rom_header((gba_cartridge_header_t*)data)) {
        free(data);
        return ROM_ERROR_INVALID_ROM;
    }

    *data_out = data;
    *size_out = size;
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

void rom_release() {
    memory_bus_reset(NULL, 0);
}