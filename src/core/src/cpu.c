#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#include "cpu.h"
#include "memory_bus.h"
#include "helper.h"
#include "gba_io.h"

static cpu_t global_cpu;

static
cpu_t* get_cpu() {
    return &global_cpu;
}

static
u32 cpu_mode_to_bank(u32 mode) {
    switch (mode) {
        case PSR_MODE_USER:
        case PSR_MODE_SYSTEM:      return 0;
        case PSR_MODE_FIQ:         return 1;
        case PSR_MODE_IRQ:         return 2;
        case PSR_MODE_SUPERVISOR:  return 3;
        case PSR_MODE_ABORT:       return 4;
        case PSR_MODE_UNDEFINED:   return 5;
        default: return 0;
    }
}

static
u32 mul_internal_cycles(u32 multiplier) {
    if (!MASK_ANY_SET(multiplier, 0xffffff00u) || MASK_IS_SET(multiplier, 0xffffff00u))
        return 1;

    if (!MASK_ANY_SET(multiplier, 0xffff0000u) || MASK_IS_SET(multiplier, 0xffff0000u))
        return 2;

    if (!MASK_ANY_SET(multiplier, 0xff000000u) || MASK_IS_SET(multiplier, 0xff000000u))
        return 3;

    return 4;
}

static
u32 umul_internal_cycles(u32 multiplier) {
    if (!MASK_ANY_SET(multiplier, 0xffffff00u))
        return 1;

    if (!MASK_ANY_SET(multiplier, 0xffff0000u))
        return 2;

    if (!MASK_ANY_SET(multiplier, 0xff000000u))
        return 3;

    return 4;
}

static
u32 cpu_current_mode() {
    cpu_t* cpu = get_cpu();
    return (cpu->cpsr & PSR_MODE_MASK);
}

static
bool is_thumb_mode() {
    cpu_t* cpu = get_cpu();
    return (cpu->cpsr & PSR_T_BIT) != 0;
}

static
bool irq_should_fire() {
    cpu_t* cpu = get_cpu();

    u16 ie = io_read16(IO_IE, IO_NOCOUNT | IO_NOSIDE_EFFECT);
    u16 iflags = io_read16(IO_IF, IO_NOCOUNT | IO_NOSIDE_EFFECT);
    u16 ime = io_read16(IO_IME, IO_NOCOUNT | IO_NOSIDE_EFFECT) & 1;

    return ime && (ie & iflags) && !(cpu->cpsr & PSR_I);
}

static
bool cpu_check_instruction_cond(u32 cond, u32 cpsr) {
    bool N = (bool)MASK_ANY_SET(cpsr, PSR_N);
    bool Z = (bool)MASK_ANY_SET(cpsr, PSR_Z);
    bool C = (bool)MASK_ANY_SET(cpsr, PSR_C);
    bool V = (bool)MASK_ANY_SET(cpsr, PSR_V);

    switch (cond) {
        case PSR_COND_EQ: return Z;
        case PSR_COND_NE: return !Z;
        case PSR_COND_CS_HS: return C;
        case PSR_COND_CC_LO: return !C;
        case PSR_COND_MI: return N;
        case PSR_COND_PL: return !N;
        case PSR_COND_VS: return V;
        case PSR_COND_VC: return !V;
        case PSR_COND_HI: return C && !Z;
        case PSR_COND_LS: return !C || Z;
        case PSR_COND_GE: return N == V;
        case PSR_COND_LT: return N != V;
        case PSR_COND_GT: return !Z && (N == V);
        case PSR_COND_LE: return Z || (N != V);
        case PSR_COND_AL: return true;
        default: return false;
    }
}

static
u32* cpu_current_spsr() {
    cpu_t* cpu = get_cpu();

    u32 bank = cpu_mode_to_bank(cpu_current_mode());
    return bank == 0 ? 0 : &cpu->spsr[bank];
}

static
void cpu_switch_mode(u32 new_mode) {
    cpu_t* cpu = get_cpu();

    u32 old_mode = cpu_current_mode();
    u32 old_bank = cpu_mode_to_bank(old_mode);
    u32 new_bank = cpu_mode_to_bank(new_mode);

    if (old_bank == new_bank)
        return;

    cpu->bank_sp[old_bank] = cpu->sp;
    cpu->bank_lr[old_bank] = cpu->lr;

    bool old_fiq = old_mode == PSR_MODE_FIQ;
    bool new_fiq = new_mode == PSR_MODE_FIQ;

    if (old_fiq != new_fiq) {
        u32* save = old_fiq ? cpu->fiq_r8_r12 : cpu->usr_r8_r12;
        u32* load = new_fiq ? cpu->fiq_r8_r12 : cpu->usr_r8_r12;

        for (int i = 0; i < 5; i++) {
            save[i] = cpu->r[8 + i];
            cpu->r[8 + i] = load[i];
        }
    }

    cpu->sp = cpu->bank_sp[new_bank];
    cpu->lr = cpu->bank_lr[new_bank];
}

