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
    cpu_t* cpu = get_cpu();
    memset(cpu, 0, sizeof(cpu_t));

    cpu->skip_bios_mode = skip_bios;

    if (!skip_bios) {
        cpu->cpsr = PSR_MODE_SUPERVISOR | PSR_IRQ_DISABLE | PSR_FIQ_DISABLE;
        cpu->pc = 0;
        return;
    }

    cpu->cpsr = PSR_MODE_SYSTEM;
    cpu->pc = CPU_START_ADDRESS;
    cpu->sp = 0x03007f00u;

    cpu->bank_sp[cpu_mode_to_bank(PSR_MODE_SYSTEM)] = cpu->sp;
    cpu->bank_sp[cpu_mode_to_bank(PSR_MODE_SUPERVISOR)] = 0x03007FE0u;
    cpu->bank_sp[cpu_mode_to_bank(PSR_MODE_IRQ)] = 0x03007FA0u;

    mbus_set_bios_open_bus(0xe129f000u);
}

bool cpu_fetch() {
    cpu_t* cpu = get_cpu();

    u32 instruction_address = cpu->pc;
    u32 instruction = mbus_read32(instruction_address, MBUS_DEFAULT);
    cpu->pc += 4;

    u32 prefetched = mbus_read32(instruction_address + 8, MBUS_NOCOUNT | MBUS_NOSIDE_EFFECT);
    mbus_set_cpu_open_bus(prefetched);

    // check
    instruction;

    // decode

    return false;
}

bool cpu_step(u32* cycles) {
    mbus_get_total_cycle_count(true);

    // irq checks

    bool result = cpu_fetch();
    u32 c = mbus_get_total_cycle_count(true);
    *cycles = c ? c : 1;
    return result;
}