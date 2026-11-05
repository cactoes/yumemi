#include <string.h>

#include "apu.h"

static apu_t global_apu;

void apu_reset() {
    memset(&global_apu, 0, sizeof(apu_t));
}

void apu_step(u32 cycles) {
    cycles;
}