static
void cpu_set_mode(u32 mode) {
    cpu_t* cpu = get_cpu();
    cpu_switch_mode(mode);
    cpu->cpsr = (cpu->cpsr & ~PSR_MODE_MASK) | mode;
}

static
u32 cpu_read_reg(u32 n) {
    cpu_t* cpu = get_cpu();

    if (n == 15)
        return is_thumb_mode() ? cpu->pc + 2 : cpu->pc + 4;

    return cpu->r[n];
}

static
void cpu_enter_irq() {
    cpu_t* cpu = get_cpu();

    u32 old_cpsr = cpu->cpsr;
    u32 return_addr = cpu->pc + 4;
    u32 irq_bank = cpu_mode_to_bank(PSR_MODE_IRQ);

    cpu_switch_mode(PSR_MODE_IRQ);
    cpu->cpsr = (old_cpsr & ~(PSR_MODE_MASK | PSR_T_BIT)) | PSR_MODE_IRQ | PSR_I;
    cpu->spsr[irq_bank] = old_cpsr;
    cpu->lr = return_addr;

    if (!cpu->skip_bios_mode) {
        cpu->pc = IRQ_VECTOR;
        return;
    }

    mbus_set_bios_open_bus(0xe25ef004u);

    cpu->sp -= 24;
    mbus_write32(cpu->sp + 0, cpu->r[0], MBUS_DEFAULT);
    mbus_write32(cpu->sp + 4, cpu->r[1], MBUS_DEFAULT);
    mbus_write32(cpu->sp + 8, cpu->r[2], MBUS_DEFAULT);
    mbus_write32(cpu->sp + 12, cpu->r[3], MBUS_DEFAULT);
    mbus_write32(cpu->sp + 16, cpu->r[12], MBUS_DEFAULT);
    mbus_write32(cpu->sp + 20, cpu->lr, MBUS_DEFAULT);

    u32 handler = mbus_read32(IRQ_USER_HANDLER, MBUS_DEFAULT);

    cpu->r[0] = 0x04000000u;
    cpu->lr = BIOS_IRQ_RETURN;
    cpu->pc = handler & ~3u;
}

static
void cpu_leave_irq_hle() {
    cpu_t* cpu = get_cpu();

    mbus_set_bios_open_bus(0xe55eC002u);

    cpu->r[0] = mbus_read32(cpu->sp + 0, MBUS_DEFAULT);
    cpu->r[1] = mbus_read32(cpu->sp + 4, MBUS_DEFAULT);
    cpu->r[2] = mbus_read32(cpu->sp + 8, MBUS_DEFAULT);
    cpu->r[3] = mbus_read32(cpu->sp + 12, MBUS_DEFAULT);
    cpu->r[12] = mbus_read32(cpu->sp + 16, MBUS_DEFAULT);
    u32 saved_lr = mbus_read32(cpu->sp + 20, MBUS_DEFAULT);
    cpu->sp += 24;

    u32 restored_cpsr = cpu->spsr[cpu_mode_to_bank(PSR_MODE_IRQ)];
    u32 return_pc = saved_lr - 4;

    cpu->lr = saved_lr;
    cpu_switch_mode(restored_cpsr & PSR_MODE_MASK);
    cpu->cpsr = restored_cpsr;
    cpu->pc = return_pc;
}

static
void cpu_set_pc(u32 target) {
    cpu_t* cpu = get_cpu();
    cpu->pc = target;
    mbus_add_cycles_from_address(target, is_thumb_mode() ? 2 : 4);
}

static
u32 cpu_count_register_list(u32 list) {
    u32 count = 0;

    for (u32 reg = 0; reg < 16; reg++) {
        if (MASK_ANY_SET(list, 1u << reg))
            count++;
    }

    return count;
}

static
u32 ror(u32 value, u32 amount) {
    amount &= 31;

    if (amount == 0)
        return value;

    return (value >> amount) | (value << (32 - amount));
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
    cpu->bank_sp[cpu_mode_to_bank(PSR_MODE_SUPERVISOR)] = 0x03007fe0u;
    cpu->bank_sp[cpu_mode_to_bank(PSR_MODE_IRQ)] = 0x03007fa0u;

    mbus_set_bios_open_bus(0xe129f000u);
}

static
u32 cpu_load_word_rotated(u32 address) {
    u32 word = mbus_read32(address & ~3u, MBUS_DEFAULT);
    return ror(word, (address & 3u) * 8);
}

