#include "gba_internal.h"
#include <string.h>

static uint32_t *bank_sp(GBA *gba, uint32_t mode) {
    switch (mode & PSR_MODE) {
        case MODE_FIQ: return &gba->cpu.bank_fiq[0];
        case MODE_IRQ: return &gba->cpu.bank_irq[0];
        case MODE_SVC: return &gba->cpu.bank_svc[0];
        case MODE_ABT: return &gba->cpu.bank_abt[0];
        case MODE_UND: return &gba->cpu.bank_und[0];
        default: return &gba->cpu.bank_usr[0];
    }
}

static uint32_t *bank_lr(GBA *gba, uint32_t mode) {
    return bank_sp(gba, mode) + 1;
}

static uint32_t *bank_spsr(GBA *gba, uint32_t mode) {
    switch (mode & PSR_MODE) {
        case MODE_FIQ: return &gba->cpu.spsr_fiq;
        case MODE_IRQ: return &gba->cpu.spsr_irq;
        case MODE_SVC: return &gba->cpu.spsr_svc;
        case MODE_ABT: return &gba->cpu.spsr_abt;
        case MODE_UND: return &gba->cpu.spsr_und;
        default: return NULL;
    }
}

void cpu_switch_mode(GBA *gba, uint32_t mode) {
    uint32_t old = gba->cpu.cpsr & PSR_MODE;
    mode &= PSR_MODE;
    if (old == mode) return;

    *bank_sp(gba, old) = gba->cpu.r[13];
    *bank_lr(gba, old) = gba->cpu.r[14];
    uint32_t *ospsr = bank_spsr(gba, old);
    if (ospsr) *ospsr = gba->cpu.spsr;

    if (old == MODE_FIQ) {
        for (int i = 0; i < 5; i++) {
            uint32_t tmp = gba->cpu.r[8 + i];
            gba->cpu.r[8 + i] = gba->cpu.bank_r8_fiq[i];
            gba->cpu.bank_r8_fiq[i] = tmp;
        }
    }
    if (mode == MODE_FIQ) {
        for (int i = 0; i < 5; i++) {
            uint32_t tmp = gba->cpu.r[8 + i];
            gba->cpu.r[8 + i] = gba->cpu.bank_r8_fiq[i];
            gba->cpu.bank_r8_fiq[i] = tmp;
        }
    }

    gba->cpu.r[13] = *bank_sp(gba, mode);
    gba->cpu.r[14] = *bank_lr(gba, mode);
    uint32_t *nspsr = bank_spsr(gba, mode);
    gba->cpu.spsr = nspsr ? *nspsr : gba->cpu.cpsr;
    gba->cpu.cpsr = (gba->cpu.cpsr & ~PSR_MODE) | mode;
}

void cpu_set_cpsr(GBA *gba, uint32_t value, bool flags, bool control) {
    uint32_t cpsr = gba->cpu.cpsr;
    if (flags)
        cpsr = (cpsr & 0x0FFFFFFFu) | (value & 0xF0000000u);
    if (control) {
        uint32_t new_mode = value & PSR_MODE;
        cpu_switch_mode(gba, new_mode);
        cpsr = (gba->cpu.cpsr & 0xFFFFFF00u) | (value & 0xFF);
        if (value & PSR_T)
            cpsr |= PSR_T;
        else
            cpsr &= ~PSR_T;
        gba->cpu.cpsr = (cpsr & ~PSR_MODE) | (gba->cpu.cpsr & PSR_MODE);
        gba->cpu.cpsr = (gba->cpu.cpsr & ~0xF00000FFu) | (cpsr & 0xF00000FFu);
        gba->cpu.cpsr = (gba->cpu.cpsr & ~PSR_MODE) | new_mode;
        return;
    }
    gba->cpu.cpsr = cpsr;
}

void cpu_reset(GBA *gba) {
    memset(&gba->cpu, 0, sizeof(gba->cpu));
    gba->cpu.cpsr = MODE_SYS;
    gba->cpu.r[13] = 0x03007F00;
    gba->cpu.bank_irq[0] = 0x03007FA0;
    gba->cpu.bank_svc[0] = 0x03007FE0;
    gba->cpu.bank_usr[0] = 0x03007F00;
    gba->cpu.r[15] = 0x08000000;
}

