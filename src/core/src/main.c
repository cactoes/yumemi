#include <stdio.h>

#include "rom.h"
#include "cpu.h"
#include "memory_bus.h"

#define ROM_FILE_PATH "../../roms/Pokemon - FireRed Version (USA, Europe) (Rev 1).gba"

int main() {
    u8* rom_data = 0;
    u64 rom_size = 0;
    u32 rom_load_result = load_rom_from_path(ROM_FILE_PATH, &rom_data, &rom_size);
    if (rom_load_result != 0) {
        printf("failed to load rom: %d\n", rom_load_result);
        return 1;
    }

    char title[13];
    get_rom_title(rom_data, title, sizeof(title));
    printf("loaded: %s\n", title);

    memory_bus_reset(rom_data, rom_size);
    cpu_reset(true);

    while (true) {
        u32 cycles;

        if (!cpu_step(&cycles)) {
            printf("~~~ instruction failed (exiting) ~~~\n");
            break;
        }
    }

    return 0;
}