static
u32 arm_read_store_reg(u32 reg) {
    u32 value = cpu_read_reg(reg);
    return reg == 15 ? value + 4 : value;
}

static
alu_shift_result_t alu_barrel_shift(u32 value, u32 type, u32 amount, bool carry_in, bool mode_register) {
    alu_shift_result_t result;
    result.value = value;
    result.carry = carry_in;

    amount &= mode_register ? 0xff : 0x1f;

    if (amount == 0) {
        if (mode_register || type == ALU_SHIFT_LSL)
            return result;

        if (type == ALU_SHIFT_ROR) {
            result.value = (value >> 1) | (carry_in ? 0x80000000u : 0);
            result.carry = MASK_ANY_SET(value, 1u);
            return result;
        }

        amount = 32;
    }

    switch (type) {
        case ALU_SHIFT_LSL:
            if (amount < 32) {
                result.value = value << amount;
                result.carry = MASK_ANY_SET(value, 1u << (32u - amount));
            } else if (amount == 32u) {
                result.value = 0;
                result.carry = MASK_ANY_SET(value, 1u);
            } else {
                result.value = 0;
                result.carry = false;
            }
            break;
        case ALU_SHIFT_LSR:
            if (amount < 32) {
                result.value = value >> amount;
                result.carry = MASK_ANY_SET(value, 1u << (amount - 1u));
            } else if (amount == 32u) {
                result.value = 0;
                result.carry = MASK_ANY_SET(value, 0x80000000u);
            } else {
                result.value = 0;
                result.carry = false;
            }
            break;
        case ALU_SHIFT_ASR: {
            bool negative = MASK_ANY_SET(value, 0x80000000u);
            if (amount < 32) {
                result.value = value >> amount;
                if (negative)
                    MASK_SET(result.value, 0xffffffffu << (32u - amount));
                result.carry = MASK_ANY_SET(value, 1u << (amount - 1u));
            } else {
                result.value = negative ? 0xffffffffu : 0u;
                result.carry = negative;
            }
            break;
        }
        case ALU_SHIFT_ROR:
            result.value = ror(value, amount);
            result.carry = MASK_ANY_SET(result.value, 0x80000000u);
            break;
    }

    return result;
}

static
alu_add_result_t alu_add_with_carry(u32 a, u32 b, bool carry_in) {
    u64 sum = (u64)a + (u64)b + (carry_in ? 1 : 0);

    alu_add_result_t result;
    result.value = (u32)sum;
    result.carry = MASK_ANY_SET(sum, UINT64_C(1) << 32);
    result.overflow = MASK_ANY_SET(~(a ^ b) & (a ^ result.value), 0x80000000u);
    return result;
}

static
void arm_restore_cpsr_from_spsr() {
    cpu_t* cpu = get_cpu();
    u32 bank = cpu_mode_to_bank(cpu_current_mode());

    if (bank == 0)
        return;

    u32 restored_cpsr = cpu->spsr[bank];

    if (MASK_ANY_SET(cpu->cpsr ^ restored_cpsr, PSR_MODE_MASK))
        cpu_switch_mode(restored_cpsr & PSR_MODE_MASK);

    cpu->cpsr = restored_cpsr;
}

static void arm_update_data_processing_flags(u32 value, bool carry, bool arithmetic, bool overflow) {
    cpu_t* cpu = get_cpu();

    MASK_CLEAR(cpu->cpsr, PSR_N | PSR_Z | PSR_C);

    if (MASK_ANY_SET(value, 0x80000000u))
        MASK_SET(cpu->cpsr, PSR_N);
    if (value == 0u)
        MASK_SET(cpu->cpsr, PSR_Z);
    if (carry)
        MASK_SET(cpu->cpsr, PSR_C);

    if (arithmetic) {
        MASK_CLEAR(cpu->cpsr, PSR_V);
        if (overflow)
            MASK_SET(cpu->cpsr, PSR_V);
    }
}

static
void arm_update_multiply_flags(u32 sign_word, bool zero) {
    cpu_t* cpu = get_cpu();

    MASK_CLEAR(cpu->cpsr, PSR_N | PSR_Z);
    if (MASK_ANY_SET(sign_word, 0x80000000u))
        MASK_SET(cpu->cpsr, PSR_N);
    if (zero)
        MASK_SET(cpu->cpsr, PSR_Z);
}