void cpu_interrupt(GBA *gba, uint32_t vector) {
    uint32_t cpsr = gba->cpu.cpsr;
    cpu_switch_mode(gba, MODE_IRQ);
    gba->cpu.spsr = cpsr;
    gba->cpu.cpsr = (gba->cpu.cpsr & ~PSR_T) | PSR_I | MODE_IRQ;
    uint32_t pc = gba->cpu.r[15];
    if (cpsr & PSR_T)
        gba->cpu.r[14] = pc;
    else
        gba->cpu.r[14] = pc - 4;
    gba->cpu.r[15] = vector;
    gba->cpu.halt = false;
}

static bool cond(uint32_t cpsr, uint32_t c) {
    bool n = cpsr & PSR_N, z = cpsr & PSR_Z, cy = cpsr & PSR_C, v = cpsr & PSR_V;
    switch (c) {
        case 0x0: return z;
        case 0x1: return !z;
        case 0x2: return cy;
        case 0x3: return !cy;
        case 0x4: return n;
        case 0x5: return !n;
        case 0x6: return v;
        case 0x7: return !v;
        case 0x8: return cy && !z;
        case 0x9: return !cy || z;
        case 0xA: return n == v;
        case 0xB: return n != v;
        case 0xC: return !z && n == v;
        case 0xD: return z || n != v;
        default: return true;
    }
}

static void nz(ARM7 *cpu, uint32_t v) {
    cpu->cpsr = (cpu->cpsr & ~(PSR_N | PSR_Z)) | (v & PSR_N) | (v == 0 ? PSR_Z : 0);
}

static uint32_t add_flags(ARM7 *cpu, uint32_t a, uint32_t b, int carry_in, bool s) {
    uint64_t r = (uint64_t)a + b + (uint32_t)carry_in;
    uint32_t res = (uint32_t)r;
    if (s) {
        nz(cpu, res);
        if (r >> 32) cpu->cpsr |= PSR_C; else cpu->cpsr &= ~PSR_C;
        if ((~(a ^ b) & (a ^ res)) & 0x80000000u) cpu->cpsr |= PSR_V; else cpu->cpsr &= ~PSR_V;
    }
    return res;
}

static uint32_t sub_flags(ARM7 *cpu, uint32_t a, uint32_t b, int carry_in, bool s) {
    /* carry_in 1 means no borrow for SBC inverted */
    return add_flags(cpu, a, ~b, carry_in, s);
}

static uint32_t ror32(uint32_t v, uint32_t r) {
    r &= 31;
    if (!r) return v;
    return (v >> r) | (v << (32 - r));
}

static uint32_t shift_imm(ARM7 *cpu, uint32_t type, uint32_t imm, uint32_t val, bool s, uint32_t *carry) {
    uint32_t c = (cpu->cpsr & PSR_C) ? 1u : 0u;
    switch (type) {
        case 0: /* LSL */
            if (imm == 0) { *carry = c; return val; }
            *carry = (val >> (32 - imm)) & 1;
            return val << imm;
        case 1: /* LSR */
            if (imm == 0) { *carry = val >> 31; return 0; }
            *carry = (val >> (imm - 1)) & 1;
            return val >> imm;
        case 2: /* ASR */
            if (imm == 0) { *carry = val >> 31; return (int32_t)val >> 31; }
            *carry = (val >> (imm - 1)) & 1;
            return (uint32_t)((int32_t)val >> imm);
        default: /* ROR / RRX */
            if (imm == 0) {
                *carry = val & 1;
                return (c << 31) | (val >> 1);
            }
            *carry = (val >> (imm - 1)) & 1;
            return ror32(val, imm);
    }
}

static uint32_t shift_reg(ARM7 *cpu, uint32_t type, uint32_t rs, uint32_t val, uint32_t *carry) {
    uint32_t amount = rs & 0xFF;
    uint32_t c = (cpu->cpsr & PSR_C) ? 1u : 0u;
    *carry = c;
    if (amount == 0) return val;
    switch (type) {
        case 0:
            if (amount >= 32) {
                *carry = amount == 32 ? (val & 1) : 0;
                return 0;
            }
            *carry = (val >> (32 - amount)) & 1;
            return val << amount;
        case 1:
            if (amount >= 32) {
                *carry = amount == 32 ? (val >> 31) : 0;
                return 0;
            }
            *carry = (val >> (amount - 1)) & 1;
            return val >> amount;
        case 2:
            if (amount >= 32) {
                *carry = val >> 31;
                return (int32_t)val >> 31;
            }
            *carry = (val >> (amount - 1)) & 1;
            return (uint32_t)((int32_t)val >> amount);
        default:
            amount &= 31;
            if (rs & 0xFF) {
                if ((rs & 0xFF) == 32) { *carry = val >> 31; return val; }
                if (amount == 0) { *carry = val >> 31; return val; }
                *carry = (val >> (amount - 1)) & 1;
                return ror32(val, amount);
            }
            return val;
    }
}

