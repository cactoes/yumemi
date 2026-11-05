#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <math.h>

#include "cpu.h"
#include "memory_bus.h"
#include "helper.h"
#include "gba_io.h"

static cpu_t global_cpu;

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
void cpu_branch_exchange(u32 target) {
    cpu_t* cpu = get_cpu();

    bool thumb = MASK_ANY_SET(target, 1u);

    if (thumb)
        MASK_SET(cpu->cpsr, PSR_T_BIT);
    else
        MASK_CLEAR(cpu->cpsr, PSR_T_BIT);

    MASK_CLEAR(target, thumb ? 1u : 3u);
    cpu_set_pc(target);
}

static
u32 ror(u32 value, u32 amount) {
    amount &= 31;

    if (amount == 0)
        return value;

    return (value >> amount) | (value << (32 - amount));
}

static
void bios_hle_set_mode(u32 mode) {
    cpu_t* cpu = get_cpu();
    cpu_switch_mode(mode);
    MASK_CLEAR(cpu->cpsr, PSR_MODE_MASK);
    MASK_SET(cpu->cpsr, mode);
}

static
bool bios_hle_soft_reset() {
    printf("[BIOS HLE] soft reset triggerd\n");

    cpu_t* cpu = get_cpu();
    u8 boot_from_ram = mbus_read8(BIOS_HLE_RESET_FLAG_ADDRESS, MBUS_DEFAULT);

    cpu->bios_hle_wait_active = false;
    cpu->bios_hle_wait_mask = 0;

    bios_hle_set_mode(PSR_MODE_IRQ);
    cpu->sp = BIOS_HLE_IRQ_STACK;
    cpu->lr = 0;
    cpu->spsr[cpu_mode_to_bank(PSR_MODE_IRQ)] = 0;

    bios_hle_set_mode(PSR_MODE_SUPERVISOR);
    cpu->sp = BIOS_HLE_SVC_STACK;
    cpu->lr = 0;
    cpu->spsr[cpu_mode_to_bank(PSR_MODE_SUPERVISOR)] = 0;

    bios_hle_set_mode(PSR_MODE_SYSTEM);
    cpu->sp = BIOS_HLE_USER_STACK;
    cpu->lr = 0;

    for (u32 reg = 0; reg < 13; reg++)
        cpu->r[reg] = 0;

    for (u32 offset = 0; offset < BIOS_HLE_RESET_RAM_SIZE; offset++)
        mbus_write8(BIOS_HLE_RESET_RAM_START + offset, 0, MBUS_DEFAULT);

    cpu->cpsr = PSR_MODE_SYSTEM;
    cpu_set_pc(boot_from_ram ? BIOS_HLE_RAM_ENTRY : BIOS_HLE_ROM_ENTRY);
    return true;
}

static
bool bios_hle_register_ram_reset() {
    mbus_reset_ram(get_cpu()->r[0] & 0xffu);
    return true;
}

static
bool bios_hle_halt() {
    io_cpu_halt();
    return true;
}

static
bool bios_hle_intr_wait(u16 mask, bool discard) {
    cpu_t* cpu = get_cpu();
    u16 flags = mbus_read16(BIOS_HLE_IRQ_FLAGS_ADDRESS, MBUS_DEFAULT);

    if (!cpu->bios_hle_wait_active) {
        cpu->bios_hle_wait_mask = mask;
        cpu->bios_hle_wait_active = true;
        if (discard) {
            MASK_CLEAR(flags, mask);
            mbus_write16(BIOS_HLE_IRQ_FLAGS_ADDRESS, flags, MBUS_DEFAULT);
        }
    }

    mask = cpu->bios_hle_wait_mask;
    io_write16(IO_IME, 1, IO_DEFAULT);

    if (MASK_ANY_SET(flags, mask)) {
        MASK_CLEAR(flags, mask);
        mbus_write16(BIOS_HLE_IRQ_FLAGS_ADDRESS, flags, MBUS_DEFAULT);
        cpu->bios_hle_wait_mask = 0;
        cpu->bios_hle_wait_active = false;
        return true;
    }

    cpu->pc -= is_thumb_mode() ? 2 : 4;
    io_cpu_halt();
    return true;
}

static
void bios_hle_copy_memory(u32 src, u32 dst, u32 count, bool word, bool fill) {
    if (count == 0u)
        return;

    if (word) {
        u32 fill_value = fill ? mbus_read32(src, MBUS_DEFAULT) : 0u;
        for (u32 index = 0u; index < count; index++) {
            u32 value = fill ? fill_value : mbus_read32(src, MBUS_DEFAULT);
            mbus_write32(dst, value, MBUS_DEFAULT);
            src += 4u;
            dst += 4u;
        }
    } else {
        u16 fill_value = fill ? mbus_read16(src, MBUS_DEFAULT) : 0u;
        for (u32 index = 0u; index < count; index++) {
            u16 value = fill ? fill_value : mbus_read16(src, MBUS_DEFAULT);
            mbus_write16(dst, value, MBUS_DEFAULT);
            src += 2u;
            dst += 2u;
        }
    }
}