static
bool arm_decode_operand2(u32 instruction, alu_shift_result_t* operand2, bool* register_shift) {
    cpu_t* cpu = get_cpu();
    bool carry_in = MASK_ANY_SET(cpu->cpsr, PSR_C);
    *register_shift = false;

    if (MASK_ANY_SET(instruction, 1u << 25)) {
        u32 immediate = instruction & 0xffu;
        u32 rotate = ((instruction >> 8) & 0xfu) * 2;

        operand2->value = ror(immediate, rotate);
        operand2->carry = rotate != 0 ? MASK_ANY_SET(operand2->value, 0x80000000u) : carry_in;
        return true;
    }

    u32 rm = instruction & 0xfu;
    u32 type = (instruction >> 5) & 3u;
    u32 amount;
    bool by_register = MASK_ANY_SET(instruction, 1u << 4);

    if (by_register) {
        if (MASK_ANY_SET(instruction, 1u << 7))
            return false;

        u32 rs = (instruction >> 8) & 0xfu;
        if (rs == 15)
            return false;

        amount = cpu_read_reg(rs) & 0xffu;
    } else {
        amount = (instruction >> 7) & 0x1fu;
    }

    u32 value = cpu_read_reg(rm);
    if (by_register && rm == 15)
        value += 4;

    *operand2 = alu_barrel_shift(value, type, amount, carry_in, by_register);
    *register_shift = by_register;

    if (by_register)
        mbus_add_cycles(1);

    return true;
}

static
bool arm_data_processing(u32 instruction) {
    cpu_t* cpu = get_cpu();

    u32 opcode = (instruction >> 21) & 0xfu;
    u32 rn = (instruction >> 16) & 0xfu;
    u32 rd = (instruction >> 12) & 0xfu;
    bool set_flags = MASK_ANY_SET(instruction, 1u << 20);
    bool test = opcode >= 8 && opcode <= 11;

    if (test && !set_flags)
        return false;

    alu_shift_result_t operand2;
    bool register_shift;
    if (!arm_decode_operand2(instruction, &operand2, &register_shift))
        return false;

    u32 a = cpu_read_reg(rn);
    if (register_shift && rn == 15)
        a += 4;

    u32 b = operand2.value;
    bool carry_in = MASK_ANY_SET(cpu->cpsr, PSR_C);
    bool arithmetic = false;
    alu_add_result_t added = { 0 };
    u32 result;

    switch (opcode) {
        case 0:
        case 8:
            result = a & b;
            break;
        case 1:
        case 9:
            result = a ^ b;
            break;
        case 2:
        case 10:
            added = alu_add_with_carry(a, ~b, true);
            arithmetic = true;
            result = added.value;
            break;
        case 3:
            added = alu_add_with_carry(b, ~a, true);
            arithmetic = true;
            result = added.value;
            break;
        case 4:
        case 11:
            added = alu_add_with_carry(a, b, false);
            arithmetic = true;
            result = added.value;
            break;
        case 5:
            added = alu_add_with_carry(a, b, carry_in);
            arithmetic = true;
            result = added.value;
            break;
        case 6:
            added = alu_add_with_carry(a, ~b, carry_in);
            arithmetic = true;
            result = added.value;
            break;
        case 7:
            added = alu_add_with_carry(b, ~a, carry_in);
            arithmetic = true;
            result = added.value;
            break;
        case 12:
            result = a | b;
            break;
        case 13:
            result = b;
            break;
        case 14:
            result = a & ~b;
            break;
        case 15:
            result = ~b;
            break;
        default:
            return false;
    }

    if (!test && rd == 15) {
        if (set_flags)
            arm_restore_cpsr_from_spsr();

        MASK_CLEAR(result, MASK_ANY_SET(cpu->cpsr, PSR_T_BIT) ? 1 : 3);
        cpu_set_pc(result);
        return true;
    }

    if (!test)
        cpu->r[rd] = result;

    if (set_flags) {
        bool carry = arithmetic ? added.carry : operand2.carry;
        bool overflow = arithmetic ? added.overflow : false;
        arm_update_data_processing_flags(result, carry, arithmetic, overflow);
    }

    return true;
}