static uint32_t op2(GBA *gba, uint32_t instr, bool s, uint32_t *carry) {
    ARM7 *cpu = &gba->cpu;
    *carry = (cpu->cpsr & PSR_C) ? 1u : 0u;
    if (instr & (1u << 25)) {
        uint32_t imm = instr & 0xFF;
        uint32_t rot = ((instr >> 8) & 0xF) * 2;
        uint32_t v = ror32(imm, rot);
        if (rot) *carry = v >> 31;
        return v;
    }
    uint32_t rm = gba->cpu.r[instr & 0xF];
    if ((instr & 0xF) == 15) rm += (gba->cpu.cpsr & PSR_T) ? 2 : 4;
    uint32_t type = (instr >> 5) & 3;
    if (instr & (1u << 4)) {
        uint32_t rs = gba->cpu.r[(instr >> 8) & 0xF] & 0xFF;
        return shift_reg(cpu, type, rs, rm, carry);
    }
    uint32_t imm = (instr >> 7) & 0x1F;
    return shift_imm(cpu, type, imm, rm, s, carry);
}

static void dataproc(GBA *gba, uint32_t instr) {
    ARM7 *cpu = &gba->cpu;
    uint32_t opcode = (instr >> 21) & 0xF;
    bool s = (instr >> 20) & 1;
    uint32_t rn_i = (instr >> 16) & 0xF;
    uint32_t rd_i = (instr >> 12) & 0xF;
    uint32_t rn = cpu->r[rn_i];
    if (rn_i == 15) rn += (cpu->cpsr & PSR_T) ? 2 : 4;
    uint32_t carry;
    uint32_t op = op2(gba, instr, s, &carry);
    uint32_t result = 0;
    bool write = true;
    switch (opcode) {
        case 0x0: result = rn & op; break;
        case 0x1: result = rn ^ op; break;
        case 0x2: result = sub_flags(cpu, rn, op, 1, s); break;
        case 0x3: result = sub_flags(cpu, op, rn, 1, s); break;
        case 0x4: result = add_flags(cpu, rn, op, 0, s); break;
        case 0x5: result = add_flags(cpu, rn, op, (cpu->cpsr & PSR_C) ? 1 : 0, s); break;
        case 0x6: result = sub_flags(cpu, rn, op, (cpu->cpsr & PSR_C) ? 1 : 0, s); break;
        case 0x7: result = sub_flags(cpu, op, rn, (cpu->cpsr & PSR_C) ? 1 : 0, s); break;
        case 0x8: result = rn & op; write = false; break;
        case 0x9: result = rn ^ op; write = false; break;
        case 0xA: result = sub_flags(cpu, rn, op, 1, true); write = false; break;
        case 0xB: result = add_flags(cpu, rn, op, 0, true); write = false; break;
        case 0xC: result = rn | op; break;
        case 0xD: result = op; break;
        case 0xE: result = rn & ~op; break;
        case 0xF: result = ~op; break;
    }
    if (s && (opcode < 2 || opcode > 7) && opcode != 0xA && opcode != 0xB) {
        nz(cpu, result);
        if (opcode != 0x8 || true) {
            if (carry) cpu->cpsr |= PSR_C; else cpu->cpsr &= ~PSR_C;
        }
        if (opcode == 0x0 || opcode == 0x1 || opcode == 0x8 || opcode == 0x9 ||
            opcode == 0xC || opcode == 0xD || opcode == 0xE || opcode == 0xF) {
            /* logical: V unchanged */
        }
    }
    if (!write) return;
    if (rd_i == 15) {
        if (s && (cpu->cpsr & PSR_MODE) != MODE_USR && (cpu->cpsr & PSR_MODE) != MODE_SYS) {
            uint32_t spsr = cpu->spsr;
            cpu_set_cpsr(gba, spsr, true, true);
        }
        cpu->r[15] = result & ((cpu->cpsr & PSR_T) ? ~1u : ~3u);
        return;
    }
    cpu->r[rd_i] = result;
}

