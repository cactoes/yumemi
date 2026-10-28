//==========================================
/// @file       cpu.h
/// @brief      ARM7TDMI - ARMv4T architecture
//==========================================

#pragma once

#ifndef CPU_H
#define CPU_H

#include "typedef.h"

typedef struct __cpu {
    union __registers {
        struct __typedef_registers {
            u32 r0, r1, r2, r3, r4, r5, r6, r7;
            u32 r8, r9, r10, r11, r12;
            union __register_sp { u32 sp, r13; };
            union __register_lr { u32 lr, r14; };
            union __register_pc { u32 pc, r15; };
        };

        u32 r[16];
    };

    u32 cpsr;

    u32 spsr[6];
    u32 bank_sp[6];
    u32 bank_lr[6];
    u32 usr_r8_r12[5];
    u32 fiq_r8_r12[5];

    bool skip_bios_mode;
} cpu_t;

typedef bool(*arm_instruction_handler_t)(u32 instruction);
typedef bool(*thumb_instruction_handler_t)(u16 instruction);

void cpu_reset(bool skip_bios);
bool cpu_step(u32* cycles);

#endif // CPU_H