static
bool arm_block_data_transfer(u32 instruction) {
    cpu_t* cpu = get_cpu();

    bool pre = MASK_ANY_SET(instruction, 1u << 24);
    bool up = MASK_ANY_SET(instruction, 1u << 23);
    bool s_bit = MASK_ANY_SET(instruction, 1u << 22);
    bool writeback = MASK_ANY_SET(instruction, 1u << 21);
    bool load = MASK_ANY_SET(instruction, 1u << 20);

    u32 rn = (instruction >> 16) & 0xFu;
    u32 rlist = instruction & 0xffffu;
    u32 base = cpu_read_reg(rn);
    u32 total;

    if (rlist == 0u) {
        MASK_SET(rlist, 1u << 15);
        total = 0x40;
    } else {
        total = cpu_count_register_list(rlist) * 4;
    }

    u32 start;
    u32 new_base;

    if (up) {
        start = pre ? base + 4u : base;
        new_base = base + total;
    } else {
        start = pre ? base - total : base - total + 4u;
        new_base = base - total;
    }

    u32 mode = cpu_current_mode();
    u32 mode_bank = cpu_mode_to_bank(mode);
    bool pc_in_list = MASK_ANY_SET(rlist, 1u << 15);
    bool user_regs = s_bit && mode_bank != 0u && (!load || !pc_in_list);
    bool in_fiq = mode == PSR_MODE_FIQ;

    bool rn_in_list = MASK_ANY_SET(rlist, 1u << rn);
    bool rn_first = rn_in_list && !MASK_ANY_SET(rlist, (1u << rn) - 1u);

    u32 address = start & ~3u;
    u32 pc_value = 0;

    if (load && writeback)
        cpu->r[rn] = new_base;

    for (u32 reg = 0; reg < 16; reg++) {
        if (!MASK_ANY_SET(rlist, 1u << reg))
            continue;

        u32* slot = &cpu->r[reg];
        if (user_regs) {
            if (reg == 13)
                slot = &cpu->bank_sp[0];
            else if (reg == 14)
                slot = &cpu->bank_lr[0];
            else if (reg >= 8 && reg <= 12 && in_fiq)
                slot = &cpu->usr_r8_r12[reg - 8];
        }

        if (load) {
            u32 value = mbus_read32(address, MBUS_DEFAULT);

            if (reg == 15) {
                pc_value = value;
            } else {
                *slot = value;
            }
        } else {
            u32 value;

            if (reg == 15) {
                value = arm_read_store_reg(15);
            } else if (reg == rn && writeback && !rn_first) {
                value = new_base;
            } else {
                value = *slot;
            }

            mbus_write32(address, value, MBUS_DEFAULT);
        }

        address += 4;
    }

    if (load) {
        mbus_add_cycles(1);

        if (pc_in_list) {
            if (s_bit)
                arm_restore_cpsr_from_spsr();

            MASK_CLEAR(pc_value, MASK_ANY_SET(cpu->cpsr, PSR_T_BIT) ? 1 : 3);
            cpu_set_pc(pc_value);
        }
    } else if (writeback) {
        cpu->r[rn] = new_base;
    }

    return true;
}

static
bool arm_branch_exchange(u32 instruction) {
    cpu_t* cpu = get_cpu();
    u32 rm = instruction & 0xfu;
    u32 target = cpu_read_reg(rm);
    bool thumb = MASK_ANY_SET(target, 1u);

    if (thumb)
        MASK_SET(cpu->cpsr, PSR_T_BIT);
    else
        MASK_CLEAR(cpu->cpsr, PSR_T_BIT);

    MASK_CLEAR(target, thumb ? 1u : 3u);
    cpu_set_pc(target);
    return true;
}

static
bool arm_multiply(u32 instruction) {
    cpu_t* cpu = get_cpu();

    bool accumulate = MASK_ANY_SET(instruction, 1u << 21);
    bool set_flags = MASK_ANY_SET(instruction, 1u << 20);
    u32 rd = (instruction >> 16) & 0xfu;
    u32 rn = (instruction >> 12) & 0xfu;
    u32 rs = (instruction >> 8) & 0xfu;
    u32 rm = instruction & 0xFu;

    if (rd == 15 || rs == 15 || rm == 15 || (accumulate && rn == 15)) {
        printf("undefined arm multiply instruction: 0x%08X\n", instruction);
        return false;
    }

    u32 multiplier = cpu->r[rs];
    u32 result = cpu->r[rm] * multiplier;
    if (accumulate)
        result += cpu->r[rn];

    cpu->r[rd] = result;
    if (set_flags)
        arm_update_multiply_flags(result, result == 0);

    mbus_add_cycles(mul_internal_cycles(multiplier) + (accumulate ? 1 : 0));
    return true;
}

static
bool arm_multiply_long(u32 instruction) {
    cpu_t* cpu = get_cpu();

    bool is_signed = MASK_ANY_SET(instruction, 1u << 22);
    bool accumulate = MASK_ANY_SET(instruction, 1u << 21);
    bool set_flags = MASK_ANY_SET(instruction, 1u << 20);
    u32 rd_hi = (instruction >> 16) & 0xfu;
    u32 rd_lo = (instruction >> 12) & 0xfu;
    u32 rs = (instruction >> 8) & 0xfu;
    u32 rm = instruction & 0xfu;

    if (rd_hi == 15 || rd_lo == 15 || rs == 15 || rm == 15 || rd_hi == rd_lo) {
        printf("undefined arm multiply long instruction: 0x%08X\n", instruction);
        return false;
    }

    u32 a = cpu->r[rm];
    u32 b = cpu->r[rs];
    u64 result;

    if (is_signed) {
        i64 signed_a = (i64)a;
        i64 signed_b = (i64)b;
        if (MASK_ANY_SET(a, 0x80000000u))
            signed_a -= INT64_C(0x100000000);
        if (MASK_ANY_SET(b, 0x80000000u))
            signed_b -= INT64_C(0x100000000);
        result = (u64)(signed_a * signed_b);
    } else {
        result = (u64)a * (u64)b;
    }

    if (accumulate)
        result += ((u64)cpu->r[rd_hi] << 32) | cpu->r[rd_lo];

    cpu->r[rd_lo] = (u32)result;
    cpu->r[rd_hi] = (u32)(result >> 32);

    if (set_flags)
        arm_update_multiply_flags((u32)(result >> 32), result == 0u);

    u32 cycles = is_signed ? mul_internal_cycles(b) : umul_internal_cycles(b);
    mbus_add_cycles(cycles + 1 + (accumulate ? 1 : 0));
    return true;
}

