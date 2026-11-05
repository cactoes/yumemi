#include <string.h>
#include <stdio.h>
#include "sram.h"
#include "helper.h"

static sram_t global_sram;

static
bool check_signature(u8* rom, u64 size) {
    static const char* signatures[] = { "SRAM_V", "SRAM_F_V" };
    static const u64 signature_count = sizeof(signatures) / sizeof(char*);

    if (!rom || size < signature_count)
        return false;

    for (u64 i = 0; i < signature_count; i++) {
        u64 length = strlen(signatures[i]);
        for (u64 offset = 0; offset <= size - (u64)length; offset++) {
            if (memcmp(rom + offset, signatures[i], length) == 0)
                return true;
        }
    }

    return false;
}

static
void sram_make_sav_path(const char* rom_path, char* out, u64 size) {
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
void sram_load() {
    FILE* fp = fopen(global_sram.sav_path, "rb");
    if (!fp)
        return;

    u64 n = fread(global_sram.data, 1, SRAM_CAPACITY, fp);
    fclose(fp);
    if (n > 0)
        return;

    memset(global_sram.data, 0xff, SRAM_CAPACITY);
}

void sram_reset(const char* rom_path, u8* rom, u64 rom_size) {
    memset(&global_sram, 0, sizeof(sram_t));
    memset(global_sram.data, 0xff, SRAM_CAPACITY);

    global_sram.enabled = check_signature(rom, rom_size);
    if (!global_sram.enabled)
        return;

    sram_make_sav_path(rom_path, global_sram.sav_path, SRAM_MAX_PATH);
    printf("sram save path: %s\n", global_sram.sav_path);
    global_sram.persistence_allowed = true;
    sram_load();
}

bool sram_is_enabled() {
    return global_sram.enabled;
}

bool sram_is_dirty() {
    return global_sram.enabled && global_sram.dirty;
}

void sram_write8(u32 address, u8 value, u8 flags) {
    if (!global_sram.enabled || (flags & MEM_ACCESS_NOSIDE_EFFECT))
        return;

    u32 offset = address & (SRAM_CAPACITY - 1u);
    if (global_sram.data[offset] != value) {
        global_sram.data[offset] = value;
        global_sram.dirty = true;
    }
}

u8 sram_read8(u32 address, u8 flags) {
    (void)flags;
    if (!global_sram.enabled)
        return 0xff;

    return global_sram.data[address & (SRAM_CAPACITY - 1u)];
}

bool sram_save() {
    if (!global_sram.enabled || !global_sram.dirty)
        return true;

    if (!global_sram.persistence_allowed)
        return false;

    char tmp[512];
    snprintf(tmp, sizeof(tmp), "%s.tmp", global_sram.sav_path);

    FILE* fp = fopen(tmp, "wb");
    if (!fp)
        return false;

    u64 n = fwrite(global_sram.data, 1, SRAM_CAPACITY, fp);
    fclose(fp);
    if (!(n > 0))
        return false;

    remove(global_sram.sav_path);

    if (rename(tmp, global_sram.sav_path) == 0) {
        global_sram.dirty = false;
        return true;
    }

    return false;
}