// The ARM11 interpreter. Translated from Zakuro's zakuro-cpu (Rust, MIT,
// Copyright (c) 2026 fearkov; see LICENSE-zakuro): lib.rs, psr.rs,
// arm/*.rs, thumb.rs, vfp.rs, cp15.rs. User mode only, so the banked
// registers, the SPSR and the privileged forms are gone.

#include "runtime/interpreter.h"

#include <bit>
#include <cmath>
#include <cstring>

namespace runtime {

namespace {

constexpr unsigned PC = 15, LR = 14, SP = 13;

inline uint32_t bits(uint32_t v, unsigned lo, unsigned hi) { return (v >> lo) & ((2u << (hi - lo)) - 1); }
inline bool bit(uint32_t v, unsigned n) { return (v >> n) & 1; }
inline int32_t sign_extend(uint32_t v, unsigned width) {
    unsigned shift = 32 - width;
    return static_cast<int32_t>(v << shift) >> shift;
}
inline uint32_t ror(uint32_t v, unsigned n) { return std::rotr(v, static_cast<int>(n & 31)); }

Exit undefined(const Cpu &, uint32_t op) { return Exit{Exit::Undefined, op}; }
constexpr Exit NONE{};

bool check_condition(uint32_t cond, const Psr &p) {
    switch (cond & 0xF) {
    case 0x0: return p.z;
    case 0x1: return !p.z;
    case 0x2: return p.c;
    case 0x3: return !p.c;
    case 0x4: return p.n;
    case 0x5: return !p.n;
    case 0x6: return p.v;
    case 0x7: return !p.v;
    case 0x8: return p.c && !p.z;
    case 0x9: return !p.c || p.z;
    case 0xA: return p.n == p.v;
    case 0xB: return p.n != p.v;
    case 0xC: return !p.z && p.n == p.v;
    case 0xD: return p.z || p.n != p.v;
    default: return true;
    }
}

// -- arithmetic --------------------------------------------------------------

struct Flags {
    uint32_t r;
    bool c, v;
};

Flags add_with_flags(uint32_t a, uint32_t b) {
    uint32_t r = a + b;
    return {r, r < a, (((a ^ r) & (b ^ r)) >> 31) != 0};
}
Flags adc_with_flags(uint32_t a, uint32_t b, bool cin) {
    uint64_t sum = uint64_t(a) + b + cin;
    uint32_t r = static_cast<uint32_t>(sum);
    return {r, sum > 0xFFFFFFFFull, (((a ^ r) & (b ^ r)) >> 31) != 0};
}
// the carry means "no borrow"
Flags sub_with_flags(uint32_t a, uint32_t b) {
    uint32_t r = a - b;
    return {r, a >= b, (((a ^ b) & (a ^ r)) >> 31) != 0};
}
Flags sbc_with_flags(uint32_t a, uint32_t b, bool cin) {
    uint64_t borrow = !cin;
    uint32_t r = static_cast<uint32_t>(uint64_t(a) - b - borrow);
    return {r, uint64_t(a) >= uint64_t(b) + borrow, (((a ^ b) & (a ^ r)) >> 31) != 0};
}

struct Sat {
    uint32_t value;
    bool saturated;
};
Sat saturate_i32(int64_t v) {
    if (v > INT32_MAX) return {uint32_t(INT32_MAX), true};
    if (v < INT32_MIN) return {uint32_t(INT32_MIN), true};
    return {uint32_t(v), false};
}
Sat signed_saturate(int64_t v, unsigned n) {
    int64_t max = (int64_t(1) << (n - 1)) - 1, min = -(int64_t(1) << (n - 1));
    if (v > max) return {uint32_t(max), true};
    if (v < min) return {uint32_t(min), true};
    return {uint32_t(v), false};
}
Sat unsigned_saturate(int64_t v, unsigned n) {
    int64_t max = n >= 32 ? int64_t(UINT32_MAX) : (int64_t(1) << n) - 1;
    if (v > max) return {uint32_t(max), true};
    if (v < 0) return {0, true};
    return {uint32_t(v), false};
}

// -- the barrel shifter ------------------------------------------------------

enum Shift { LSL, LSR, ASR, ROR };

struct Shifted {
    uint32_t value;
    bool carry;
};

Shifted shift_by_immediate(unsigned ty, uint32_t v, uint32_t amount, bool cin) {
    switch (ty & 3) {
    case LSL:
        if (amount == 0) return {v, cin};
        return {v << amount, ((v >> (32 - amount)) & 1) != 0};
    case LSR: // #0 is #32
        if (amount == 0) return {0, (v >> 31) != 0};
        return {v >> amount, ((v >> (amount - 1)) & 1) != 0};
    case ASR: // #0 is #32
        if (amount == 0) return {uint32_t(int32_t(v) >> 31), (v >> 31) != 0};
        return {uint32_t(int32_t(v) >> amount), ((int32_t(v) >> (amount - 1)) & 1) != 0};
    default: // ROR #0 is RRX, a rotate through the carry
        if (amount == 0) return {(uint32_t(cin) << 31) | (v >> 1), (v & 1) != 0};
        return {ror(v, amount), ((v >> (amount - 1)) & 1) != 0};
    }
}

Shifted shift_by_register(unsigned ty, uint32_t v, uint32_t amount, bool cin) {
    amount &= 0xFF;
    if (amount == 0) return {v, cin};
    switch (ty & 3) {
    case LSL:
        if (amount < 32) return {v << amount, ((v >> (32 - amount)) & 1) != 0};
        if (amount == 32) return {0, (v & 1) != 0};
        return {0, false};
    case LSR:
        if (amount < 32) return {v >> amount, ((v >> (amount - 1)) & 1) != 0};
        if (amount == 32) return {0, (v >> 31) != 0};
        return {0, false};
    case ASR:
        if (amount >= 32) return {uint32_t(int32_t(v) >> 31), (v >> 31) != 0};
        return {uint32_t(int32_t(v) >> amount), ((int32_t(v) >> (amount - 1)) & 1) != 0};
    default: {
        unsigned r = amount & 31;
        if (r == 0) return {v, (v >> 31) != 0};
        return {ror(v, r), ((v >> (r - 1)) & 1) != 0};
    }
    }
}

Shifted decode_operand(const Cpu &cpu, uint32_t op) {
    bool cin = cpu.cpsr.c;
    if (op & (1u << 25)) { // an 8-bit immediate rotated right by twice a 4-bit field
        uint32_t imm = op & 0xFF, rotate = bits(op, 8, 11) * 2;
        if (rotate == 0) return {imm, cin};
        uint32_t v = ror(imm, rotate);
        return {v, (v >> 31) != 0};
    }
    unsigned rm = op & 0xF, ty = bits(op, 5, 6);
    if (!(op & (1u << 4))) return shift_by_immediate(ty, cpu.regs[rm], bits(op, 7, 11), cin);
    unsigned rs = bits(op, 8, 11);
    uint32_t v = rm == PC ? cpu.regs[PC] + 4 : cpu.regs[rm];
    uint32_t amount = rs == PC ? cpu.regs[PC] + 4 : cpu.regs[rs];
    return shift_by_register(ty, v, amount, cin);
}

// -- data processing ---------------------------------------------------------

Exit dataproc(Cpu &cpu, uint32_t op) {
    unsigned opcode = bits(op, 21, 24), rd = bits(op, 12, 15), rn = bits(op, 16, 19);
    bool set_flags = op & (1u << 20);
    Shifted operand = decode_operand(cpu, op);
    // Rn reads one step further with a register-specified shift
    uint32_t a = rn == PC && !(op & (1u << 25)) && (op & (1u << 4)) ? cpu.regs[PC] + 4 : cpu.regs[rn];
    bool cin = cpu.cpsr.c;
    bool compare = opcode >= 0x8 && opcode <= 0xB;
    uint32_t b = operand.value;
    Flags f{0, operand.carry, false};
    bool logical = true;
    switch (opcode) {
    case 0x0: case 0x8: f.r = a & b; break;          // AND, TST
    case 0x1: case 0x9: f.r = a ^ b; break;          // EOR, TEQ
    case 0x2: case 0xA: f = sub_with_flags(a, b); logical = false; break; // SUB, CMP
    case 0x3: f = sub_with_flags(b, a); logical = false; break;           // RSB
    case 0x4: case 0xB: f = add_with_flags(a, b); logical = false; break; // ADD, CMN
    case 0x5: f = adc_with_flags(a, b, cin); logical = false; break;
    case 0x6: f = sbc_with_flags(a, b, cin); logical = false; break;
    case 0x7: f = sbc_with_flags(b, a, cin); logical = false; break;      // RSC
    case 0xC: f.r = a | b; break;
    case 0xD: f.r = b; break;                         // MOV
    case 0xE: f.r = a & ~b; break;                    // BIC
    default: f.r = ~b; break;                         // MVN
    }
    if (set_flags) {
        if (rd == PC && !compare) { // an exception return: no SPSR in user mode
            cpu.set_reg(PC, f.r);
            return NONE;
        }
        cpu.set_nz(f.r);
        cpu.cpsr.c = f.c;
        if (!logical) cpu.cpsr.v = f.v;
    }
    if (!compare) cpu.set_reg(rd, f.r);
    return NONE;
}

// -- multiplies --------------------------------------------------------------

Exit multiply_short(Cpu &cpu, uint32_t op) {
    unsigned rd = bits(op, 16, 19), rn = bits(op, 12, 15), rs = bits(op, 8, 11), rm = op & 0xF;
    uint32_t r = cpu.regs[rm] * cpu.regs[rs];
    if (bit(op, 21)) r += cpu.regs[rn];
    cpu.regs[rd] = r;
    if (bit(op, 20)) cpu.set_nz(r);
    return NONE;
}

Exit multiply_long(Cpu &cpu, uint32_t op) {
    unsigned hi = bits(op, 16, 19), lo = bits(op, 12, 15), rs = bits(op, 8, 11), rm = op & 0xF;
    if (bits(op, 21, 23) == 0b010) { // UMAAL
        uint64_t sum = uint64_t(cpu.regs[rm]) * cpu.regs[rs] + cpu.regs[lo] + cpu.regs[hi];
        cpu.regs[lo] = uint32_t(sum);
        cpu.regs[hi] = uint32_t(sum >> 32);
        return NONE;
    }
    uint64_t acc = bit(op, 21) ? (uint64_t(cpu.regs[hi]) << 32 | cpu.regs[lo]) : 0;
    uint64_t r = bit(op, 22) ? uint64_t(int64_t(int32_t(cpu.regs[rm])) * int32_t(cpu.regs[rs])) + acc
                             : uint64_t(cpu.regs[rm]) * cpu.regs[rs] + acc;
    cpu.regs[lo] = uint32_t(r);
    cpu.regs[hi] = uint32_t(r >> 32);
    if (bit(op, 20)) {
        cpu.cpsr.n = (r >> 63) != 0;
        cpu.cpsr.z = r == 0;
    }
    return NONE;
}

int32_t half(uint32_t v, bool top) { return top ? int16_t(v >> 16) : int16_t(v); }

Exit multiply_halfword(Cpu &cpu, uint32_t op) {
    unsigned rd = bits(op, 16, 19), rn = bits(op, 12, 15), rs = bits(op, 8, 11), rm = op & 0xF;
    bool x = bit(op, 5), y = bit(op, 6);
    switch (bits(op, 21, 22)) {
    case 0b00: { // SMLAxy
        int64_t sum = int64_t(half(cpu.regs[rm], x)) * half(cpu.regs[rs], y) + int32_t(cpu.regs[rn]);
        cpu.regs[rd] = uint32_t(sum);
        cpu.cpsr.q |= sum != int32_t(sum);
        break;
    }
    case 0b01: { // SMLAWy / SMULWy
        int64_t product = (int64_t(int32_t(cpu.regs[rm])) * half(cpu.regs[rs], y)) >> 16;
        if (x) {
            cpu.regs[rd] = uint32_t(product);
        } else {
            int64_t sum = product + int32_t(cpu.regs[rn]);
            cpu.regs[rd] = uint32_t(sum);
            cpu.cpsr.q |= sum != int32_t(sum);
        }
        break;
    }
    case 0b10: { // SMLALxy
        int64_t product = int64_t(half(cpu.regs[rm], x)) * half(cpu.regs[rs], y);
        uint64_t sum = (uint64_t(cpu.regs[rd]) << 32 | cpu.regs[rn]) + uint64_t(product);
        cpu.regs[rn] = uint32_t(sum);
        cpu.regs[rd] = uint32_t(sum >> 32);
        break;
    }
    default: // SMULxy
        cpu.regs[rd] = uint32_t(int64_t(half(cpu.regs[rm], x)) * half(cpu.regs[rs], y));
    }
    return NONE;
}

Exit multiply_saturating(Cpu &cpu, uint32_t op) {
    unsigned rd = bits(op, 12, 15), rn = bits(op, 16, 19), rm = op & 0xF;
    int64_t a = int32_t(cpu.regs[rm]), b = int32_t(cpu.regs[rn]);
    Sat s;
    switch (bits(op, 21, 22)) {
    case 0b00: s = saturate_i32(a + b); break;
    case 0b01: s = saturate_i32(a - b); break;
    case 0b10: {
        Sat d = saturate_i32(b * 2);
        s = saturate_i32(a + int32_t(d.value));
        s.saturated |= d.saturated;
        break;
    }
    default: {
        Sat d = saturate_i32(b * 2);
        s = saturate_i32(a - int32_t(d.value));
        s.saturated |= d.saturated;
    }
    }
    cpu.regs[rd] = s.value;
    cpu.cpsr.q |= s.saturated;
    return NONE;
}

// -- loads and stores --------------------------------------------------------

Exit load_store_single(Cpu &cpu, Memory &bus, uint32_t op) {
    unsigned rn = bits(op, 16, 19), rd = bits(op, 12, 15);
    bool pre = bit(op, 24), add = bit(op, 23), byte = bit(op, 22), wb = bit(op, 21), load = bit(op, 20);
    uint32_t offset = bit(op, 25)
                          ? shift_by_immediate(bits(op, 5, 6), cpu.regs[op & 0xF], bits(op, 7, 11), cpu.cpsr.c).value
                          : op & 0xFFF;
    uint32_t base = cpu.regs[rn];
    uint32_t offset_addr = add ? base + offset : base - offset;
    uint32_t addr = pre ? offset_addr : base;
    if (load) {
        uint32_t v = byte ? bus.read8(addr) : bus.read32(addr);
        // the base first, so ldr rN, [rN], #imm ends with the value
        if ((!pre || wb) && rn != rd) cpu.regs[rn] = offset_addr;
        if (rd == PC) cpu.branch_exchange(v); // a load into the PC interworks
        else cpu.set_reg(rd, v);
    } else {
        uint32_t v = rd == PC ? cpu.regs[PC] + 4 : cpu.regs[rd]; // pc + 12
        if (byte) bus.write8(addr, uint8_t(v));
        else bus.write32(addr, v);
        if (!pre || wb) cpu.regs[rn] = offset_addr;
    }
    return NONE;
}

Exit load_store_extra(Cpu &cpu, Memory &bus, uint32_t op) {
    unsigned rn = bits(op, 16, 19), rd = bits(op, 12, 15), sh = bits(op, 5, 6);
    bool pre = bit(op, 24), add = bit(op, 23), imm = bit(op, 22), wb = bit(op, 21), load = bit(op, 20);
    if (!load && sh >= 2 && rd == PC) return undefined(cpu, op); // r15 has no partner
    uint32_t offset = imm ? (bits(op, 8, 11) << 4) | (op & 0xF) : cpu.regs[op & 0xF];
    uint32_t base = cpu.regs[rn];
    uint32_t offset_addr = add ? base + offset : base - offset;
    uint32_t addr = pre ? offset_addr : base;
    bool wrote = false;
    auto writeback = [&] {
        if (!pre || wb) {
            cpu.regs[rn] = offset_addr;
            wrote = true;
        }
    };
    if (sh == 1 && load) { // LDRH
        uint32_t v = bus.read16(addr);
        writeback();
        cpu.set_reg(rd, v);
    } else if (sh == 1) { // STRH
        bus.write16(addr, uint16_t(cpu.regs[rd]));
    } else if (sh == 2 && load) { // LDRSB
        uint32_t v = uint32_t(int32_t(int8_t(bus.read8(addr))));
        writeback();
        cpu.set_reg(rd, v);
    } else if (sh == 2) { // LDRD
        uint32_t lo = bus.read32(addr), hi = bus.read32(addr + 4);
        writeback();
        cpu.set_reg(rd, lo);
        cpu.set_reg(rd + 1, hi);
    } else if (sh == 3 && load) { // LDRSH
        uint32_t v = uint32_t(int32_t(int16_t(bus.read16(addr))));
        writeback();
        cpu.set_reg(rd, v);
    } else if (sh == 3) { // STRD
        bus.write32(addr, cpu.regs[rd]);
        bus.write32(addr + 4, cpu.regs[rd + 1]);
    } else {
        return undefined(cpu, op);
    }
    if (!wrote && (!pre || wb)) cpu.regs[rn] = offset_addr;
    return NONE;
}

Exit load_store_block(Cpu &cpu, Memory &bus, uint32_t op) {
    unsigned rn = bits(op, 16, 19);
    bool pre = bit(op, 24), add = bit(op, 23), wb = bit(op, 21), load = bit(op, 20);
    uint32_t list = op & 0xFFFF;
    uint32_t base = cpu.regs[rn];
    bool empty = list == 0; // an empty list moves r15 alone and the base by 0x40
    if (empty) list = 1u << 15;
    uint32_t span = empty ? 0x40 : std::popcount(list) * 4u;
    uint32_t lowest = add ? (pre ? base + 4 : base) : (pre ? base - span : base - span + 4);
    uint32_t final_base = add ? base + span : base - span;
    uint32_t addr = lowest;
    if (load) {
        bool base_in_list = list & (1u << rn);
        if (wb && !base_in_list) cpu.regs[rn] = final_base;
        for (unsigned i = 0; i < 16; i++) {
            if (!(list & (1u << i))) continue;
            uint32_t v = bus.read32(addr);
            addr += 4;
            if (i == PC) cpu.branch_exchange(v);
            else cpu.regs[i] = v;
        }
    } else {
        for (unsigned i = 0; i < 16; i++) {
            if (!(list & (1u << i))) continue;
            uint32_t v;
            if (i == PC) v = cpu.regs[PC] + 4;
            // the base, not the lowest of the list, is stored written back
            else if (i == rn && wb && (list & ((1u << rn) - 1))) v = final_base;
            else v = cpu.regs[i];
            bus.write32(addr, v);
            addr += 4;
        }
        if (wb) cpu.regs[rn] = final_base;
    }
    return NONE;
}

Exit swap(Cpu &cpu, Memory &bus, uint32_t op) {
    unsigned rn = bits(op, 16, 19), rd = bits(op, 12, 15), rm = op & 0xF;
    uint32_t addr = cpu.regs[rn], source = cpu.regs[rm];
    if (bit(op, 22)) {
        uint32_t old = bus.read8(addr);
        bus.write8(addr, uint8_t(source));
        cpu.set_reg(rd, old);
    } else {
        uint32_t old = bus.read32(addr);
        bus.write32(addr, source);
        cpu.set_reg(rd, old);
    }
    return NONE;
}

Exit exclusive(Cpu &cpu, Memory &bus, uint32_t op) {
    bool load = bit(op, 20);
    unsigned size = bits(op, 21, 22), rn = bits(op, 16, 19), rd = bits(op, 12, 15);
    uint32_t addr = cpu.regs[rn];
    unsigned pair = load ? rd : op & 0xF;
    if (size == 1 && pair == PC) return undefined(cpu, op);
    if (load) {
        cpu.exclusive = addr;
        switch (size) {
        case 0: cpu.set_reg(rd, bus.read32(addr)); break;
        case 1: // LDREXD
            cpu.regs[rd] = bus.read32(addr);
            cpu.set_reg(rd + 1, bus.read32(addr + 4));
            break;
        case 2: cpu.set_reg(rd, bus.read8(addr)); break;
        default: cpu.set_reg(rd, bus.read16(addr));
        }
    } else {
        unsigned rm = op & 0xF;
        // one core: only a thread switch loses the reservation
        bool ok = cpu.exclusive == addr;
        if (ok) {
            switch (size) {
            case 0: bus.write32(addr, cpu.regs[rm]); break;
            case 1:
                bus.write32(addr, cpu.regs[rm]);
                bus.write32(addr + 4, cpu.regs[rm + 1]);
                break;
            case 2: bus.write8(addr, uint8_t(cpu.regs[rm])); break;
            default: bus.write16(addr, uint16_t(cpu.regs[rm]));
            }
            cpu.exclusive.reset();
        }
        cpu.set_reg(rd, !ok);
    }
    return NONE;
}

// -- media -------------------------------------------------------------------

struct Lane {
    uint32_t value;
    bool ge;
};

Lane lane(uint32_t a, uint32_t b, unsigned width, bool sub, bool is_signed, bool saturating, bool halving) {
    uint32_t mask = width == 16 ? 0xFFFF : 0xFF;
    int32_t wide;
    bool ge;
    if (is_signed) {
        int32_t x = width == 16 ? int16_t(a) : int8_t(a), y = width == 16 ? int16_t(b) : int8_t(b);
        wide = sub ? x - y : x + y;
        ge = wide >= 0;
    } else {
        int32_t x = int32_t(a & mask), y = int32_t(b & mask);
        wide = sub ? x - y : x + y;
        ge = sub ? wide >= 0 : wide > int32_t(mask);
    }
    uint32_t v;
    if (halving) v = uint32_t(wide >> 1);
    else if (saturating) v = is_signed ? signed_saturate(wide, width).value : unsigned_saturate(wide, width).value;
    else v = uint32_t(wide);
    return {v & mask, ge};
}

Exit parallel(Cpu &cpu, uint32_t op, unsigned op1, unsigned op2) {
    unsigned rd = bits(op, 12, 15);
    uint32_t a = cpu.regs[bits(op, 16, 19)], b = cpu.regs[op & 0xF];
    bool is_signed = !(op1 & 0b100);
    bool saturating, halving;
    switch (op1 & 3) {
    case 1: saturating = false, halving = false; break;
    case 2: saturating = true, halving = false; break;
    case 3: saturating = false, halving = true; break;
    default: return undefined(cpu, op);
    }
    uint8_t ge = 0;
    uint32_t result = 0;
    if (op2 <= 3) {
        bool cross = op2 == 1 || op2 == 2;
        uint32_t b_lo = cross ? b >> 16 : b & 0xFFFF, b_hi = cross ? b & 0xFFFF : b >> 16;
        bool sub_lo = op2 == 1 || op2 == 3, sub_hi = op2 == 2 || op2 == 3;
        Lane lo = lane(a & 0xFFFF, b_lo, 16, sub_lo, is_signed, saturating, halving);
        Lane hi = lane(a >> 16, b_hi, 16, sub_hi, is_signed, saturating, halving);
        if (lo.ge) ge |= 0b0011;
        if (hi.ge) ge |= 0b1100;
        result = lo.value | hi.value << 16;
    } else if (op2 == 4 || op2 == 7) {
        for (unsigned i = 0; i < 4; i++) {
            Lane l = lane(a >> (i * 8), b >> (i * 8), 8, op2 == 7, is_signed, saturating, halving);
            result |= l.value << (i * 8);
            if (l.ge) ge |= 1 << i;
        }
    } else {
        return undefined(cpu, op);
    }
    cpu.regs[rd] = result;
    if (!saturating && !halving) cpu.cpsr.ge = ge;
    return NONE;
}

Exit extend(Cpu &cpu, uint32_t op, unsigned op1) {
    unsigned rd = bits(op, 12, 15), rn = bits(op, 16, 19);
    uint32_t rotated = ror(cpu.regs[op & 0xF], bits(op, 10, 11) * 8);
    uint32_t value;
    bool packed = false;
    switch (op1 & 7) {
    case 0: // SXTB16
        value = (uint32_t(int32_t(int8_t(rotated))) & 0xFFFF) | (uint32_t(int32_t(int8_t(rotated >> 16))) & 0xFFFF) << 16;
        packed = true;
        break;
    case 2: value = uint32_t(int32_t(int8_t(rotated))); break;
    case 3: value = uint32_t(int32_t(int16_t(rotated))); break;
    case 4: value = rotated & 0x00FF00FF; packed = true; break;
    case 6: value = rotated & 0xFF; break;
    case 7: value = rotated & 0xFFFF; break;
    default: return undefined(cpu, op);
    }
    uint32_t acc = cpu.regs[rn];
    if (rn == 15) {
        cpu.regs[rd] = value;
    } else if (packed) {
        cpu.regs[rd] = uint16_t(acc + value) | uint32_t(uint16_t((acc >> 16) + (value >> 16))) << 16;
    } else {
        cpu.regs[rd] = acc + value;
    }
    return NONE;
}

Exit saturate32(Cpu &cpu, uint32_t op, bool is_unsigned) {
    unsigned rd = bits(op, 12, 15);
    uint32_t b = cpu.regs[op & 0xF];
    unsigned n = bits(op, 16, 20) + (is_unsigned ? 0 : 1), amount = bits(op, 7, 11);
    int64_t shifted;
    if (bit(op, 6)) {
        unsigned a = amount == 0 ? 32 : amount;
        shifted = a >= 32 ? int32_t(b) >> 31 : int32_t(b) >> a;
    } else {
        shifted = int32_t(b << amount);
    }
    Sat s = is_unsigned ? unsigned_saturate(shifted, n) : signed_saturate(shifted, n);
    cpu.regs[rd] = s.value;
    cpu.cpsr.q |= s.saturated;
    return NONE;
}

Exit saturate16(Cpu &cpu, uint32_t op, bool is_unsigned) {
    unsigned rd = bits(op, 12, 15);
    uint32_t b = cpu.regs[op & 0xF];
    unsigned n = bits(op, 16, 19) + (is_unsigned ? 0 : 1);
    uint32_t out = 0;
    bool saturated = false;
    for (unsigned i = 0; i < 2; i++) {
        int64_t l = int16_t(b >> (i * 16));
        Sat s = is_unsigned ? unsigned_saturate(l, n) : signed_saturate(l, n);
        out |= (s.value & 0xFFFF) << (i * 16);
        saturated |= s.saturated;
    }
    cpu.regs[rd] = out;
    cpu.cpsr.q |= saturated;
    return NONE;
}

Exit pack_sat_extend(Cpu &cpu, uint32_t op, unsigned op1, unsigned op2) {
    unsigned low = op1 & 7, rd = bits(op, 12, 15);
    uint32_t rm = cpu.regs[op & 0xF];
    if (op1 == 0b01000 && low == 0 && (op2 & 1) == 0) { // PKHBT / PKHTB
        uint32_t a = cpu.regs[bits(op, 16, 19)];
        unsigned shift = bits(op, 7, 11);
        if (bit(op, 6)) {
            unsigned amount = shift == 0 ? 32 : shift;
            uint32_t lo = amount >= 32 ? uint32_t(int32_t(rm) >> 31) : uint32_t(int32_t(rm) >> amount);
            cpu.regs[rd] = (lo & 0xFFFF) | (a & 0xFFFF0000);
        } else {
            cpu.regs[rd] = (a & 0xFFFF) | ((rm << shift) & 0xFFFF0000);
        }
        return NONE;
    }
    if (op2 == 0b011) return extend(cpu, op, op1);
    if (op1 == 0b01000 && op2 == 0b101) { // SEL
        uint32_t a = cpu.regs[bits(op, 16, 19)], out = 0;
        for (unsigned i = 0; i < 4; i++) out |= ((cpu.cpsr.ge >> i) & 1 ? a : rm) & (0xFFu << (i * 8));
        cpu.regs[rd] = out;
        return NONE;
    }
    if (op1 == 0b01011 && op2 == 0b001) { cpu.regs[rd] = __builtin_bswap32(rm); return NONE; }
    if (op1 == 0b01011 && op2 == 0b101) { cpu.regs[rd] = ((rm & 0x00FF00FF) << 8) | ((rm & 0xFF00FF00) >> 8); return NONE; }
    if (op1 == 0b01111 && op2 == 0b001) { // RBIT
        uint32_t v = rm;
        v = ((v >> 1) & 0x55555555u) | ((v & 0x55555555u) << 1);
        v = ((v >> 2) & 0x33333333u) | ((v & 0x33333333u) << 2);
        v = ((v >> 4) & 0x0F0F0F0Fu) | ((v & 0x0F0F0F0Fu) << 4);
        cpu.regs[rd] = __builtin_bswap32(v);
        return NONE;
    }
    if (op1 == 0b01111 && op2 == 0b101) { // REVSH
        cpu.regs[rd] = uint32_t(int32_t(int16_t(((rm & 0xFF) << 8) | ((rm >> 8) & 0xFF))));
        return NONE;
    }
    if ((low == 0b010 || low == 0b110) && op2 == 0b001) return saturate16(cpu, op, low & 0b100);
    unsigned group = (op1 >> 2) & 7;
    if ((op2 & 1) == 0 && (group == 0b010 || group == 0b011)) return saturate32(cpu, op, op1 & 0b00100);
    return undefined(cpu, op);
}

Exit dual_multiply(Cpu &cpu, uint32_t op, unsigned op1) {
    unsigned rd = bits(op, 16, 19), ra = bits(op, 12, 15), rs = bits(op, 8, 11), rm = op & 0xF;
    uint32_t a = cpu.regs[rm];
    uint32_t b = bit(op, 5) ? ror(cpu.regs[rs], 16) : cpu.regs[rs];
    int64_t a_lo = int16_t(a), a_hi = int16_t(a >> 16), b_lo = int16_t(b), b_hi = int16_t(b >> 16);
    bool subtract = bit(op, 6);
    int64_t dual = subtract ? a_lo * b_lo - a_hi * b_hi : a_lo * b_lo + a_hi * b_hi;
    switch (op1) {
    case 0b10000: { // SMLAD / SMUAD / SMLSD / SMUSD
        int64_t sum = ra == 15 ? dual : dual + int32_t(cpu.regs[ra]);
        cpu.regs[rd] = uint32_t(sum);
        cpu.cpsr.q |= sum != int32_t(sum);
        break;
    }
    case 0b10100: { // SMLALD / SMLSLD
        uint64_t sum = (uint64_t(cpu.regs[rd]) << 32 | cpu.regs[ra]) + uint64_t(dual);
        cpu.regs[ra] = uint32_t(sum);
        cpu.regs[rd] = uint32_t(sum >> 32);
        break;
    }
    case 0b10101: { // SMMUL / SMMLA / SMMLS
        int64_t product = int64_t(int32_t(a)) * int32_t(cpu.regs[rs]);
        int64_t acc = ra == 15 ? 0 : int64_t(int32_t(cpu.regs[ra])) * (int64_t(1) << 32);
        int64_t total = subtract ? acc - product : acc + product;
        if (bit(op, 5)) total += 0x80000000ll;
        cpu.regs[rd] = uint32_t(total >> 32);
        break;
    }
    default: return undefined(cpu, op);
    }
    return NONE;
}

Exit media(Cpu &cpu, uint32_t op) {
    unsigned op1 = bits(op, 20, 24), op2 = bits(op, 5, 7);
    switch (op1 >> 3) {
    case 0b00: return parallel(cpu, op, op1, op2);
    case 0b01: return pack_sat_extend(cpu, op, op1, op2);
    case 0b10: return dual_multiply(cpu, op, op1);
    default:
        if (op1 == 0b11000 && op2 == 0) { // USAD8 / USADA8
            unsigned rd = bits(op, 16, 19), ra = bits(op, 12, 15);
            uint32_t a = cpu.regs[op & 0xF], b = cpu.regs[bits(op, 8, 11)], sum = 0;
            for (unsigned i = 0; i < 4; i++) {
                int32_t x = (a >> (i * 8)) & 0xFF, y = (b >> (i * 8)) & 0xFF;
                sum += uint32_t(x > y ? x - y : y - x);
            }
            if (ra != 15) sum += cpu.regs[ra];
            cpu.regs[rd] = sum;
            return NONE;
        }
        return undefined(cpu, op);
    }
}

// -- VFP -----------------------------------------------------------------------

constexpr uint32_t FPSCR_FLAGS = 0xF0000000;

struct Vfp {
    Cpu &cpu;
    float f32(unsigned i) const { return std::bit_cast<float>(cpu.vfp[i & 31]); }
    double f64(unsigned i) const {
        return std::bit_cast<double>(uint64_t(cpu.vfp[(i * 2) & 31]) | uint64_t(cpu.vfp[(i * 2 + 1) & 31]) << 32);
    }
    void set_f32(unsigned i, float v) {
        if ((*cpu.fpscr & FPSCR_FZ) && v != 0 && std::fpclassify(v) == FP_SUBNORMAL) v = std::signbit(v) ? -0.0f : 0.0f;
        cpu.vfp[i & 31] = std::bit_cast<uint32_t>(v);
    }
    void set_f64(unsigned i, double v) {
        if ((*cpu.fpscr & FPSCR_FZ) && v != 0 && std::fpclassify(v) == FP_SUBNORMAL) v = std::signbit(v) ? -0.0 : 0.0;
        uint64_t b = std::bit_cast<uint64_t>(v);
        cpu.vfp[(i * 2) & 31] = uint32_t(b);
        cpu.vfp[(i * 2 + 1) & 31] = uint32_t(b >> 32);
    }
};

unsigned sreg(unsigned v, bool extra) { return (v << 1) | extra; }
unsigned dreg(unsigned v, bool extra) { return (unsigned(extra) << 4) | v; }

// VFPv2's short vectors: the SDK does its matrix math with them
struct Vector {
    unsigned iterations, stride, bank;
    Vector(uint32_t fpscr, bool is_double, unsigned d) {
        bank = is_double ? 4 : 8;
        iterations = d < bank ? 1 : bits(fpscr, 16, 18) + 1;
        stride = bits(fpscr, 20, 21) == 3 ? 2 : 1;
    }
    unsigned step(unsigned r, unsigned i) const { return (r & ~(bank - 1)) | ((r + i * stride) & (bank - 1)); }
};

template <typename T> T arithmetic(int operation, T a, T b, T acc) {
    switch (operation) {
    case 0: return acc + a * b;    // VMLA
    case 1: return acc - a * b;    // VMLS
    case 2: return -acc - a * b;   // VNMLA
    case 3: return -acc + a * b;   // VNMLS
    case 4: return a * b;          // VMUL
    case 5: return -(a * b);       // VNMUL
    case 6: return a + b;
    case 7: return a - b;
    default: return a / b;
    }
}

double round_to_mode(double v, unsigned mode) {
    switch (mode) {
    case 0: return std::nearbyint(v); // nearest, ties to even (the default rounding)
    case 1: return std::ceil(v);
    case 2: return std::floor(v);
    default: return std::trunc(v);
    }
}

uint32_t to_i32(double v) {
    if (std::isnan(v)) return 0;
    if (v >= 2147483647.0) return 0x7FFFFFFF;
    if (v <= -2147483648.0) return 0x80000000;
    return uint32_t(int32_t(v));
}

uint32_t to_u32(double v) {
    if (std::isnan(v)) return 0;
    if (v >= 4294967295.0) return 0xFFFFFFFF;
    if (v <= 0) return 0;
    return uint32_t(v);
}

void monadic(Cpu &cpu, bool is_double, bool second, bool top, unsigned d, unsigned m) {
    Vfp f{cpu};
    if (is_double) {
        // the moves and sign operations copy bits and flush nothing: titles
        // keep two singles in a double register
        uint64_t raw = uint64_t(cpu.vfp[(m * 2) & 31]) | uint64_t(cpu.vfp[(m * 2 + 1) & 31]) << 32;
        uint64_t b;
        if (!second && !top) b = raw;
        else if (!second) b = raw & ~(1ull << 63);
        else if (!top) b = raw ^ (1ull << 63);
        else {
            f.set_f64(d, std::sqrt(f.f64(m)));
            return;
        }
        cpu.vfp[(d * 2) & 31] = uint32_t(b);
        cpu.vfp[(d * 2 + 1) & 31] = uint32_t(b >> 32);
    } else {
        uint32_t raw = cpu.vfp[m & 31];
        if (!second && !top) cpu.vfp[d & 31] = raw;
        else if (!second) cpu.vfp[d & 31] = raw & 0x7FFFFFFF;
        else if (!top) cpu.vfp[d & 31] = raw ^ 0x80000000;
        else f.set_f32(d, std::sqrt(std::bit_cast<float>(raw)));
    }
}

void compare(Cpu &cpu, bool is_double, unsigned d, std::optional<unsigned> m) {
    Vfp f{cpu};
    double a = is_double ? f.f64(d) : f.f32(d);
    double b = m ? (is_double ? f.f64(*m) : f.f32(*m)) : 0.0;
    uint32_t flags = (std::isnan(a) || std::isnan(b)) ? 0b0011 : a == b ? 0b0110 : a < b ? 0b1000 : 0b0010;
    *cpu.fpscr = (*cpu.fpscr & ~FPSCR_FLAGS) | flags << 28;
}

Exit vfp_extension(Cpu &cpu, uint32_t op, bool is_double, unsigned d, unsigned m) {
    Vfp f{cpu};
    unsigned opc2 = bits(op, 16, 19);
    bool top = bit(op, 7);
    switch (opc2) {
    case 0b0000:
    case 0b0001: { // VMOV, VABS, VNEG, VSQRT, in short vectors too
        Vector v(*cpu.fpscr, is_double, d);
        bool scalar_m = m < v.bank;
        for (unsigned i = 0; i < v.iterations; i++) monadic(cpu, is_double, opc2 == 1, top, v.step(d, i), scalar_m ? m : v.step(m, i));
        return NONE;
    }
    case 0b0100: compare(cpu, is_double, d, m); return NONE;
    case 0b0101: compare(cpu, is_double, d, std::nullopt); return NONE;
    case 0b0111:
        if (!top) break;
        if (is_double) f.set_f32(sreg(bits(op, 12, 15), bit(op, 22)), float(f.f64(m)));
        else f.set_f64(dreg(bits(op, 12, 15), bit(op, 22)), double(f.f32(sreg(op & 0xF, bit(op, 5)))));
        return NONE;
    case 0b1000: { // from an integer, signed when bit 7 is set
        uint32_t raw = cpu.vfp[sreg(op & 0xF, bit(op, 5)) & 31];
        double v = top ? double(int32_t(raw)) : double(raw);
        if (is_double) f.set_f64(d, v);
        else f.set_f32(d, float(v));
        return NONE;
    }
    case 0b1100:
    case 0b1101: { // to an integer
        double source = is_double ? f.f64(m) : double(f.f32(m));
        double rounded = top ? std::trunc(source) : round_to_mode(source, bits(*cpu.fpscr, 22, 23));
        cpu.vfp[sreg(bits(op, 12, 15), bit(op, 22)) & 31] = opc2 == 0b1101 ? to_i32(rounded) : to_u32(rounded);
        return NONE;
    }
    default: break;
    }
    return undefined(cpu, op);
}

Exit vfp_data_processing(Cpu &cpu, uint32_t op) {
    unsigned cp = bits(op, 8, 11);
    if (cp != 10 && cp != 11) return undefined(cpu, op);
    bool is_double = cp == 11;
    bool d_bit = bit(op, 22), n_bit = bit(op, 7), m_bit = bit(op, 5);
    unsigned vd = bits(op, 12, 15), vn = bits(op, 16, 19), vm = op & 0xF;
    unsigned d = is_double ? dreg(vd, d_bit) : sreg(vd, d_bit);
    unsigned n = is_double ? dreg(vn, n_bit) : sreg(vn, n_bit);
    unsigned m = is_double ? dreg(vm, m_bit) : sreg(vm, m_bit);
    unsigned opc1 = bits(op, 20, 23) & 0b1011;
    bool negate = bit(op, 6);
    int operation;
    switch (opc1) {
    case 0b0000: operation = negate ? 1 : 0; break;
    case 0b0001: operation = negate ? 2 : 3; break;
    case 0b0010: operation = negate ? 5 : 4; break;
    case 0b0011: operation = negate ? 7 : 6; break;
    case 0b1000:
        if (negate) return undefined(cpu, op);
        operation = 8;
        break;
    case 0b1011:
        if (negate) return vfp_extension(cpu, op, is_double, d, m);
        return undefined(cpu, op);
    default: return undefined(cpu, op);
    }
    Vfp f{cpu};
    Vector v(*cpu.fpscr, is_double, d);
    bool scalar_m = m < v.bank;
    for (unsigned i = 0; i < v.iterations; i++) {
        unsigned di = v.step(d, i), ni = v.step(n, i), mi = scalar_m ? m : v.step(m, i);
        // singles in float, so each result is rounded once, as the hardware does
        if (is_double) f.set_f64(di, arithmetic(operation, f.f64(ni), f.f64(mi), f.f64(di)));
        else f.set_f32(di, arithmetic(operation, f.f32(ni), f.f32(mi), f.f32(di)));
    }
    return NONE;
}

Exit vfp_register(Cpu &cpu, uint32_t op) {
    bool load = bit(op, 20);
    unsigned rt = bits(op, 12, 15), opc1 = bits(op, 21, 23);
    if (opc1 == 7) { // VMRS / VMSR
        unsigned reg = bits(op, 16, 19);
        if (load) {
            uint32_t v = reg == 0 ? 0x41000000 : reg == 1 ? *cpu.fpscr : reg == 8 ? cpu.fpexc : 0;
            if (rt == 15) { // VMRS APSR_nzcv
                cpu.cpsr.n = bit(v, 31);
                cpu.cpsr.z = bit(v, 30);
                cpu.cpsr.c = bit(v, 29);
                cpu.cpsr.v = bit(v, 28);
            } else {
                cpu.regs[rt] = v;
            }
        } else if (reg == 1) {
            *cpu.fpscr = cpu.regs[rt];
        } else if (reg == 8) {
            cpu.fpexc = cpu.regs[rt];
        }
        return NONE;
    }
    if (opc1 == 0) { // VMOV between a core register and a single
        unsigned sn = sreg(bits(op, 16, 19), bit(op, 7)) & 31;
        if (load) cpu.set_reg(rt, cpu.vfp[sn]);
        else cpu.vfp[sn] = cpu.regs[rt];
    }
    return NONE;
}

Exit vfp_load_store(Cpu &cpu, Memory &bus, uint32_t op) {
    unsigned cp = bits(op, 8, 11);
    if (cp != 10 && cp != 11) return NONE;
    bool is_double = cp == 11;
    if (bits(op, 21, 27) == 0b1100010) { // VMOV with two core registers
        bool load = bit(op, 20);
        unsigned rt = bits(op, 12, 15), rt2 = bits(op, 16, 19);
        unsigned first = is_double ? dreg(op & 0xF, bit(op, 5)) * 2 : sreg(op & 0xF, bit(op, 5));
        if (load) {
            cpu.regs[rt] = cpu.vfp[first & 31];
            cpu.regs[rt2] = cpu.vfp[(first + 1) & 31];
        } else {
            cpu.vfp[first & 31] = cpu.regs[rt];
            cpu.vfp[(first + 1) & 31] = cpu.regs[rt2];
        }
        return NONE;
    }
    bool load = bit(op, 20), pre = bit(op, 24), add = bit(op, 23), wb = bit(op, 21);
    unsigned rn = bits(op, 16, 19);
    uint32_t offset = (op & 0xFF) * 4;
    unsigned vd = bits(op, 12, 15);
    unsigned first = is_double ? dreg(vd, bit(op, 22)) : sreg(vd, bit(op, 22));
    // VLDR/VSTR have P set and W clear; the multiple forms one of the two
    unsigned count = pre && !wb ? 1 : is_double ? (op & 0xFF) / 2 : op & 0xFF;
    uint32_t base = cpu.regs[rn];
    uint32_t addr = add ? (pre && !wb ? base + offset : base) : base - offset;
    for (unsigned i = 0; i < std::max(count, 1u); i++) {
        if (is_double) {
            unsigned index = first + i;
            if (load) {
                cpu.vfp[(index * 2) & 31] = bus.read32(addr);
                cpu.vfp[(index * 2 + 1) & 31] = bus.read32(addr + 4);
            } else {
                bus.write32(addr, cpu.vfp[(index * 2) & 31]);
                bus.write32(addr + 4, cpu.vfp[(index * 2 + 1) & 31]);
            }
            addr += 8;
        } else {
            unsigned index = (first + i) & 31;
            if (load) cpu.vfp[index] = bus.read32(addr);
            else bus.write32(addr, cpu.vfp[index]);
            addr += 4;
        }
    }
    if (wb) cpu.regs[rn] = add ? base + offset : base - offset;
    return NONE;
}

// -- branches, status registers, coprocessors ---------------------------------

Exit coprocessor_register(Cpu &cpu, uint32_t op) {
    unsigned cp = bits(op, 8, 11);
    if (cp == 10 || cp == 11) return vfp_register(cpu, op);
    if (cp != 15) return NONE;
    unsigned opc1 = bits(op, 21, 23), crn = bits(op, 16, 19), rd = bits(op, 12, 15), crm = op & 0xF, opc2 = bits(op, 5, 7);
    if (bit(op, 20)) { // MRC
        uint32_t v = 0;
        if (opc1 == 0 && crn == 0 && crm == 0 && opc2 == 0) v = 0x410FB024; // main ID
        else if (opc1 == 0 && crn == 0 && crm == 0 && opc2 == 1) v = 0x0F0D2112;
        else if (opc1 == 0 && crn == 1) v = 0x00052078;
        else if (opc1 == 0 && crn == 13 && crm == 0 && opc2 == 2) v = cpu.tid_rw;
        else if (opc1 == 0 && crn == 13 && crm == 0 && opc2 == 3) v = cpu.tls;
        if (rd == 15) {
            cpu.cpsr.n = bit(v, 31);
            cpu.cpsr.z = bit(v, 30);
            cpu.cpsr.c = bit(v, 29);
            cpu.cpsr.v = bit(v, 28);
        } else {
            cpu.regs[rd] = v;
        }
    } else if (opc1 == 0 && crn == 13 && crm == 0 && opc2 == 2) {
        cpu.tid_rw = cpu.regs[rd];
    }
    // the cache maintenance and the rest: nothing to do
    return NONE;
}

Exit arm_misc(Cpu &cpu, uint32_t op) {
    switch (bits(op, 4, 7)) {
    case 0b0000:
        if (bit(op, 21)) { // MSR: only the flags in user mode
            uint32_t v = cpu.regs[op & 0xF];
            if (bits(op, 16, 19) & 0b1000 && !bit(op, 22)) {
                cpu.cpsr.n = bit(v, 31);
                cpu.cpsr.z = bit(v, 30);
                cpu.cpsr.c = bit(v, 29);
                cpu.cpsr.v = bit(v, 28);
                cpu.cpsr.q = bit(v, 27);
            }
        } else { // MRS
            cpu.regs[bits(op, 12, 15)] = cpu.cpsr.bits();
        }
        return NONE;
    case 0b0001:
        if (bits(op, 21, 22) == 0b11) { // CLZ
            cpu.regs[bits(op, 12, 15)] = std::countl_zero(cpu.regs[op & 0xF]);
            return NONE;
        }
        [[fallthrough]];
    case 0b0011: { // BX / BLX
        uint32_t target = cpu.regs[op & 0xF];
        if (bits(op, 4, 7) == 0b0011) cpu.regs[LR] = cpu.regs[PC] - 4;
        cpu.branch_exchange(target);
        return NONE;
    }
    case 0b0101: return multiply_saturating(cpu, op);
    case 0b0111: return Exit{Exit::Breakpoint, op};
    default:
        if ((bits(op, 4, 7) & 0b1001) == 0b1000) return multiply_halfword(cpu, op);
        return undefined(cpu, op);
    }
}

Exit arm(Cpu &cpu, Memory &bus, uint32_t op) {
    uint32_t cond = op >> 28;
    if (cond == 0xF) { // unconditional
        if ((op & 0xFE000000) == 0xFA000000) { // BLX label, always to Thumb
            int32_t offset = (sign_extend(op & 0x00FFFFFF, 24) * 4) | (int32_t(bit(op, 24)) << 1);
            cpu.regs[LR] = cpu.regs[PC] - 4;
            cpu.cpsr.thumb = true;
            cpu.branch(cpu.regs[PC] + uint32_t(offset));
            return NONE;
        }
        if ((op & 0xFFF000F0) == 0xF5700010) { // CLREX
            cpu.exclusive.reset();
            return NONE;
        }
        switch (bits(op, 25, 27)) {
        case 0b000: case 0b010: case 0b011: return NONE; // CPS, SETEND, PLD
        case 0b110: return vfp_load_store(cpu, bus, op);
        case 0b111: return bit(op, 4) ? coprocessor_register(cpu, op) : vfp_data_processing(cpu, op);
        default: return undefined(cpu, op);
        }
    }
    if (!check_condition(cond, cpu.cpsr)) return NONE;
    switch (bits(op, 25, 27)) {
    case 0b000:
        if ((op & 0x90) == 0x90) { // multiplies, synchronization, extra loads
            if (bits(op, 5, 6) == 0) {
                if (bit(op, 24)) return bit(op, 23) ? exclusive(cpu, bus, op) : swap(cpu, bus, op);
                switch (bits(op, 21, 23)) {
                case 0b000: case 0b001: return multiply_short(cpu, op);
                case 0b011: return undefined(cpu, op); // MLS is ARMv6T2
                default: return multiply_long(cpu, op);
                }
            }
            return load_store_extra(cpu, bus, op);
        }
        if (bits(op, 23, 24) == 0b10 && !bit(op, 20)) return arm_misc(cpu, op);
        return dataproc(cpu, op);
    case 0b001:
        if (bits(op, 23, 24) == 0b10 && !bit(op, 20)) {
            if (bit(op, 21) && !bit(op, 22) && (bits(op, 16, 19) & 0b1000)) { // MSR immediate, flags
                uint32_t v = ror(op & 0xFF, bits(op, 8, 11) * 2);
                cpu.cpsr.n = bit(v, 31);
                cpu.cpsr.z = bit(v, 30);
                cpu.cpsr.c = bit(v, 29);
                cpu.cpsr.v = bit(v, 28);
                cpu.cpsr.q = bit(v, 27);
                return NONE;
            }
            return bit(op, 21) ? NONE : undefined(cpu, op);
        }
        return dataproc(cpu, op);
    case 0b010: return load_store_single(cpu, bus, op);
    case 0b011: return bit(op, 4) ? media(cpu, op) : load_store_single(cpu, bus, op);
    case 0b100: return load_store_block(cpu, bus, op);
    case 0b101: { // B / BL
        uint32_t target = cpu.regs[PC] + uint32_t(sign_extend(op & 0x00FFFFFF, 24) * 4);
        if (bit(op, 24)) cpu.regs[LR] = cpu.regs[PC] - 4;
        cpu.branch(target);
        return NONE;
    }
    case 0b110: return vfp_load_store(cpu, bus, op);
    default:
        if (bit(op, 24)) { // SVC: resume after it
            cpu.branch(cpu.regs[PC] - 4);
            return Exit{Exit::Supervisor, op & 0x00FFFFFF};
        }
        return bit(op, 4) ? coprocessor_register(cpu, op) : vfp_data_processing(cpu, op);
    }
}

// -- Thumb ---------------------------------------------------------------------

void set_add_sub(Cpu &cpu, unsigned rd, Flags f, bool write = true) {
    if (write) cpu.regs[rd] = f.r;
    cpu.set_nz(f.r);
    cpu.cpsr.c = f.c;
    cpu.cpsr.v = f.v;
}

Exit thumb(Cpu &cpu, Memory &bus, uint32_t op) {
    switch (bits(op, 13, 15)) {
    case 0b000: {
        if (bits(op, 11, 12) == 0b11) { // add/subtract register or 3-bit immediate
            uint32_t operand = bit(op, 10) ? bits(op, 6, 8) : cpu.regs[bits(op, 6, 8)];
            uint32_t a = cpu.regs[bits(op, 3, 5)];
            set_add_sub(cpu, op & 7, bit(op, 9) ? sub_with_flags(a, operand) : add_with_flags(a, operand));
            return NONE;
        }
        Shifted s = shift_by_immediate(bits(op, 11, 12), cpu.regs[bits(op, 3, 5)], bits(op, 6, 10), cpu.cpsr.c);
        cpu.regs[op & 7] = s.value;
        cpu.set_nz(s.value);
        cpu.cpsr.c = s.carry;
        return NONE;
    }
    case 0b001: { // MOV/CMP/ADD/SUB with an 8-bit immediate
        unsigned rd = bits(op, 8, 10);
        uint32_t imm = op & 0xFF, a = cpu.regs[rd];
        switch (bits(op, 11, 12)) {
        case 0: cpu.regs[rd] = imm; cpu.set_nz(imm); break;
        case 1: set_add_sub(cpu, rd, sub_with_flags(a, imm), false); break;
        case 2: set_add_sub(cpu, rd, add_with_flags(a, imm)); break;
        default: set_add_sub(cpu, rd, sub_with_flags(a, imm)); break;
        }
        return NONE;
    }
    case 0b010: {
        if (bit(op, 12)) { // register-offset loads and stores
            uint32_t addr = cpu.regs[bits(op, 3, 5)] + cpu.regs[bits(op, 6, 8)];
            unsigned rd = op & 7;
            if (bit(op, 9)) {
                switch (bits(op, 10, 11)) {
                case 0: bus.write16(addr, uint16_t(cpu.regs[rd])); break;
                case 1: cpu.regs[rd] = uint32_t(int32_t(int8_t(bus.read8(addr)))); break;
                case 2: cpu.regs[rd] = bus.read16(addr); break;
                default: cpu.regs[rd] = uint32_t(int32_t(int16_t(bus.read16(addr))));
                }
            } else {
                switch (bits(op, 10, 11)) {
                case 0: bus.write32(addr, cpu.regs[rd]); break;
                case 1: bus.write8(addr, uint8_t(cpu.regs[rd])); break;
                case 2: cpu.regs[rd] = bus.read32(addr); break;
                default: cpu.regs[rd] = bus.read8(addr);
                }
            }
            return NONE;
        }
        if (bit(op, 11)) { // PC-relative load, from the word-aligned PC
            cpu.regs[bits(op, 8, 10)] = bus.read32((cpu.regs[PC] & ~3u) + (op & 0xFF) * 4);
            return NONE;
        }
        if (bit(op, 10)) { // high registers and BX/BLX
            unsigned rd = (op & 7) | ((op >> 4) & 8), rs = bits(op, 3, 6);
            uint32_t source = cpu.regs[rs];
            switch (bits(op, 8, 9)) {
            case 0: cpu.set_reg(rd, cpu.regs[rd] + source); break;
            case 1: set_add_sub(cpu, rd, sub_with_flags(cpu.regs[rd], source), false); break;
            case 2: cpu.set_reg(rd, source); break;
            default:
                if (bit(op, 7)) cpu.regs[LR] = (cpu.regs[PC] - 2) | 1;
                cpu.branch_exchange(source);
            }
            return NONE;
        }
        // the ALU operations
        unsigned rd = op & 7;
        uint32_t a = cpu.regs[rd], b = cpu.regs[bits(op, 3, 5)];
        bool cin = cpu.cpsr.c;
        auto logical = [&](uint32_t r) {
            cpu.regs[rd] = r;
            cpu.set_nz(r);
        };
        switch (bits(op, 6, 9)) {
        case 0x0: logical(a & b); break;
        case 0x1: logical(a ^ b); break;
        case 0x2: case 0x3: case 0x4: case 0x7: {
            static const unsigned types[] = {0, 0, LSL, LSR, ASR, 0, 0, ROR};
            Shifted s = shift_by_register(types[bits(op, 6, 9)], a, b, cin);
            logical(s.value);
            cpu.cpsr.c = s.carry;
            break;
        }
        case 0x5: set_add_sub(cpu, rd, adc_with_flags(a, b, cin)); break;
        case 0x6: set_add_sub(cpu, rd, sbc_with_flags(a, b, cin)); break;
        case 0x8: cpu.set_nz(a & b); break;                                  // TST
        case 0x9: set_add_sub(cpu, rd, sub_with_flags(0, b)); break;         // NEG
        case 0xA: set_add_sub(cpu, rd, sub_with_flags(a, b), false); break;  // CMP
        case 0xB: set_add_sub(cpu, rd, add_with_flags(a, b), false); break;  // CMN
        case 0xC: logical(a | b); break;
        case 0xD: logical(a * b); break;
        case 0xE: logical(a & ~b); break;
        default: logical(~b);
        }
        return NONE;
    }
    case 0b011: { // immediate-offset loads and stores; words scale by four
        bool byte = bit(op, 12), load = bit(op, 11);
        uint32_t offset = bits(op, 6, 10);
        uint32_t addr = cpu.regs[bits(op, 3, 5)] + (byte ? offset : offset * 4);
        unsigned rd = op & 7;
        if (load) cpu.regs[rd] = byte ? bus.read8(addr) : bus.read32(addr);
        else if (byte) bus.write8(addr, uint8_t(cpu.regs[rd]));
        else bus.write32(addr, cpu.regs[rd]);
        return NONE;
    }
    case 0b100: {
        if (bit(op, 12)) { // SP-relative
            uint32_t addr = cpu.regs[SP] + (op & 0xFF) * 4;
            unsigned rd = bits(op, 8, 10);
            if (bit(op, 11)) cpu.regs[rd] = bus.read32(addr);
            else bus.write32(addr, cpu.regs[rd]);
            return NONE;
        }
        uint32_t addr = cpu.regs[bits(op, 3, 5)] + bits(op, 6, 10) * 2;
        if (bit(op, 11)) cpu.regs[op & 7] = bus.read16(addr);
        else bus.write16(addr, uint16_t(cpu.regs[op & 7]));
        return NONE;
    }
    case 0b101: {
        if (!bit(op, 12)) { // ADD Rd, PC/SP, #imm
            uint32_t base = bit(op, 11) ? cpu.regs[SP] : cpu.regs[PC] & ~3u;
            cpu.regs[bits(op, 8, 10)] = base + (op & 0xFF) * 4;
            return NONE;
        }
        switch (bits(op, 8, 11)) {
        case 0b0000: {
            uint32_t offset = (op & 0x7F) * 4;
            cpu.regs[SP] = bit(op, 7) ? cpu.regs[SP] - offset : cpu.regs[SP] + offset;
            return NONE;
        }
        case 0b0010: {
            uint32_t v = cpu.regs[bits(op, 3, 5)];
            static const int kinds[] = {0, 1, 2, 3};
            switch (kinds[bits(op, 6, 7)]) {
            case 0: cpu.regs[op & 7] = uint32_t(int32_t(int16_t(v))); break;
            case 1: cpu.regs[op & 7] = uint32_t(int32_t(int8_t(v))); break;
            case 2: cpu.regs[op & 7] = v & 0xFFFF; break;
            default: cpu.regs[op & 7] = v & 0xFF;
            }
            return NONE;
        }
        case 0b1010: {
            uint32_t v = cpu.regs[bits(op, 3, 5)];
            switch (bits(op, 6, 7)) {
            case 0: cpu.regs[op & 7] = __builtin_bswap32(v); break;
            case 1: cpu.regs[op & 7] = ((v & 0x00FF00FF) << 8) | ((v & 0xFF00FF00) >> 8); break;
            case 3: cpu.regs[op & 7] = uint32_t(int32_t(int16_t(((v & 0xFF) << 8) | ((v >> 8) & 0xFF)))); break;
            default: return undefined(cpu, op);
            }
            return NONE;
        }
        case 0b0110: case 0b1111: return NONE; // CPS, SETEND, hints
        case 0b0100: case 0b0101: case 0b1100: case 0b1101: { // PUSH / POP
            bool load = bit(op, 11), extra = bit(op, 8);
            uint32_t list = op & 0xFF, count = std::popcount(list) + extra;
            if (load) {
                uint32_t addr = cpu.regs[SP];
                for (unsigned i = 0; i < 8; i++) {
                    if (list & (1u << i)) {
                        cpu.regs[i] = bus.read32(addr);
                        addr += 4;
                    }
                }
                cpu.regs[SP] += count * 4;
                if (extra) cpu.branch_exchange(bus.read32(addr)); // POP {pc} interworks
            } else {
                uint32_t start = cpu.regs[SP] - count * 4, addr = start;
                for (unsigned i = 0; i < 8; i++) {
                    if (list & (1u << i)) {
                        bus.write32(addr, cpu.regs[i]);
                        addr += 4;
                    }
                }
                if (extra) bus.write32(addr, cpu.regs[LR]);
                cpu.regs[SP] = start;
            }
            return NONE;
        }
        case 0b1110: return Exit{Exit::Breakpoint, op};
        default: return undefined(cpu, op);
        }
    }
    case 0b110: {
        if (bit(op, 12)) { // conditional branch, SVC
            unsigned cond = bits(op, 8, 11);
            if (cond == 0xF) {
                cpu.branch(cpu.regs[PC] - 2);
                return Exit{Exit::Supervisor, op & 0xFF};
            }
            if (cond == 0xE) return undefined(cpu, op);
            if (check_condition(cond, cpu.cpsr)) cpu.branch(cpu.regs[PC] + uint32_t(sign_extend(op & 0xFF, 8) * 2));
            return NONE;
        }
        // LDMIA / STMIA
        bool load = bit(op, 11);
        unsigned rb = bits(op, 8, 10);
        uint32_t list = op & 0xFF;
        if (list == 0) {
            cpu.regs[rb] += 0x40;
            return NONE;
        }
        uint32_t addr = cpu.regs[rb];
        bool writeback = !(load && (list & (1u << rb)));
        uint32_t final_base = addr + std::popcount(list) * 4u;
        for (unsigned i = 0; i < 8; i++) {
            if (!(list & (1u << i))) continue;
            if (load) cpu.regs[i] = bus.read32(addr);
            else bus.write32(addr, cpu.regs[i]);
            addr += 4;
        }
        if (writeback) cpu.regs[rb] = final_base;
        return NONE;
    }
    default: // unconditional and long branches
        switch (bits(op, 11, 12)) {
        case 0b00:
            cpu.branch(cpu.regs[PC] + uint32_t(sign_extend(op & 0x7FF, 11) * 2));
            return NONE;
        case 0b01:
        case 0b11: { // BL/BLX suffix, the low half of the offset
            uint32_t target = cpu.regs[LR] + ((op & 0x7FF) << 1);
            cpu.regs[LR] = (cpu.regs[PC] - 2) | 1;
            if (bits(op, 11, 12) == 0b01) { // BLX: to ARM, word-aligned
                cpu.cpsr.thumb = false;
                cpu.branch(target & ~3u);
            } else {
                cpu.branch(target);
            }
            return NONE;
        }
        default: // BL/BLX prefix: the high half into LR
            cpu.regs[LR] = cpu.regs[PC] + uint32_t(sign_extend(op & 0x7FF, 11) * 4096);
            return NONE;
        }
    }
}

} // namespace

uint32_t Psr::bits() const {
    return uint32_t(n) << 31 | uint32_t(z) << 30 | uint32_t(c) << 29 | uint32_t(v) << 28 | uint32_t(q) << 27 |
           uint32_t(ge & 0xF) << 16 | uint32_t(thumb) << 5 | 0x10; // user mode
}

Exit step(Cpu &cpu, Memory &bus) {
    uint32_t addr = cpu.regs[PC];
    Exit exit;
    if (cpu.cpsr.thumb) {
        uint32_t op = bus.read16(addr);
        cpu.regs[PC] = addr + 4;
        exit = thumb(cpu, bus, op);
        if (!cpu.branched) cpu.regs[PC] = addr + 2;
    } else {
        uint32_t op = bus.read32(addr);
        cpu.regs[PC] = addr + 8;
        exit = arm(cpu, bus, op);
        if (!cpu.branched) cpu.regs[PC] = addr + 4;
    }
    cpu.branched = false;
    if (exit.kind == Exit::Undefined || exit.kind == Exit::Breakpoint) cpu.regs[PC] = addr;
    return exit;
}

void load(const Context &c, Cpu &cpu) {
    std::memcpy(cpu.regs, c.r, sizeof cpu.regs);
    cpu.cpsr.n = c.n;
    cpu.cpsr.z = c.z;
    cpu.cpsr.c = c.c;
    cpu.cpsr.v = c.v;
    cpu.cpsr.q = c.q;
    cpu.cpsr.ge = c.ge;
    cpu.cpsr.thumb = c.thumb;
    cpu.vfp = c.vfp;
    cpu.fpscr = c.fpscr;
    cpu.tls = c.tls;
    cpu.exclusive = c.exclusive ? std::optional<uint32_t>(c.exclusive_address) : std::nullopt;
}

void store(const Cpu &cpu, Context &c) {
    std::memcpy(c.r, cpu.regs, sizeof c.r);
    c.n = cpu.cpsr.n;
    c.z = cpu.cpsr.z;
    c.c = cpu.cpsr.c;
    c.v = cpu.cpsr.v;
    c.q = cpu.cpsr.q;
    c.ge = cpu.cpsr.ge;
    c.thumb = cpu.cpsr.thumb;
    c.exclusive = cpu.exclusive.has_value();
    c.exclusive_address = cpu.exclusive.value_or(0);
}

} // namespace runtime