static
bool arm_swap(u32 instruction) {
    cpu_t* cpu = get_cpu();

    bool byte = MASK_ANY_SET(instruction, 1u << 22);
    u32 rn = (instruction >> 16) & 0xfu;
    u32 rd = (instruction >> 12) & 0xfu;
    u32 rm = instruction & 0xFu;

    if (rn == 15 || rd == 15 || rm == 15) {
        printf("undefined arm swap instruction: 0x%08X\n", instruction);
        return false;
    }

    u32 address = cpu->r[rn];
    u32 source = cpu->r[rm];
    u32 loaded;

    if (byte) {
        loaded = (u32)mbus_read8(address, MBUS_DEFAULT);
        mbus_write8(address, (u8)source, MBUS_DEFAULT);
    } else {
        loaded = cpu_load_word_rotated(address);
        mbus_write32(address & ~3u, source, MBUS_DEFAULT);
    }

    cpu->r[rd] = loaded;
    mbus_add_cycles(1);
    return true;
}

static
bool arm_halfword_data_transfer(u32 instruction) {
    cpu_t* cpu = get_cpu();

    bool immediate = MASK_ANY_SET(instruction, 1u << 22);
    bool pre = MASK_ANY_SET(instruction, 1u << 24);
    bool up = MASK_ANY_SET(instruction, 1u << 23);
    bool writeback = MASK_ANY_SET(instruction, 1u << 21);
    bool load = MASK_ANY_SET(instruction, 1u << 20);
    u32 rn = (instruction >> 16) & 0xfu;
    u32 rd = (instruction >> 12) & 0xfu;
    u32 sh = (instruction >> 5) & 3u;

    if (sh == 0 || (!load && sh != 1)) {
        printf("undefined arm halfword data transfer instruction: 0x%08X\n", instruction);
        return false;
    }

    u32 offset = immediate ? (((instruction >> 8) & 0xfu) << 4) | (instruction & 0xfu) : cpu_read_reg(instruction & 0xfu);
    u32 base = cpu_read_reg(rn);
    u32 adjusted_base = up ? base + offset : base - offset;
    u32 address = pre ? adjusted_base : base;
    bool do_writeback = !pre || writeback;

    if (!load) {
        u32 value = arm_read_store_reg(rd);
        mbus_write16(address & ~1u, (u16)value, MBUS_DEFAULT);

        if (do_writeback)
            cpu->r[rn] = adjusted_base;
        return true;
    }

    u32 value = 0;
    bool odd = MASK_ANY_SET(address, 1);

    if (sh == 1) {
        u32 aligned = address;
        value = (u32)mbus_read16(aligned & ~1u, MBUS_DEFAULT);
        if (odd)
            value = ror(value, 8u);
    } else if (sh == 2 || (sh == 3 && odd)) {
        value = (u32)mbus_read8(address, MBUS_DEFAULT);
        if (MASK_ANY_SET(value, 0x80u))
            MASK_SET(value, 0xffffff00u);
    } else {
        value = (u32)mbus_read16(address, MBUS_DEFAULT);
        if (MASK_ANY_SET(value, 0x8000u))
            MASK_SET(value, 0xffff0000u);
    }

    mbus_add_cycles(1);
    if (do_writeback)
        cpu->r[rn] = adjusted_base;

    if (rd == 15) {
        cpu_set_pc(value & ~3u);
    } else {
        cpu->r[rd] = value;
    }

    return true;
}

static
bool arm_mrs(u32 instruction) {
    cpu_t* cpu = get_cpu();
    bool use_spsr = MASK_ANY_SET(instruction, 1u << 22);
    u32 rd = (instruction >> 12) & 0xfu;

    if (rd == 15) {
        printf("undefined arm mrs instruction: 0x%08X\n", instruction);
        return false;
    }

    u32* spsr = cpu_current_spsr();
    cpu->r[rd] = (use_spsr && spsr) ? *spsr : cpu->cpsr;
    return true;
}

