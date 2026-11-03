#include <stdio.h>
#include <stdlib.h>
#include <raylib.h>
#undef bool

#include "rom.h"
#include "cpu.h"
#include "memory_bus.h"
#include "flash.h"
#include "timer.h"
#include "gba_io.h"
#include "dma.h"
#include "ppu.h"
#include "renderer.h"
#include "wtime.h"

char* rom_path = 0;

int main() {
    render_init(PPU_WIDTH, PPU_HEIGHT);

    SetTargetFPS(60);

    while (rom_path == 0) {
        if (WindowShouldClose())
            return 0;

        render_draw_intro();
    }

    SetTargetFPS(0);

    u8* rom_data = 0;
    u64 rom_size = 0;
    u32 rom_load_result = load_rom_from_path(rom_path, &rom_data, &rom_size);
    if (rom_load_result != 0) {
        printf("failed to load rom: %d (%s)\n", rom_load_result, rom_path);
        free(rom_path);
        return 1;
    }

    char title[13];
    get_rom_title(rom_data, title, sizeof(title));
    printf("loaded: %s\n", title);

    memory_bus_reset(rom_data, rom_size);
    dma_reset();
    io_reset();
    flash_reset(rom_path);
    timer_reset();
    ppu_reset();
    cpu_reset(true);

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
    free(rom_path);
    // free(rom_data)

    return 0;
}