static
bool bios_hle_cpu_set(bool fast) {
    cpu_t* cpu = get_cpu();
    u32 control = cpu->r[2];
    u32 count = control & 0x001fffffu;
    bool fill = MASK_ANY_SET(control, 1u << 24);
    bool word = fast || MASK_ANY_SET(control, 1u << 26);

    if (fast) {
        count += 7u;
        MASK_CLEAR(count, 7u);
    }

    bios_hle_copy_memory(cpu->r[0], cpu->r[1], count, word, fill);
    return true;
}

static
int32_t bios_hle_sin_approx(u32 angle) {
    const double tau = 6.283185307179586;
    double radians = (double)(angle & 0xffu) * tau / 256.0;
    return (int32_t)(sin(radians) * 16384.0);
}

static
int32_t bios_hle_read_s16(u32 address) {
    u32 value = mbus_read16(address, MBUS_DEFAULT);
    return (int32_t)(value & 0x7fffu) - (int32_t)(value & 0x8000u);
}

static
int32_t bios_hle_fixed14_product(int32_t scale, int32_t trig) {
    int64_t product = (int64_t)scale * trig;
    return product >= 0 ? (int32_t)(product / 16384) : (int32_t)(-((-product + 16383) / 16384));
}

static
bool bios_hle_obj_affine_set() {
    cpu_t* cpu = get_cpu();
    u32 src = cpu->r[0];
    u32 dst = cpu->r[1];
    u32 count = cpu->r[2];
    u32 stride = cpu->r[3];

    for (u32 index = 0; index < count; index++) {
        int32_t sx = bios_hle_read_s16(src);
        int32_t sy = bios_hle_read_s16(src + 2u);
        u32 angle = mbus_read16(src + 4u, MBUS_DEFAULT) >> 8;
        src += 8;

        int32_t sine = bios_hle_sin_approx(angle);
        int32_t cosine = bios_hle_sin_approx(angle + 64);
        int32_t matrix[4] = {
            bios_hle_fixed14_product(sx, cosine),
            bios_hle_fixed14_product(-sx, sine),
            bios_hle_fixed14_product(sy, sine),
            bios_hle_fixed14_product(sy, cosine)
        };

        for (u32 component = 0; component < 4; component++) {
            mbus_write16(dst, (u16)matrix[component], MBUS_DEFAULT);
            dst += stride;
        }
    }

    return true;
}

static
bool bios_hle_bg_affine_set() {
    cpu_t* cpu = get_cpu();
    u32 src = cpu->r[0];
    u32 dst = cpu->r[1];
    u32 count = cpu->r[2];

    for (u32 index = 0; index < count; index++) {
        int32_t ox = (int32_t)mbus_read32(src, MBUS_DEFAULT);
        int32_t oy = (int32_t)mbus_read32(src + 4, MBUS_DEFAULT);
        int32_t cx = bios_hle_read_s16(src + 8);
        int32_t cy = bios_hle_read_s16(src + 10);
        int32_t sx = bios_hle_read_s16(src + 12);
        int32_t sy = bios_hle_read_s16(src + 14);
        u32 angle = mbus_read16(src + 16, MBUS_DEFAULT) >> 8;
        src += 20;

        int32_t sine = bios_hle_sin_approx(angle);
        int32_t cosine = bios_hle_sin_approx(angle + 64);

        int32_t pa = bios_hle_fixed14_product(sx, cosine);
        int32_t pb = bios_hle_fixed14_product(-sx, sine);
        int32_t pc = bios_hle_fixed14_product(sy, sine);
        int32_t pd = bios_hle_fixed14_product(sy, cosine);

        int32_t dx = ox - (pa * cx + pb * cy);
        int32_t dy = oy - (pc * cx + pd * cy);

        mbus_write16(dst, (u16)pa, MBUS_DEFAULT);
        mbus_write16(dst + 2, (u16)pb, MBUS_DEFAULT);
        mbus_write16(dst + 4, (u16)pc, MBUS_DEFAULT);
        mbus_write16(dst + 6, (u16)pd, MBUS_DEFAULT);
        mbus_write32(dst + 8, (u32)dx, MBUS_DEFAULT);
        mbus_write32(dst + 12, (u32)dy, MBUS_DEFAULT);
        dst += 16;
    }

    return true;
}

