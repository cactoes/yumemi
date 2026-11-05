#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <raylib.h>
#include <threads.h>
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
#include "wwindow.h"
#include "eeprom.h"
#include "sram.h"
#include "apu.h"

static u32 rom_load_status = ROM_OK;
static mtx_t rom_mutex;
char* rom_path = 0;

static
u32 load_rom(const char* path) {
    u8* rom_data = 0;
    u64 rom_size = 0;
    u32 rom_load_result = load_rom_from_path(path, &rom_data, &rom_size);
    if (rom_load_result != 0) {
        printf("failed to load rom: %d (%s)\n", rom_load_result, path);
        return rom_load_result;
    }

    char title[13];
    get_rom_title(rom_data, title, sizeof(title));
    printf("loaded: %s\n", title);

    memory_bus_reset(rom_data, rom_size);
    eeprom_reset(path, rom_data, rom_size);
    dma_reset();
    io_reset();
    sram_reset(path, rom_data, rom_size);
    if (!sram_is_enabled())
        flash_reset(path);
    timer_reset();
    ppu_reset();
    apu_reset();
    cpu_reset(true);

    return ROM_OK;
}

static
int worker_load_rom(void* data) {
    u32 result = load_rom((const char*)data);

    struct timespec delay = { 0, 400000000 };
    thrd_sleep(&delay, 0);

    mtx_lock(&rom_mutex);
    rom_load_status = result;
    mtx_unlock(&rom_mutex);

    return 0;
}

static
int wait_for_rom() {
    thrd_t thread = { 0 };
    const char* error_text = 0;
    int loading = 0;
    int result = 0;

    if (mtx_init(&rom_mutex, mtx_plain) != thrd_success) {
        printf("failed to create rom mutex\n");
        return -1;
    }

    rom_load_status = ROM_OK;
    SetTargetFPS(60);

    while (result == 0 && !render_should_close()) {
        if (loading) {
            mtx_lock(&rom_mutex);
            u32 status = rom_load_status;
            mtx_unlock(&rom_mutex);

            if (status != ROM_BUSY) {
                thrd_join(thread, 0);
                loading = 0;

                if (status == ROM_OK) {
                    result = 1;
                    break;
                }

                error_text = rom_status_to_text(status);
                free(rom_path);
                rom_path = 0;
            }
        } else if (rom_path != 0) {
            rom_load_status = ROM_BUSY;
            error_text = 0;

            if (thrd_create(&thread, worker_load_rom, rom_path) != thrd_success) {
                printf("failed to create rom loading thread\n");
                result = -1;
                break;
            }

            loading = 1;
        }

        if (loading)
            render_draw_loading_rom();
        else if (error_text != 0)
            render_draw_error(error_text);
        else
            render_draw_intro();
    }

    if (loading)
        thrd_join(thread, 0);

    if (result != 1) {
        free(rom_path);
        rom_path = 0;
    } else {
        SetTargetFPS(0);
    }

    mtx_destroy(&rom_mutex);
    return result;
}

int main(int argc, char** argv) {
    render_init(PPU_WIDTH, PPU_HEIGHT);

    if (argc > 1)
        rom_path = _strdup(argv[1]);

    if (wait_for_rom() != 1) {
        CloseWindow();
        return 1;
    }

    apu_enable_audio();
    disable_import_rom();
    render_update_events();

    while (!render_should_close()) {
        static u32 counter = 0;
        if ((counter++ & 0xfff) == 0) {
            render_poll_events();
            apu_audio_pump();
        }

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
        eeprom_step(cycles);

        render_handle_frame();
    }

    apu_audio_close();
    render_close();
    free(rom_path);
    // free(rom_data)

    return 0;
}