static
bool arm_msr(u32 instruction) {
    cpu_t* cpu = get_cpu();
    bool use_spsr = MASK_ANY_SET(instruction, 1u << 22);
    bool immediate = MASK_ANY_SET(instruction, 1u << 25);
    u32 value;

    if (immediate) {
        u32 rotate = ((instruction >> 8) & 0xfu) * 2;
        value = ror(instruction & 0xffu, rotate);
    } else {
        u32 rm = instruction & 0xfu;
        if (rm == 15) {
            printf("undefined arm msr instruction: 0x%08X\n", instruction);
            return false;
        }
        value = cpu->r[rm];
    }

    u32 mask = 0u;
    if (MASK_ANY_SET(instruction, 1u << 19))
        MASK_SET(mask, PSR_FLAGS_MASK);
    if (MASK_ANY_SET(instruction, 1u << 16))
        MASK_SET(mask, 0xffu);

    if (use_spsr) {
        u32* spsr = cpu_current_spsr();
        if (spsr) {
            MASK_CLEAR(*spsr, mask);
            MASK_SET(*spsr, value & mask);
        }
        return true;
    }

    if (cpu_current_mode() == PSR_MODE_USER)
        mask &= PSR_FLAGS_MASK;

    MASK_CLEAR(mask, PSR_T_BIT);

    u32 new_cpsr = cpu->cpsr;
    MASK_CLEAR(new_cpsr, mask);
    MASK_SET(new_cpsr, value & mask);

    if (MASK_ANY_SET(cpu->cpsr ^ new_cpsr, PSR_MODE_MASK))
        cpu_switch_mode(new_cpsr & PSR_MODE_MASK);

    cpu->cpsr = new_cpsr;
    return true;
}

static
bool arm_decode_data_misc(u32 instruction) {
    if (!MASK_MATCHES(instruction, 0x0C000000u, 0u)) {
        printf("undefined arm decode data misc instruction: 0x%08X\n", instruction);
        return false;
    }

    if (MASK_MATCHES(instruction, ARM_DM_BX_MASK, ARM_DM_BX_VALUE))
        return arm_branch_exchange(instruction);

    bool register_encoding = !MASK_ANY_SET(instruction, 1u << 25);
    if (register_encoding && MASK_IS_SET(instruction, (1u << 7) | (1u << 4))) {
        if (((instruction >> 5) & 3u) != 0) {
            if (MASK_MATCHES(instruction, ARM_DM_HALF_REG_MASK, ARM_DM_HALF_REG_VALUE)
                || MASK_MATCHES(instruction, ARM_DM_HALF_IMM_MASK, ARM_DM_HALF_IMM_VALUE))
                return arm_halfword_data_transfer(instruction);

            printf("undefined arm decode data misc instruction: 0x%08X\n", instruction);
            return false;
        }

        if (MASK_MATCHES(instruction, ARM_DM_MUL_MASK, ARM_DM_MUL_VALUE))
            return arm_multiply(instruction);
        if (MASK_MATCHES(instruction, ARM_DM_MULL_MASK, ARM_DM_MULL_VALUE))
            return arm_multiply_long(instruction);
        if (MASK_MATCHES(instruction, ARM_DM_SWP_MASK, ARM_DM_SWP_VALUE))
            return arm_swap(instruction);

        printf("undefined arm decode data misc instruction: 0x%08X\n", instruction);
        return false;
    }

    if (MASK_MATCHES(instruction, ARM_DM_MRS_MASK, ARM_DM_MRS_VALUE))
        return arm_mrs(instruction);

    if (MASK_MATCHES(instruction, ARM_DM_MSR_REG_MASK, ARM_DM_MSR_REG_VALUE)
        || MASK_MATCHES(instruction, ARM_DM_MSR_IMM_MASK, ARM_DM_MSR_IMM_VALUE))
        return arm_msr(instruction);

    if (!arm_data_processing(instruction)) {
        printf("undefined arm decode data misc instruction: 0x%08X\n", instruction);
        return false;
    }

    return true;
}

