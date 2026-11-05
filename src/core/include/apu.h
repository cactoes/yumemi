//==========================================
/// @file       apu.h
/// @brief      
//==========================================

#pragma once

#ifndef APU_HP
#define APU_HP

typedef struct __apu {
    int as;
} apu_t;

#include "typedef.h"

void apu_reset();
void apu_step(u32 cycles);

#endif // APU_HP