static int exec_arm(GBA *gba, uint32_t instr) {
    ARM7 *cpu = &gba->cpu;
    if (!cond(cpu->cpsr, instr >> 28)) return 1;

    /* BX */
    if ((instr & 0x0FFFFFF0) == 0x012FFF10) {
        uint32_t rm = cpu->r[instr & 0xF];
        if (rm & 1) cpu->cpsr |= PSR_T; else cpu->cpsr &= ~PSR_T;
        cpu->r[15] = rm & ~1u;
        return 3;
    }

    /* MRS */
    if ((instr & 0x0FBF0FFF) == 0x010F0000) {
        uint32_t rd = (instr >> 12) & 0xF;
        cpu->r[rd] = (instr & (1u << 22)) ? cpu->spsr : cpu->cpsr;
        return 1;
    }
    /* MSR */
    if ((instr & 0x0DB0F000) == 0x0120F000) {
        uint32_t operand;
        uint32_t carry;
        if (instr & (1u << 25))
            operand = op2(gba, instr, false, &carry);
        else
            operand = cpu->r[instr & 0xF];
        bool flags = (instr >> 19) & 1;
        bool control = (instr >> 16) & 1;
        if (instr & (1u << 22)) {
            if (flags) cpu->spsr = (cpu->spsr & 0x0FFFFFFF) | (operand & 0xF0000000);
            if (control) cpu->spsr = (cpu->spsr & 0xFFFFFF00) | (operand & 0xFF);
        } else {
            cpu_set_cpsr(gba, operand, flags, control);
        }
        return 1;
    }

    /* Multiply */
    if ((instr & 0x0FC000F0) == 0x00000090) {
        uint32_t rd = (instr >> 16) & 0xF;
        uint32_t rn = (instr >> 12) & 0xF;
        uint32_t rs = (instr >> 8) & 0xF;
        uint32_t rm = instr & 0xF;
        uint32_t res = cpu->r[rm] * cpu->r[rs];
        if (instr & (1u << 21)) res += cpu->r[rn];
        cpu->r[rd] = res;
        if (instr & (1u << 20)) nz(cpu, res);
        return 4;
    }
    if ((instr & 0x0F8000F0) == 0x00800090) {
        uint32_t rdhi = (instr >> 16) & 0xF;
        uint32_t rdlo = (instr >> 12) & 0xF;
        uint32_t rs = (instr >> 8) & 0xF;
        uint32_t rm = instr & 0xF;
        bool is_signed = (instr & (1u << 22)) != 0;
        uint64_t res;
        if (is_signed)
            res = (int64_t)(int32_t)cpu->r[rm] * (int64_t)(int32_t)cpu->r[rs];
        else
            res = (uint64_t)cpu->r[rm] * (uint64_t)cpu->r[rs];
        if (instr & (1u << 21))
            res += ((uint64_t)cpu->r[rdhi] << 32) | cpu->r[rdlo];
        cpu->r[rdlo] = (uint32_t)res;
        cpu->r[rdhi] = (uint32_t)(res >> 32);
        if (instr & (1u << 20)) {
            nz(cpu, cpu->r[rdhi] ? cpu->r[rdhi] : cpu->r[rdlo]);
            if ((res >> 32) == 0 && (uint32_t)res == 0) cpu->cpsr |= PSR_Z; 
        }
        return 5;
    }

    /* SWP */
    if ((instr & 0x0FB00FF0) == 0x01000090) {
        uint32_t rn = cpu->r[(instr >> 16) & 0xF];
        uint32_t rd = (instr >> 12) & 0xF;
        uint32_t rm = cpu->r[instr & 0xF];
        if (instr & (1u << 22)) {
            uint32_t b = gba_read8(gba, rn);
            gba_write8(gba, rn, rm);
            cpu->r[rd] = b;
        } else {
            uint32_t w = gba_read32(gba, rn);
            gba_write32(gba, rn, rm);
            cpu->r[rd] = w;
        }
        return 4;
    }

    /* Halfword extra load/store */
    if ((instr & 0x0E000090) == 0x00000090 && (instr & 0x00000060)) {
        bool p = (instr >> 24) & 1;
        bool u = (instr >> 23) & 1;
        bool w = (instr >> 21) & 1;
        bool l = (instr >> 20) & 1;
        uint32_t rn_i = (instr >> 16) & 0xF;
        uint32_t rd_i = (instr >> 12) & 0xF;
        uint32_t rn = cpu->r[rn_i];
        uint32_t off;
        if (instr & (1u << 22))
            off = ((instr >> 4) & 0xF0) | (instr & 0xF);
        else
            off = cpu->r[instr & 0xF];
        uint32_t addr = p ? (u ? rn + off : rn - off) : rn;
        uint32_t sh = (instr >> 5) & 3;
        if (l) {
            if (sh == 1) cpu->r[rd_i] = gba_read16(gba, addr);
            else if (sh == 2) cpu->r[rd_i] = (int8_t)gba_read8(gba, addr);
            else cpu->r[rd_i] = (int16_t)gba_read16(gba, addr);
        } else {
            if (sh == 1) gba_write16(gba, addr, cpu->r[rd_i]);
            else if (sh == 2) gba_write8(gba, addr, cpu->r[rd_i]);
            else gba_write16(gba, addr, cpu->r[rd_i]);
        }
        uint32_t wb = u ? rn + off : rn - off;
        if (!p || w) {
            if (rn_i != 15) cpu->r[rn_i] = wb;
        }
        return 3;
    }

    /* Single data transfer */
    if ((instr & 0x0C000000) == 0x04000000) {
        bool i = (instr >> 25) & 1;
        bool p = (instr >> 24) & 1;
        bool u = (instr >> 23) & 1;
        bool b = (instr >> 22) & 1;
        bool w = (instr >> 21) & 1;
        bool l = (instr >> 20) & 1;
        uint32_t rn_i = (instr >> 16) & 0xF;
        uint32_t rd_i = (instr >> 12) & 0xF;
        uint32_t rn = cpu->r[rn_i];
        if (rn_i == 15) rn += 4;
        uint32_t off;
        if (!i)
            off = instr & 0xFFF;
        else {
            uint32_t carry;
            uint32_t fake = (instr & 0xFFF) | (1u << 25);
            /* register shifted */
            uint32_t rm = cpu->r[instr & 0xF];
            uint32_t type = (instr >> 5) & 3;
            uint32_t imm = (instr >> 7) & 0x1F;
            off = shift_imm(cpu, type, imm, rm, false, &carry);
            (void)fake;
        }
        uint32_t addr = p ? (u ? rn + off : rn - off) : rn;
        if (l) {
            uint32_t val = b ? gba_read8(gba, addr) : gba_read32(gba, addr);
            if (rd_i == 15) {
                if (val & 1) cpu->cpsr |= PSR_T; else cpu->cpsr &= ~PSR_T;
                cpu->r[15] = val & ~1u;
            } else
                cpu->r[rd_i] = val;
        } else {
            uint32_t val = cpu->r[rd_i];
            if (rd_i == 15) val += 4;
            if (b) gba_write8(gba, addr, val);
            else gba_write32(gba, addr, val);
        }
        uint32_t wb = u ? rn + off : rn - off;
        if (!p || w) {
            if (rn_i != 15 && (!l || rd_i != rn_i)) cpu->r[rn_i] = wb;
        }
        return 3;
    }

    /* Block data transfer */
    if ((instr & 0x0E000000) == 0x08000000) {
        bool p = (instr >> 24) & 1;
        bool u = (instr >> 23) & 1;
        bool s = (instr >> 22) & 1;
        bool w = (instr >> 21) & 1;
        bool l = (instr >> 20) & 1;
        uint32_t rn_i = (instr >> 16) & 0xF;
        uint32_t list = instr & 0xFFFF;
        uint32_t addr = cpu->r[rn_i];
        int count = list ? __builtin_popcount(list) : 16;
        uint32_t start = u
            ? (p ? addr + 4 : addr)
            : (p ? addr - 4 * (uint32_t)count : addr - 4 * (uint32_t)count + 4);
        uint32_t a = start;
        uint32_t orig = cpu->r[rn_i];
        if (w) {
            cpu->r[rn_i] = u ? orig + 4 * (uint32_t)count : orig - 4 * (uint32_t)count;
        }
        if (!list) {
            if (l) cpu->r[15] = gba_read32(gba, a);
            else gba_write32(gba, a, cpu->r[15] + 4);
            return 3;
        }
        for (int i = 0; i < 16; i++) {
            if (!(list & (1u << i))) continue;
            if (l) {
                uint32_t val = gba_read32(gba, a);
                if (i == 15) {
                    if (s) cpu_set_cpsr(gba, cpu->spsr, true, true);
                    cpu->r[15] = val & ((cpu->cpsr & PSR_T) ? ~1u : ~3u);
                } else {
                    cpu->r[i] = val;
                }
            } else {
                uint32_t val = (i == 15) ? cpu->r[15] + 4 : cpu->r[i];
                if (i == (int)rn_i && (list & (1u << rn_i)) && i == __builtin_ctz(list))
                    val = orig;
                gba_write32(gba, a, val);
            }
            a += 4;
        }
        (void)s;
        return 3 + count;
    }

    /* Branch */
    if ((instr & 0x0E000000) == 0x0A000000) {
        int32_t off = (int32_t)(instr << 8) >> 6;
        if (instr & (1u << 24))
            cpu->r[14] = cpu->r[15] - 4;
        /* r15 is already at instr+4; ARM pipeline is instr+8 */
        cpu->r[15] = (cpu->r[15] + 4 + (uint32_t)off) & ~3u;
        return 3;
    }

    /* SWI */
    if ((instr & 0x0F000000) == 0x0F000000) {
        gba_hle_swi(gba, (instr >> 16) & 0xFF);
        return 3;
    }

    /* Data processing (remaining 00x) */
    if ((instr & 0x0C000000) == 0) {
        dataproc(gba, instr);
        return 1;
    }
    return 1;
}

