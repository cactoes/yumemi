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
    cpu->bank_sp[cpu_mode_to_bank(PSR_MODE_SUPERVISOR)] = 0x03007FE0u;
    cpu->bank_sp[cpu_mode_to_bank(PSR_MODE_IRQ)] = 0x03007FA0u;

    mbus_set_bios_open_bus(0xe129f000u);
}

static
bool thumb_decode(u16 instruction) {
    return false;
}

static
bool arm_decode(u32 instruction) {
    return false;
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