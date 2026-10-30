#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#include "cpu.h"
#include "memory_bus.h"

static cpu_t global_cpu;

static
cpu_t* get_cpu() {
    return &global_cpu;
}

void cpu_reset(bool skip_bios) {
    skip_bios;
}

bool cpu_fetch() {
    cpu_t* cpu = get_cpu();

    u32 instruction_address = cpu->pc;
    u32 instruction = mbus_read32(instruction_address, MBUS_DEFAULT);
    cpu->pc += 4;

    u32 prefetched = mbus_read32(instruction_address + 8, MBUS_NOCOUNT | MBUS_NOSIDE_EFFECT);
    mbus_set_cpu_open_bus(prefetched);

    // check

    // decode

    return false;
}

bool cpu_step(u32* cycles) {
    cpu_t* cpu = get_cpu();
    mbus_get_cycle_count(true);

    // irq checks

    bool result = cpu_fetch();
    u32 cycles = mbus_get_total_cycle_count(true);
    *cycles = cycles ? cycles : 1;
    return result;
}