static
bool bios_hle_lz77_decode(u32 src, u8* out, u32 size) {
    u32 written = 0u;
    while (written < size) {
        u8 flags = mbus_read8(src++, MBUS_DEFAULT);
        for (u32 bit = 0; bit < 8 && written < size; bit++) {
            bool compressed = MASK_ANY_SET(flags, 0x80u);
            flags = (u8)(flags << 1);

            if (!compressed) {
                out[written++] = mbus_read8(src++, MBUS_DEFAULT);
                continue;
            }

            u8 first = mbus_read8(src++, MBUS_DEFAULT);
            u8 second = mbus_read8(src++, MBUS_DEFAULT);
            u32 length = (first >> 4) + 3;
            u32 distance = (((u32)first & 0x0fu) << 8) | second;
            distance += 1;

            if (distance > written)
                return false;

            for (u32 index = 0; index < length && written < size; index++) {
                out[written] = out[written - distance];
                written++;
            }
        }
    }
    return true;
}

static
void bios_hle_write_decompressed(u32 dst, const u8* data, u32 size, bool halfword) {
    if (!halfword) {
        for (u32 index = 0u; index < size; index++)
            mbus_write8(dst + index, data[index], MBUS_DEFAULT);

        return;
    }

    MASK_CLEAR(dst, 1u);
    for (u32 index = 0; index < size; index += 2) {
        u16 value = data[index];
        if (index + 1u < size)
            value |= (u16)((u16)data[index + 1u] << 8);

        mbus_write16(dst + index, value, MBUS_DEFAULT);
    }
}

static
bool bios_hle_lz77_uncompress(bool vram) {
    cpu_t* cpu = get_cpu();
    u32 src = cpu->r[0];
    u32 dst = cpu->r[1];
    u32 header = mbus_read32(src, MBUS_DEFAULT);
    u32 size = header >> 8;

    if ((header & 0xf0u) != 0x10u)
        return false;

    if (size == 0u)
        return true;

    u8* output = malloc(size);
    if (!output)
        return false;

    bool success = bios_hle_lz77_decode(src + 4u, output, size);
    if (success)
        bios_hle_write_decompressed(dst, output, size, vram);

    free(output);
    return success;
}

static
bios_div_result_t bios_div(i32 number, i32 denom) {
    bios_div_result_t result;

    if (denom == 0) {
        result.quotient = number < 0 ? -1 : 1;
        result.remainder = number;
        result.abs_quotient = 1;
        return result;
    }

    if (denom == -1 && number == INT32_MIN) {
        result.quotient = INT32_MIN;
        result.remainder = 0;
        result.abs_quotient = 0x80000000u;
        return result;
    }

    result.quotient = number / denom;
    result.remainder = number % denom;
    result.abs_quotient = (uint32_t)(result.quotient < 0 ? -result.quotient : result.quotient);
    return result;
}

static
bool bios_hle_dispatch(u32 service) {
    cpu_t* cpu = get_cpu();
    switch (service) {
        case SWI_SOFT_RESET:
            return bios_hle_soft_reset();
        case SWI_REGISTER_RAM_RESET:
            return bios_hle_register_ram_reset();
        case SWI_HALT:
            return bios_hle_halt();
        case SWI_INTR_WAIT:
            return bios_hle_intr_wait((u16)cpu->r[1], cpu->r[0] != 0u);
        case SWI_VBLANK_INTR_WAIT:
            return bios_hle_intr_wait(1, true);
        case SWI_BIOS_DIV: {
            bios_div_result_t d = bios_div((i32)cpu->r[0], (i32)cpu->r[1]);
            cpu->r[0] = (u32)d.quotient;
            cpu->r[1] = (u32)d.remainder;
            cpu->r[3] = d.abs_quotient;
            return true;
        }
        case SWI_BIOS_DIV_ARM: {
            bios_div_result_t d = bios_div((i32)cpu->r[1], (i32)cpu->r[0]);
            cpu->r[0] = (u32)d.quotient;
            cpu->r[1] = (u32)d.remainder;
            cpu->r[3] = d.abs_quotient;
            return true;
        }
        case SWI_CPU_SET:
            return bios_hle_cpu_set(false);
        case SWI_CPU_FAST_SET:
            return bios_hle_cpu_set(true);
        case SWI_OBJ_AFFINE_SET:
            return bios_hle_obj_affine_set();
        case SWI_BG_AFFINE_SET:
            return bios_hle_bg_affine_set();
        case SWI_LZ77_WRAM:
            return bios_hle_lz77_uncompress(LZ77_WRAM);
        case SWI_LZ77_VRAM:
            return bios_hle_lz77_uncompress(LZ77_VRAM);
        default:
            printf("unimplemented BIOS HLE service: 0x%02x\n", (unsigned)service);
            return false;
    }
}

