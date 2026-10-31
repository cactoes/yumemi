#include <stdio.h>

#include "rom.h"
#include "cpu.h"
#include "memory_bus.h"
#include "flash.h"
#include "timer.h"
#include "gba_io.h"
#include "dma.h"
#include "ppu.h"
#include "renderer.h"

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
    dma_reset();
    io_reset();
    flash_reset(ROM_FILE_PATH);
    timer_reset();
    ppu_reset();
    cpu_reset(true);

    render_init(PPU_WIDTH, PPU_HEIGHT);
    render_update_events();

    while (!render_should_close()) {
        static u32 counter = 0;
        if ((counter++ & 0xfff) == 0)
            render_poll_events();

        u32 cycles;

        if (dma_is_active()) {
            dma_step(&cycles);
        } else {
            if (!io_cpu_handle_irq(&cycles)) {
                if (!cpu_step(&cycles)) {
                    printf("~~~ instruction failed (exiting) ~~~\n");
                    break;
                }
            }
        }

        ppu_step(cycles);
        timer_step(cycles);

        render_handle_frame();
    }

    render_close();
    // free(rom_data)

    return 0;
}