static int exec_thumb(GBA *gba, uint16_t op) {
    ARM7 *cpu = &gba->cpu;
    uint32_t *r = cpu->r;

    if ((op & 0xF800) == 0x0000 || (op & 0xF800) == 0x0800 || (op & 0xF800) == 0x1000) {
        uint32_t rd = op & 7, rs = (op >> 3) & 7, off = (op >> 6) & 31;
        uint32_t type = (op >> 11) & 3;
        uint32_t carry = 0, val;
        if (type == 0) {
            val = shift_imm(cpu, 0, off, r[rs], true, &carry);
        } else if (type == 1) {
            val = shift_imm(cpu, 1, off, r[rs], true, &carry);
        } else {
            val = shift_imm(cpu, 2, off, r[rs], true, &carry);
        }
        r[rd] = val;
        nz(cpu, val);
        if (carry) cpu->cpsr |= PSR_C; else cpu->cpsr &= ~PSR_C;
        return 1;
    }
    if ((op & 0xF800) == 0x1800) {
        uint32_t rd = op & 7, rs = (op >> 3) & 7, rn = (op >> 6) & 7;
        bool imm = (op >> 10) & 1;
        bool sub = (op >> 9) & 1;
        uint32_t b = imm ? rn : r[rn];
        r[rd] = sub ? sub_flags(cpu, r[rs], b, 1, true) : add_flags(cpu, r[rs], b, 0, true);
        return 1;
    }
    if ((op & 0xE000) == 0x2000) {
        uint32_t rd = (op >> 8) & 7, imm = op & 0xFF;
        switch ((op >> 11) & 3) {
            case 0: r[rd] = imm; nz(cpu, r[rd]); break;
            case 1: sub_flags(cpu, r[rd], imm, 1, true); break;
            case 2: r[rd] = add_flags(cpu, r[rd], imm, 0, true); break;
            case 3: r[rd] = sub_flags(cpu, r[rd], imm, 1, true); break;
        }
        return 1;
    }
    if ((op & 0xFC00) == 0x4000) {
        uint32_t rd = op & 7, rs = (op >> 3) & 7;
        uint32_t a = r[rd], b = r[rs], carry, res;
        switch ((op >> 6) & 0xF) {
            case 0: res = a & b; nz(cpu, res); r[rd] = res; break;
            case 1: res = a ^ b; nz(cpu, res); r[rd] = res; break;
            case 2:
                res = shift_reg(cpu, 0, b, a, &carry); nz(cpu, res);
                if (carry) cpu->cpsr |= PSR_C; else cpu->cpsr &= ~PSR_C;
                r[rd] = res; break;
            case 3:
                res = shift_reg(cpu, 1, b, a, &carry); nz(cpu, res);
                if (carry) cpu->cpsr |= PSR_C; else cpu->cpsr &= ~PSR_C;
                r[rd] = res; break;
            case 4:
                res = shift_reg(cpu, 2, b, a, &carry); nz(cpu, res);
                if (carry) cpu->cpsr |= PSR_C; else cpu->cpsr &= ~PSR_C;
                r[rd] = res; break;
            case 5: r[rd] = add_flags(cpu, a, b, (cpu->cpsr & PSR_C) ? 1 : 0, true); break;
            case 6: r[rd] = sub_flags(cpu, a, b, (cpu->cpsr & PSR_C) ? 1 : 0, true); break;
            case 7:
                res = shift_reg(cpu, 3, b, a, &carry); nz(cpu, res);
                if (carry) cpu->cpsr |= PSR_C; else cpu->cpsr &= ~PSR_C;
                r[rd] = res; break;
            case 8: nz(cpu, a & b); break;
            case 9: r[rd] = sub_flags(cpu, 0, b, 1, true); break;
            case 10: sub_flags(cpu, a, b, 1, true); break;
            case 11: add_flags(cpu, a, b, 0, true); break;
            case 12: res = a | b; nz(cpu, res); r[rd] = res; break;
            case 13: res = a * b; nz(cpu, res); r[rd] = res; break;
            case 14: res = a & ~b; nz(cpu, res); r[rd] = res; break;
            case 15: res = ~b; nz(cpu, res); r[rd] = res; break;
        }
        return 1;
    }
    if ((op & 0xFC00) == 0x4400) {
        uint32_t rd = (op & 7) | ((op >> 4) & 8);
        uint32_t rs = (op >> 3) & 0xF;
        uint32_t h = (op >> 8) & 3;
        if (h == 3) {
            uint32_t v = r[rs];
            if (v & 1) cpu->cpsr |= PSR_T; else cpu->cpsr &= ~PSR_T;
            r[15] = v & ~1u;
            return 3;
        }
        uint32_t a = r[rd], b = r[rs];
        if (rd == 15) a += 2;
        if (rs == 15) b += 2;
        if (h == 1) sub_flags(cpu, a, b, 1, true);
        else if (h == 0) {
            if (rd == 15) r[15] = (a + b) & ~1u;
            else r[rd] = a + b;
        } else {
            if (rd == 15) r[15] = b & ~1u;
            else r[rd] = b;
        }
        return 1;
    }
    if ((op & 0xF800) == 0x4800) {
        uint32_t rd = (op >> 8) & 7;
        uint32_t addr = (r[15] & ~2u) + ((op & 0xFF) << 2);
        r[rd] = gba_read32(gba, addr);
        return 3;
    }
    if ((op & 0xF200) == 0x5000 || (op & 0xF200) == 0x5200) {
        /* STR/LDR reg offset */
    }
    if ((op & 0xF000) == 0x5000) {
        uint32_t rd = op & 7, rb = (op >> 3) & 7, ro = (op >> 6) & 7;
        uint32_t addr = r[rb] + r[ro];
        int op5 = (op >> 9) & 7;
        switch (op5) {
            case 0: gba_write32(gba, addr, r[rd]); break;
            case 1: gba_write16(gba, addr, r[rd]); break;
            case 2: gba_write8(gba, addr, r[rd]); break;
            case 3: r[rd] = (int8_t)gba_read8(gba, addr); break;
            case 4: r[rd] = gba_read32(gba, addr); break;
            case 5: r[rd] = gba_read16(gba, addr); break;
            case 6: r[rd] = gba_read8(gba, addr); break;
            case 7: r[rd] = (int16_t)gba_read16(gba, addr); break;
        }
        return 3;
    }
    if ((op & 0xE000) == 0x6000) {
        uint32_t rd = op & 7, rb = (op >> 3) & 7, off = (op >> 6) & 31;
        uint32_t addr;
        if ((op & 0x1000) == 0) {
            addr = r[rb] + (off << 2);
            if (op & 0x0800) r[rd] = gba_read32(gba, addr);
            else gba_write32(gba, addr, r[rd]);
        } else {
            addr = r[rb] + off;
            if (op & 0x0800) r[rd] = gba_read8(gba, addr);
            else gba_write8(gba, addr, r[rd]);
        }
        return 3;
    }
    if ((op & 0xF000) == 0x8000) {
        uint32_t rd = op & 7, rb = (op >> 3) & 7, off = (op >> 6) & 31;
        uint32_t addr = r[rb] + (off << 1);
        if (op & 0x0800) r[rd] = gba_read16(gba, addr);
        else gba_write16(gba, addr, r[rd]);
        return 3;
    }
    if ((op & 0xF000) == 0x9000) {
        uint32_t rd = (op >> 8) & 7;
        uint32_t addr = r[13] + ((op & 0xFF) << 2);
        if (op & 0x0800) r[rd] = gba_read32(gba, addr);
        else gba_write32(gba, addr, r[rd]);
        return 3;
    }
    if ((op & 0xF000) == 0xA000) {
        uint32_t rd = (op >> 8) & 7;
        uint32_t src = (op & 0x0800) ? r[13] : (r[15] & ~2u);
        r[rd] = src + ((op & 0xFF) << 2);
        return 1;
    }
    if ((op & 0xFF00) == 0xB000) {
        int32_t imm = (op & 0x7F) << 2;
        if (op & 0x80) r[13] -= imm; else r[13] += imm;
        return 1;
    }
    if ((op & 0xF600) == 0xB400) {
        uint32_t list = op & 0xFF;
        bool pc_lr = (op >> 8) & 1;
        bool load = (op >> 11) & 1;
        if (!load) {
            if (pc_lr) {
                r[13] -= 4;
                gba_write32(gba, r[13], r[14]);
            }
            for (int i = 7; i >= 0; i--) {
                if (list & (1 << i)) {
                    r[13] -= 4;
                    gba_write32(gba, r[13], r[i]);
                }
            }
        } else {
            for (int i = 0; i < 8; i++) {
                if (list & (1 << i)) {
                    r[i] = gba_read32(gba, r[13]);
                    r[13] += 4;
                }
            }
            if (pc_lr) {
                uint32_t pc = gba_read32(gba, r[13]);
                r[13] += 4;
                if (pc & 1) cpu->cpsr |= PSR_T; else cpu->cpsr &= ~PSR_T;
                r[15] = pc & ~1u;
            }
        }
        return 4;
    }
    if ((op & 0xF000) == 0xC000) {
        uint32_t rb = (op >> 8) & 7, list = op & 0xFF;
        bool l = (op >> 11) & 1;
        uint32_t addr = r[rb];
        if (!list) {
            if (l) r[15] = gba_read32(gba, addr);
            else gba_write32(gba, addr, r[15] + 2);
            r[rb] = addr + 0x40;
            return 3;
        }
        for (int i = 0; i < 8; i++) {
            if (!(list & (1 << i))) continue;
            if (l) r[i] = gba_read32(gba, addr);
            else gba_write32(gba, addr, r[i]);
            addr += 4;
        }
        if (!(l && (list & (1 << rb)))) r[rb] = addr;
        return 3;
    }
    if ((op & 0xF000) == 0xD000) {
        if ((op & 0xFF00) == 0xDF00) {
            gba_hle_swi(gba, op & 0xFF);
            return 3;
        }
        if ((op & 0xFF00) == 0xDE00) return 1;
        if (cond(cpu->cpsr, (op >> 8) & 0xF)) {
            int32_t off = (int8_t)(op & 0xFF);
            r[15] = r[15] + 2 + (uint32_t)(off << 1);
        }
        return 3;
    }
    if ((op & 0xF800) == 0xE000) {
        int32_t off = (int32_t)(op << 21) >> 20;
        r[15] = r[15] + 2 + (uint32_t)off;
        return 3;
    }
    if ((op & 0xF800) == 0xF000 || (op & 0xF800) == 0xF800) {
        /* BL prefix/suffix handled in cpu_step */
    }
    return 1;
}