static
bool cpu_swi(u32 service) {
    cpu_t* cpu = get_cpu();
    if (!cpu->skip_bios_mode) {
        printf("unimplemented SWI (bios): 0x%02X\n", service);
        return false;
    }

    mbus_add_cycles(3);
    return bios_hle_dispatch(service);
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

    if (test && rd == 15) {
        arm_restore_cpsr_from_spsr();
        return true;
    }

    if (!test && rd == 15) {
        if (set_flags)
            arm_restore_cpsr_from_spsr();

        MASK_CLEAR(result, MASK_ANY_SET(cpu->cpsr, PSR_T_BIT) ? 1u : 3u);

        cpu_set_pc(result);
        return true;
    }

    if (!test)
        cpu->r[rd] = result;

    if (set_flags) {
        bool carry = arithmetic ? added.carry : operand2.carry;
        bool overflow = arithmetic ? added.overflow : false;

        arm_update_data_processing_flags(
            result, carry, arithmetic, overflow);
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

    u32 address = start;
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
    u32 rm = instruction & 0xfu;
    u32 target = cpu_read_reg(rm);
    cpu_branch_exchange(target);
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
        mbus_write32(address, source, MBUS_DEFAULT);
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
        mbus_write16(address, (u16)value, MBUS_DEFAULT);

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
            mbus_write32(address, value, MBUS_DEFAULT);

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
bool arm_decode_swi_coprocessor(u32 instruction) {
    if (MASK_MATCHES(instruction, 0x0f000000u, 0x0f000000u))
        return cpu_swi((instruction >> 16) & 0xffu);

    printf("unsupported arm coprocessor instruction: 0x%08x\n", instruction);
    return false;
}

static
const arm_instruction_handler_t arm_handlers[8] = {
    arm_decode_data_misc,
    arm_decode_data_misc,
    arm_single_data_transfer,
    arm_single_data_transfer,
    arm_block_data_transfer,
    arm_branch,
    0,
    arm_decode_swi_coprocessor
};

static
i32 thumb_sign_extend(u32 value, u32 bits) {
    u32 sign = 1u << (bits - 1u);
    return (i32)(value & (sign - 1u)) - (i32)(value & sign);
}

static
void thumb_update_nz_flags(u32 value) {
    cpu_t* cpu = get_cpu();

    MASK_CLEAR(cpu->cpsr, PSR_N | PSR_Z);

    if (MASK_ANY_SET(value, 0x80000000u))
        MASK_SET(cpu->cpsr, PSR_N);

    if (value == 0)
        MASK_SET(cpu->cpsr, PSR_Z);
}

static
void thumb_update_nzc_flags(u32 value, bool carry) {
    cpu_t* cpu = get_cpu();

    thumb_update_nz_flags(value);

    MASK_CLEAR(cpu->cpsr, PSR_C);

    if (carry)
        MASK_SET(cpu->cpsr, PSR_C);
}

static
void thumb_commit_arithmetic(u32 rd, alu_add_result_t result, bool write_result) {
    cpu_t* cpu = get_cpu();

    if (write_result)
        cpu->r[rd] = result.value;

    thumb_update_nzc_flags(result.value, result.carry);

    MASK_CLEAR(cpu->cpsr, PSR_V);

    if (result.overflow)
        MASK_SET(cpu->cpsr, PSR_V);
}

static
void thumb_write_pc(u32 target) {
    cpu_set_pc(target & ~1u);
}

static
u32 thumb_load_halfword(u32 address) {
    bool odd = MASK_ANY_SET(address, 1u);

    u32 value = (u32)mbus_read16(address & ~1u, MBUS_DEFAULT);

    return odd ? ror(value, 8u) : value;
}

static
u32 thumb_load_signed_byte(u32 address) {
    u32 value = (u32)mbus_read8(address, MBUS_DEFAULT);

    if (MASK_ANY_SET(value, 0x80u))
        MASK_SET(value, 0xffffff00u);

    return value;
}

static
u32 thumb_load_signed_halfword(u32 address) {
    if (MASK_ANY_SET(address, 1u))
        return thumb_load_signed_byte(address);

    u32 value = (u32)mbus_read16(address, MBUS_DEFAULT);

    if (MASK_ANY_SET(value, 0x8000u))
        MASK_SET(value, 0xffff0000u);

    return value;
}

static
bool thumb_transfer_register(u32 rd, u32 address, bool load, u32 type) {
    cpu_t* cpu = get_cpu();

    if (load) {
        u32 value = 0;

        switch (type) {
            case THUMB_MEMORY_WORD:
                value = cpu_load_word_rotated(address);
                break;
            case THUMB_MEMORY_BYTE:
                value = (u32)mbus_read8(address, MBUS_DEFAULT);
                break;
            case THUMB_MEMORY_HALFWORD:
                value = thumb_load_halfword(address);
                break;
            case THUMB_MEMORY_SIGNED_BYTE:
                value = thumb_load_signed_byte(address);
                break;
            case THUMB_MEMORY_SIGNED_HALFWORD:
                value = thumb_load_signed_halfword(address);
                break;
            default:
                return false;
        }

        cpu->r[rd] = value;
        mbus_add_cycles(1);

        return true;
    }

    u32 value = cpu->r[rd];

    switch (type) {
        case THUMB_MEMORY_WORD:
            mbus_write32(address, value, MBUS_DEFAULT);
            break;
        case THUMB_MEMORY_BYTE:
            mbus_write8(address, (u8)value, MBUS_DEFAULT);
            break;
        case THUMB_MEMORY_HALFWORD:
            mbus_write16(address, (u16)value, MBUS_DEFAULT);
            break;
        default:
            return false;
    }

    return true;
}

static
bool thumb_shift_immediate(u16 instruction) {
    cpu_t* cpu = get_cpu();

    u32 type = (instruction >> 11) & 3u;
    u32 amount = (instruction >> 6) & 0x1fu;
    u32 rs = (instruction >> 3) & 7u;
    u32 rd = instruction & 7u;

    if (type == 3) {
        printf("undefined thumb shift immediate instruction: 0x%04x\n", instruction);
        return false;
    }

    bool carry_in = MASK_ANY_SET(cpu->cpsr, PSR_C);

    alu_shift_result_t shifted = alu_barrel_shift(cpu->r[rs], type, amount, carry_in, false);

    cpu->r[rd] = shifted.value;
    thumb_update_nzc_flags(shifted.value, shifted.carry);

    return true;
}

static
bool thumb_add_subtract(u16 instruction) {
    cpu_t* cpu = get_cpu();

    bool immediate = MASK_ANY_SET(instruction, 1u << 10);
    bool subtract = MASK_ANY_SET(instruction, 1u << 9);

    u32 rd = instruction & 7u;
    u32 rs = (instruction >> 3) & 7u;
    u32 operand = (instruction >> 6) & 7u;

    u32 b = immediate ? operand : cpu->r[operand];

    alu_add_result_t result = alu_add_with_carry(cpu->r[rs], subtract ? ~b : b, subtract);

    thumb_commit_arithmetic(rd, result, true);

    return true;
}

static
bool thumb_data_processing_immediate(u16 instruction) {
    cpu_t* cpu = get_cpu();

    u32 opcode = (instruction >> 11) & 3u;
    u32 rd = (instruction >> 8) & 7u;
    u32 immediate = instruction & 0xffu;

    if (opcode == 0) {
        cpu->r[rd] = immediate;
        thumb_update_nz_flags(immediate);
        return true;
    }

    bool subtract = opcode != 2;

    alu_add_result_t result = alu_add_with_carry(cpu->r[rd], subtract ? ~immediate : immediate, subtract);

    thumb_commit_arithmetic(rd, result, opcode != 1);

    return true;
}

static
bool thumb_alu_register(u16 instruction) {
    cpu_t* cpu = get_cpu();

    u32 opcode = (instruction >> 6) & 0xfu;
    u32 rs = (instruction >> 3) & 7u;
    u32 rd = instruction & 7u;

    u32 a = cpu->r[rd];
    u32 b = cpu->r[rs];

    bool carry_in = MASK_ANY_SET(cpu->cpsr, PSR_C);

    u32 value = 0;
    bool arithmetic = false;
    bool write_result = true;

    alu_add_result_t added = {0};

    switch (opcode) {
        case 0x0:
            value = a & b;
            break;
        case 0x1:
            value = a ^ b;
            break;
        case 0x2:
        case 0x3:
        case 0x4:
        case 0x7: {
            u32 type = opcode == 0x7u ? 3u : opcode - 2u;

            alu_shift_result_t shifted = alu_barrel_shift(
                a,
                type,
                b & 0xffu,
                carry_in,
                true
            );

            cpu->r[rd] = shifted.value;
            thumb_update_nzc_flags(shifted.value, shifted.carry);

            mbus_add_cycles(1);

            return true;
        }

        case 0x5:
            added = alu_add_with_carry(a, b, carry_in);
            arithmetic = true;
            break;
        case 0x6:
            added = alu_add_with_carry(a, ~b, carry_in);
            arithmetic = true;
            break;
        case 0x8:
            value = a & b;
            write_result = false;
            break;
        case 0x9:
            added = alu_add_with_carry(0u, ~b, true);
            arithmetic = true;
            break;
        case 0xa:
            added = alu_add_with_carry(a, ~b, true);
            arithmetic = true;
            write_result = false;
            break;
        case 0xb:
            added = alu_add_with_carry(a, b, false);
            arithmetic = true;
            write_result = false;
            break;
        case 0xc:
            value = a | b;
            break;
        case 0xd:
            value = a * b;
            mbus_add_cycles(mul_internal_cycles(a));
            break;
        case 0xe:
            value = a & ~b;
            break;
        case 0xf:
            value = ~b;
            break;
        default:
            printf("undefined thumb alu register instruction: 0x%04x\n", instruction);
            return false;
    }

    if (arithmetic) {
        thumb_commit_arithmetic(rd, added, write_result);
    } else {
        if (write_result)
            cpu->r[rd] = value;

        thumb_update_nz_flags(value);
    }

    return true;
}

static
bool thumb_high_register(u16 instruction) {
    cpu_t* cpu = get_cpu();

    u32 opcode = (instruction >> 8) & 3u;
    u32 rd = (instruction & 7u) | ((instruction >> 4) & 8u);
    u32 rs = (instruction >> 3) & 0xfu;

    switch (opcode) {
        case 0: {
            u32 value = cpu_read_reg(rd) + cpu_read_reg(rs);

            if (rd == 15)
                thumb_write_pc(value);
            else
                cpu->r[rd] = value;

            return true;
        }
        case 1: {
            alu_add_result_t result = alu_add_with_carry(cpu_read_reg(rd), ~cpu_read_reg(rs), true);

            thumb_commit_arithmetic(rd, result, false);

            return true;
        }
        case 2: {
            u32 value = cpu_read_reg(rs);

            if (rd == 15)
                thumb_write_pc(value);
            else
                cpu->r[rd] = value;

            return true;
        }
        case 3:
            if (MASK_ANY_SET(instruction, 0x0087u)) {
                printf("undefined thumb high register instruction: 0x%04x\n", instruction);
                return false;
            }

            cpu_branch_exchange(cpu_read_reg(rs));

            return true;
    }

    printf("undefined thumb high register instruction: 0x%04x\n", instruction);
    return false;
}

static
bool thumb_load_literal(u16 instruction) {
    u32 rd = (instruction >> 8) & 7u;

    u32 base = cpu_read_reg(15);

    u32 address = (base & ~3u) + ((instruction & 0xffu) << 2);

    return thumb_transfer_register(rd, address, true, THUMB_MEMORY_WORD );
}

static
bool thumb_load_store_register(u16 instruction) {
    cpu_t* cpu = get_cpu();

    u32 ro = (instruction >> 6) & 7u;
    u32 rb = (instruction >> 3) & 7u;
    u32 rd = instruction & 7u;

    bool load = MASK_ANY_SET(instruction, 1u << 11);
    bool byte = MASK_ANY_SET(instruction, 1u << 10);

    u32 address = cpu->r[rb] + cpu->r[ro];

    return thumb_transfer_register(rd, address, load, byte ? THUMB_MEMORY_BYTE : THUMB_MEMORY_WORD);
}

static
bool thumb_load_store_signed(u16 instruction) {
    cpu_t* cpu = get_cpu();

    u32 opcode = (instruction >> 10) & 3u;
    u32 ro = (instruction >> 6) & 7u;
    u32 rb = (instruction >> 3) & 7u;
    u32 rd = instruction & 7u;

    u32 address = cpu->r[rb] + cpu->r[ro];

    static const u32 types[4] = {
        THUMB_MEMORY_HALFWORD,
        THUMB_MEMORY_SIGNED_BYTE,
        THUMB_MEMORY_HALFWORD,
        THUMB_MEMORY_SIGNED_HALFWORD
    };

    return thumb_transfer_register(rd, address, opcode != 0, types[opcode]);
}

static
bool thumb_load_store_immediate(u16 instruction) {
    cpu_t* cpu = get_cpu();

    bool byte = MASK_ANY_SET(instruction, 1u << 12);
    bool load = MASK_ANY_SET(instruction, 1u << 11);

    u32 offset = (instruction >> 6) & 0x1fu;
    u32 rb = (instruction >> 3) & 7u;
    u32 rd = instruction & 7u;

    if (!byte)
        offset <<= 2;

    return thumb_transfer_register(rd, cpu->r[rb] + offset, load, byte ? THUMB_MEMORY_BYTE : THUMB_MEMORY_WORD);
}

static
bool thumb_load_store_halfword_immediate(u16 instruction) {
    cpu_t* cpu = get_cpu();

    bool load = MASK_ANY_SET(instruction, 1u << 11);

    u32 offset = ((instruction >> 6) & 0x1fu) << 1;
    u32 rb = (instruction >> 3) & 7u;
    u32 rd = instruction & 7u;

    return thumb_transfer_register(rd, cpu->r[rb] + offset, load, THUMB_MEMORY_HALFWORD);
}

static
bool thumb_load_store_sp_relative(u16 instruction) {
    cpu_t* cpu = get_cpu();

    bool load = MASK_ANY_SET(instruction, 1u << 11);
    u32 rd = (instruction >> 8) & 7u;
    u32 offset = (instruction & 0xffu) << 2;

    return thumb_transfer_register(rd, cpu->sp + offset, load, THUMB_MEMORY_WORD);
}

static
bool thumb_load_address(u16 instruction) {
    cpu_t* cpu = get_cpu();

    bool use_sp = MASK_ANY_SET(instruction, 1u << 11);

    u32 rd = (instruction >> 8) & 7u;
    u32 offset = (instruction & 0xffu) << 2;

    u32 base = use_sp ? cpu->sp : cpu_read_reg(15);

    if (!use_sp)
        MASK_CLEAR(base, 3u);

    cpu->r[rd] = base + offset;

    return true;
}

static
bool thumb_adjust_sp(u16 instruction) {
    cpu_t* cpu = get_cpu();

    bool subtract = MASK_ANY_SET(instruction, 1u << 7);
    u32 offset = (instruction & 0x7fu) << 2;

    cpu->sp = subtract ? cpu->sp - offset : cpu->sp + offset;

    return true;
}

static
bool thumb_push_pop(u16 instruction) {
    cpu_t* cpu = get_cpu();

    bool load = MASK_ANY_SET(instruction, 1u << 11);
    bool extra = MASK_ANY_SET(instruction, 1u << 8);

    u32 rlist = instruction & 0xffu;

    if (extra)
        MASK_SET(rlist, 1u << (load ? 15 : 14));

    u32 base = cpu->sp;

    if (rlist == 0u) {
        u32 address = (load ? base : base - 0x40);

        if (load) {
            u32 target = mbus_read32(address, MBUS_DEFAULT);

            cpu->sp = base + 0x40;

            mbus_add_cycles(1);
            thumb_write_pc(target);
        } else {
            mbus_write32(address, cpu_read_reg(15) + 2, MBUS_DEFAULT);
            cpu->sp = base - 0x40;
        }

        return true;
    }

    u32 total = cpu_count_register_list(rlist) * 4;
    u32 new_sp = load ? base + total : base - total;

    u32 address = (load ? base : new_sp);

    u32 pc_value = 0;

    for (u32 reg = 0; reg < 16; reg++) {
        if (!MASK_ANY_SET(rlist, 1u << reg))
            continue;

        if (load) {
            u32 value = mbus_read32(address, MBUS_DEFAULT);

            if (reg == 15)
                pc_value = value;
            else
                cpu->r[reg] = value;
        } else {
            mbus_write32(address, cpu_read_reg(reg), MBUS_DEFAULT);
        }

        address += 4;
    }

    cpu->sp = new_sp;

    if (load) {
        mbus_add_cycles(1);

        if (MASK_ANY_SET(rlist, 1u << 15))
            thumb_write_pc(pc_value);
    }

    return true;
}

static
bool thumb_load_store_multiple(u16 instruction) {
    cpu_t* cpu = get_cpu();

    bool load = MASK_ANY_SET(instruction, 1u << 11);

    u32 rb = (instruction >> 8) & 7u;
    u32 rlist = instruction & 0xffu;

    u32 base = cpu->r[rb];
    u32 address = base;

    if (rlist == 0u) {
        if (load) {
            u32 target = mbus_read32(address, MBUS_DEFAULT);

            cpu->r[rb] = base + 0x40;

            mbus_add_cycles(1);
            thumb_write_pc(target);
        } else {
            mbus_write32(address, cpu_read_reg(15) + 2, MBUS_DEFAULT);

            cpu->r[rb] = base + 0x40;
        }

        return true;
    }

    u32 new_base = base + cpu_count_register_list(rlist) * 4;

    bool base_first = !MASK_ANY_SET(rlist, (1u << rb) - 1u);

    for (u32 reg = 0; reg < 8; reg++) {
        if (!MASK_ANY_SET(rlist, 1u << reg))
            continue;

        if (load) {
            cpu->r[reg] = mbus_read32(address, MBUS_DEFAULT);
        } else {
            u32 value = cpu->r[reg];

            if (reg == rb && !base_first)
                value = new_base;

            mbus_write32(address, value, MBUS_DEFAULT);
        }

        address += 4;
    }

    if (!load || !MASK_ANY_SET(rlist, 1u << rb))
        cpu->r[rb] = new_base;

    if (load)
        mbus_add_cycles(1);

    return true;
}

static
bool thumb_branch_conditional(u16 instruction) {
    cpu_t* cpu = get_cpu();

    u32 condition = (instruction >> 8) & 0xfu;

    if (condition == 0xf)
        return cpu_swi(instruction & 0xffu);

    if (condition == 0xe) {
        printf("undefined thumb branch conditional instruction: 0x%04x\n", instruction);
        return false;
    }

    if (!cpu_check_instruction_cond(condition, cpu->cpsr))
        return true;

    i32 offset = thumb_sign_extend(instruction & 0xffu, 8u) * 2;

    thumb_write_pc(cpu_read_reg(15) + (u32)offset);

    return true;
}

static
bool thumb_branch(u16 instruction) {
    i32 offset = thumb_sign_extend(instruction & 0x7ffu, 11u) * 2;

    thumb_write_pc(cpu_read_reg(15) + (u32)offset);

    return true;
}

static
bool thumb_branch_link_prefix(u16 instruction) {
    cpu_t* cpu = get_cpu();

    i32 offset = thumb_sign_extend(instruction & 0x7ffu, 11u) * 4096;

    cpu->lr = cpu_read_reg(15) + (u32)offset;

    return true;
}

static
bool thumb_branch_link_suffix(u16 instruction) {
    cpu_t* cpu = get_cpu();

    u32 target = cpu->lr + ((instruction & 0x7ffu) << 1);

    u32 return_address = cpu->pc;
    MASK_SET(return_address, 1u);

    cpu->lr = return_address;
    thumb_write_pc(target);

    return true;
}

static
bool thumb_decode_shift_add_sub(u16 instruction) {
    if (MASK_MATCHES(instruction, THUMB_FMT_ADD_SUB_MASK, THUMB_FMT_ADD_SUB_VALUE))
        return thumb_add_subtract(instruction);

    return thumb_shift_immediate(instruction);
}

static
bool thumb_decode_alu_load_store(u16 instruction) {
    if (MASK_MATCHES(instruction, THUMB_FMT_ALU_MASK, THUMB_FMT_ALU_VALUE))
        return thumb_alu_register(instruction);

    if (MASK_MATCHES(instruction, THUMB_FMT_HIGH_REGISTER_MASK, THUMB_FMT_HIGH_REGISTER_VALUE))
        return thumb_high_register(instruction);

    if (MASK_MATCHES(instruction, THUMB_FMT_LITERAL_LOAD_MASK, THUMB_FMT_LITERAL_LOAD_VALUE))
        return thumb_load_literal(instruction);

    if (MASK_MATCHES(instruction, THUMB_FMT_REGISTER_TRANSFER_MASK, THUMB_FMT_REGISTER_TRANSFER_VALUE))
        return thumb_load_store_register(instruction);

    if (MASK_MATCHES(instruction, THUMB_FMT_SIGNED_TRANSFER_MASK, THUMB_FMT_SIGNED_TRANSFER_VALUE))
        return thumb_load_store_signed(instruction);

    printf("undefined thumb decode alu load store instruction: 0x%04x\n", instruction);
    return false;
}

static
bool thumb_decode_halfword_sp_relative(u16 instruction) {
    if (MASK_ANY_SET(instruction, 1u << 12))
        return thumb_load_store_sp_relative(instruction);

    return thumb_load_store_halfword_immediate(instruction);
}

static
bool thumb_decode_address_stack(u16 instruction) {
    if (MASK_MATCHES(instruction, THUMB_FMT_LOAD_ADDRESS_MASK, THUMB_FMT_LOAD_ADDRESS_VALUE))
        return thumb_load_address(instruction);

    if (MASK_MATCHES(instruction, THUMB_FMT_ADJUST_SP_MASK, THUMB_FMT_ADJUST_SP_VALUE))
        return thumb_adjust_sp(instruction);

    if (MASK_MATCHES(instruction, THUMB_FMT_PUSH_POP_MASK, THUMB_FMT_PUSH_POP_VALUE))
        return thumb_push_pop(instruction);

    printf("undefined thumb decode address stack instruction: 0x%04x\n", instruction);
    return false;
}

static
bool thumb_decode_multiple_conditional(u16 instruction) {
    if (!MASK_ANY_SET(instruction, 1u << 12))
        return thumb_load_store_multiple(instruction);

    return thumb_branch_conditional(instruction);
}

static
bool thumb_decode_branch_long(u16 instruction) {
    if (MASK_MATCHES(instruction, THUMB_FMT_BRANCH_MASK, THUMB_FMT_BRANCH_VALUE))
        return thumb_branch(instruction);

    if (MASK_MATCHES(instruction, THUMB_FMT_BL_PREFIX_MASK, THUMB_FMT_BL_PREFIX_VALUE))
        return thumb_branch_link_prefix(instruction);

    if (MASK_MATCHES(instruction, THUMB_FMT_BL_SUFFIX_MASK, THUMB_FMT_BL_SUFFIX_VALUE))
        return thumb_branch_link_suffix(instruction);

    printf("undefined thumb decode branch long instruction: 0x%04x\n", instruction);
    return false;
}

static
const thumb_instruction_handler_t thumb_handlers[8] = {
    thumb_decode_shift_add_sub,
    thumb_data_processing_immediate,
    thumb_decode_alu_load_store,
    thumb_load_store_immediate,
    thumb_decode_halfword_sp_relative,
    thumb_decode_address_stack,
    thumb_decode_multiple_conditional,
    thumb_decode_branch_long
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