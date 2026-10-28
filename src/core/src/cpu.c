#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#include "cpu.h"

static cpu_t global_cpu;

static
cpu_t* get_cpu() {
    return &global_cpu;
}

void cpu_reset(bool skip_bios) {
    skip_bios;
}

bool cpu_step(u32* cycles) {
    *cycles = 0;
    return false;
}