int cpu_step(GBA *gba) {
    ARM7 *cpu = &gba->cpu;
    if (cpu->halt) return 1;

    if (cpu->r[15] < 0x4000 && cpu->r[15] >= 0x18 && cpu->r[15] < 0x20) {
        /* Synthetic BIOS IRQ: jump to 0x03007FFC */
        uint32_t handler = gba_read32(gba, 0x03007FFC);
        cpu->r[15] = handler;
        return 3;
    }

    if (cpu->cpsr & PSR_T) {
        uint32_t pc = cpu->r[15];
        uint16_t op = (uint16_t)gba_read16(gba, pc);
        gba->last_prefetch = op;
        cpu->r[15] = pc + 2;
        if ((op & 0xF800) == 0xF000) {
            uint16_t op2 = (uint16_t)gba_read16(gba, cpu->r[15]);
            cpu->r[15] += 2;
            if ((op2 & 0xF800) == 0xF800) {
                int32_t hi = (int32_t)(op << 21) >> 9;
                uint32_t lo = (op2 & 0x7FF) << 1;
                cpu->r[14] = (pc + 4) | 1;
                cpu->r[15] = (pc + 4 + (uint32_t)hi + lo) & ~1u;
                return 4;
            }
        }
        return exec_thumb(gba, op);
    }

    uint32_t pc = cpu->r[15];
    uint32_t instr = gba_read32(gba, pc);
    gba->last_prefetch = instr;
    cpu->r[15] = pc + 4;
    return exec_arm(gba, instr);
}