static
bool arm_single_data_transfer(u32 instruction) {
    cpu_t* cpu = get_cpu();

    bool reg_offset = MASK_ANY_SET(instruction, 1u << 25);
    bool pre = MASK_ANY_SET(instruction, 1u << 24);
    bool up = MASK_ANY_SET(instruction, 1u << 23);
    bool byte = MASK_ANY_SET(instruction, 1u << 22);
    bool writeback = MASK_ANY_SET(instruction, 1u << 21);
    bool load = MASK_ANY_SET(instruction, 1u << 20);

    u32 rn = (instruction >> 16) & 0xfu;
    u32 rd = (instruction >> 12) & 0xfu;
    u32 offset;

    if (!reg_offset) {
        offset = instruction & 0xfffu;
    } else {
        if (MASK_ANY_SET(instruction, 1u << 4)) {
            printf("undefined arm single data transfer: 0x%08X\n", instruction);
            return false;
        }

        u32 value = cpu_read_reg(instruction & 0xfu);
        u32 type = (instruction >> 5) & 3u;
        u32 amount = (instruction >> 7) & 0x1fu;
        bool carry_in = MASK_ANY_SET(cpu->cpsr, PSR_C);

        alu_shift_result_t shifted = alu_barrel_shift(value, type, amount, carry_in, false);
        offset = shifted.value;
    }

    u32 base = cpu_read_reg(rn);
    u32 adjusted_base = up ? base + offset : base - offset;
    u32 address = pre ? adjusted_base : base;
    bool do_writeback = !pre || writeback;

    if (load) {
        u32 value = byte ? (u32)mbus_read8(address, MBUS_DEFAULT) : cpu_load_word_rotated(address);

        if (do_writeback)
            cpu->r[rn] = adjusted_base;

        if (rd == 15)
            cpu_set_pc(value & ~3u);
        else
            cpu->r[rd] = value;

        mbus_add_cycles(1);
    } else {
        u32 value = arm_read_store_reg(rd);

        if (byte)
            mbus_write8(address, (u8)value, MBUS_DEFAULT);
        else
            mbus_write32(address & ~3u, value, MBUS_DEFAULT);

        if (do_writeback)
            cpu->r[rn] = adjusted_base;
    }

    return true;
}

static
bool arm_branch(u32 instruction) {
    cpu_t* cpu = get_cpu();

    bool link = MASK_ANY_SET(instruction, 1u << 24);
    u32 offset = (instruction & 0x00ffffffu) << 2;

    if (MASK_ANY_SET(offset, 0x02000000u))
        MASK_SET(offset, 0xfc000000u);

    if (link)
        cpu->lr = cpu->pc;

    cpu_set_pc(cpu->pc + 4 + offset);

    return true;
}

static
const arm_instruction_handler_t arm_handlers[8] = {
    arm_decode_data_misc,
    arm_data_processing,
    arm_single_data_transfer,
    arm_single_data_transfer,
    arm_block_data_transfer,
    arm_branch,
    0, 0
};

static
const thumb_instruction_handler_t thumb_handlers[8] = {
    0, 0, 0, 0, 0, 0, 0, 0
};

static
bool thumb_decode(u16 instruction) {
    u16 instruction_handler = (instruction >> 13) & 0x7;

    if (thumb_handlers[instruction_handler] == 0) {
        printf("(thumb) unimplemented handler: %d\n", instruction_handler);
        return false;
    }

    return thumb_handlers[instruction_handler](instruction);
}

static
bool arm_decode(u32 instruction) {
    u32 instruction_handler = (instruction >> 25) & 0x7;

    if (arm_handlers[instruction_handler] == 0) {
        printf("(arm) unimplemented handler: %d\n", instruction_handler);
        return false;
    }

    return arm_handlers[instruction_handler](instruction);
}

static
bool cpu_fetch() {
    cpu_t* cpu = get_cpu();
    u32 address = cpu->pc;

    if (is_thumb_mode()) {
        u16 instruction = mbus_read16(address, MBUS_DEFAULT);
        cpu->pc = address + 2;

        u32 bus_value = (u32)instruction;
        mbus_set_cpu_open_bus(bus_value | (bus_value << 16));

        return thumb_decode(instruction);
    }

    u32 instruction = mbus_read32(address, MBUS_DEFAULT);
    cpu->pc = address + 4;

    mbus_set_cpu_open_bus(instruction);

    if (!cpu_check_instruction_cond(instruction >> 28, cpu->cpsr))
        return true;

    return arm_decode(instruction);
}

bool cpu_step(u32* cycles) {
    cpu_t* cpu = get_cpu();
    mbus_get_total_cycle_count(true);

    bool result = true;
    bool hle_irq_return = cpu->skip_bios_mode && cpu_current_mode() == PSR_MODE_IRQ && !(cpu->cpsr & PSR_T_BIT) && cpu->pc == BIOS_IRQ_RETURN;

    if (hle_irq_return) {
        cpu_leave_irq_hle();
        mbus_add_cycles(CPU_IRQ_APPROX_OVERHEAD);
    } else if (irq_should_fire()) {
        cpu_enter_irq();
        mbus_add_cycles(CPU_IRQ_APPROX_OVERHEAD);
    } else {
        result = cpu_fetch();
    }

    u32 elapsed = mbus_get_total_cycle_count(true);
    *cycles = elapsed ? elapsed : 1;
    return result;
}