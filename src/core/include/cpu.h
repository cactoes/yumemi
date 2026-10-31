//==========================================
/// @file       cpu.h
/// @brief      ARM7TDMI - ARMv4T architecture
//==========================================

#pragma once

#ifndef CPU_H
#define CPU_H

#define CPU_IRQ_APPROX_OVERHEAD 3u

#define CPU_START_ADDRESS   0x08000000u
#define IRQ_VECTOR          0x00000018
#define BIOS_IRQ_RETURN     0x00000138u
#define IRQ_USER_HANDLER    0x03007FFCu

#define PSR_N (1u << 31)
#define PSR_Z (1u << 30)
#define PSR_C (1u << 29)
#define PSR_V (1u << 28)
#define PSR_Q (1u << 27)
#define PSR_I (1u << 7)
#define PSR_F (1u << 6)
#define PSR_T (1u << 5)

#define PSR_MODE_USER       0x10u
#define PSR_MODE_FIQ        0x11u
#define PSR_MODE_IRQ        0x12u
#define PSR_MODE_SUPERVISOR 0x13u
#define PSR_MODE_ABORT      0x17u
#define PSR_MODE_UNDEFINED  0x1bu
#define PSR_MODE_SYSTEM     0x1fu

#define PSR_MODE_MASK       0x1fu
#define PSR_FLAGS_MASK      0xf0000000u
#define PSR_T_BIT           (1u << 5)
#define PSR_IRQ_DISABLE     (1u << 7)
#define PSR_FIQ_DISABLE     (1u << 6)

#define PSR_COND_EQ    0x0u
#define PSR_COND_NE    0x1u
#define PSR_COND_CS_HS 0x2u
#define PSR_COND_CC_LO 0x3u
#define PSR_COND_MI    0x4u
#define PSR_COND_PL    0x5u
#define PSR_COND_VS    0x6u
#define PSR_COND_VC    0x7u
#define PSR_COND_HI    0x8u
#define PSR_COND_LS    0x9u
#define PSR_COND_GE    0xau
#define PSR_COND_LT    0xbu
#define PSR_COND_GT    0xcu
#define PSR_COND_LE    0xdu
#define PSR_COND_AL    0xeu

#define SWI_SOFT_RESET              0x00
#define SWI_REGISTER_RAM_RESET      0x01
#define SWI_HALT                    0x02
#define SWI_INTR_WAIT               0x04
#define SWI_VBLANK_INTR_WAIT        0x05
#define SWI_CPU_SET                 0x0b
#define SWI_CPU_FAST_SET            0x0c
#define SWI_OBJ_AFFINE_SET          0x0f
#define SWI_LZ77_UNCOMPRESS_WRAM    0x11
#define SWI_LZ77_UNCOMPRESS_VRAM    0x12

#define ALU_SHIFT_LSL 0
#define ALU_SHIFT_LSR 1
#define ALU_SHIFT_ASR 2
#define ALU_SHIFT_ROR 3

#define ARM_DM_BX_MASK           0x0ffffff0u
#define ARM_DM_BX_VALUE          0x012fff10u
#define ARM_DM_MUL_MASK          0x0fc000f0u
#define ARM_DM_MUL_VALUE         0x00000090u
#define ARM_DM_MULL_MASK         0x0f8000f0u
#define ARM_DM_MULL_VALUE        0x00800090u
#define ARM_DM_SWP_MASK          0x0fb00ff0u
#define ARM_DM_SWP_VALUE         0x01000090u
#define ARM_DM_HALF_REG_MASK     0x0E400f90u
#define ARM_DM_HALF_REG_VALUE    0x00000090u
#define ARM_DM_HALF_IMM_MASK     0x0E400090u
#define ARM_DM_HALF_IMM_VALUE    0x00400090u
#define ARM_DM_MRS_MASK          0x0fbf0fffu
#define ARM_DM_MRS_VALUE         0x010f0000u
#define ARM_DM_MSR_REG_MASK      0x0fb0fff0u
#define ARM_DM_MSR_REG_VALUE     0x0120f000u
#define ARM_DM_MSR_IMM_MASK      0x0fb0f000u
#define ARM_DM_MSR_IMM_VALUE     0x0320f000u

#define THUMB_FMT_ADD_SUB_MASK            0xf800u
#define THUMB_FMT_ADD_SUB_VALUE           0x1800u
#define THUMB_FMT_ALU_MASK                0xfc00u
#define THUMB_FMT_ALU_VALUE               0x4000u
#define THUMB_FMT_HIGH_REGISTER_MASK      0xfc00u
#define THUMB_FMT_HIGH_REGISTER_VALUE     0x4400u
#define THUMB_FMT_LITERAL_LOAD_MASK       0xf800u
#define THUMB_FMT_LITERAL_LOAD_VALUE      0x4800u
#define THUMB_FMT_REGISTER_TRANSFER_MASK  0xf200u
#define THUMB_FMT_REGISTER_TRANSFER_VALUE 0x5000u
#define THUMB_FMT_SIGNED_TRANSFER_MASK    0xf200u
#define THUMB_FMT_SIGNED_TRANSFER_VALUE   0x5200u
#define THUMB_FMT_LOAD_ADDRESS_MASK       0xf000u
#define THUMB_FMT_LOAD_ADDRESS_VALUE      0xa000u
#define THUMB_FMT_ADJUST_SP_MASK          0xff00u
#define THUMB_FMT_ADJUST_SP_VALUE         0xb000u
#define THUMB_FMT_PUSH_POP_MASK           0xf600u
#define THUMB_FMT_PUSH_POP_VALUE          0xb400u
#define THUMB_FMT_BRANCH_MASK             0xf800u
#define THUMB_FMT_BRANCH_VALUE            0xe000u
#define THUMB_FMT_BL_PREFIX_MASK          0xf800u
#define THUMB_FMT_BL_PREFIX_VALUE         0xf000u
#define THUMB_FMT_BL_SUFFIX_MASK          0xf800u
#define THUMB_FMT_BL_SUFFIX_VALUE         0xf800u

#define THUMB_MEMORY_WORD               0
#define THUMB_MEMORY_BYTE               1
#define THUMB_MEMORY_HALFWORD           2
#define THUMB_MEMORY_SIGNED_BYTE        3
#define THUMB_MEMORY_SIGNED_HALFWORD    4

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

typedef struct __alu_shift_result {
    u32 value;
    bool carry;
} alu_shift_result_t;

typedef struct __alu_add_result {
    u32 value;
    bool carry;
    bool overflow;
} alu_add_result_t;

typedef bool(*arm_instruction_handler_t)(u32 instruction);
typedef bool(*thumb_instruction_handler_t)(u16 instruction);

void cpu_reset(bool skip_bios);
bool cpu_step(u32* cycles);

#endif // CPU_H