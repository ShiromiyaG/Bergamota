// Lowering ARM instructions to C. Translated from 3dsrecomp's codegen/arm.rs
// (Rust, MIT, Copyright (c) 2026 fearkov; see LICENSE-3dsrecomp).
//
// the decoding follows zakuro-cpu's, down to which pipeline offset each form
// reads r15 with, so that the two agree on every instruction. anything
// uncommon is left to the interpreter.

#include <bit>

#include "scope.h"

namespace recomp::codegen::arm {

namespace {

uint32_t sign_extend_24(uint32_t value) { return uint32_t(int32_t(value << 8) >> 8); }

// reading a register, where r15 is the address the pipeline gives it.
std::string reg(const Scope &scope, uint32_t number, uint32_t pc) {
    if (number == 15) return scope.at(pc);
    return std::format("ctx->r[{}]", number);
}

bool unconditional(std::string &out, const Scope &scope, uint32_t a, uint32_t op);
bool body(std::string &out, const Scope &scope, uint32_t a, uint32_t op);

} // namespace

bool lower(std::string &out, const Scope &scope, uint32_t address, uint32_t op) {
    uint32_t condition = op >> 28;
    if (condition == 0xF) return unconditional(out, scope, address, op);
    if (condition == 0xE) return body(out, scope, address, op);
    emit(out, "    if ({}) {{", CONDITIONS[condition]);
    body(out, scope, address, op);
    emit(out, "    }}");
    return true;
}

namespace {

bool register_space(std::string &out, const Scope &scope, uint32_t a, uint32_t op);
bool status_register(std::string &out, const Scope &scope, uint32_t a, uint32_t op);
bool data_processing(std::string &out, const Scope &scope, uint32_t a, uint32_t op);
bool single_transfer(std::string &out, const Scope &scope, uint32_t a, uint32_t op);
bool media(std::string &out, const Scope &scope, uint32_t a, uint32_t op);
bool block_transfer(std::string &out, const Scope &scope, uint32_t a, uint32_t op);
bool interpret(std::string &out, const Scope &scope, uint32_t a, uint32_t op);

bool body(std::string &out, const Scope &scope, uint32_t a, uint32_t op) {
    switch ((op >> 25) & 7) {
    case 0b000: return register_space(out, scope, a, op);
    case 0b001:
        // msr with an immediate lives in the compare opcodes without s, and
        // with no fields to write it is nop and the other hints
        if (((op >> 23) & 3) == 0b10 && (op & (1u << 20)) == 0) {
            if ((op & (1u << 21)) != 0 && ((op >> 16) & 0xF) == 0) return true;
            return status_register(out, scope, a, op);
        }
        return data_processing(out, scope, a, op);
    case 0b010: return single_transfer(out, scope, a, op);
    case 0b011:
        if ((op & 0x10) != 0) return media(out, scope, a, op);
        return single_transfer(out, scope, a, op);
    case 0b100: return block_transfer(out, scope, a, op);
    case 0b101: {
        uint32_t target = (a + 8) + (sign_extend_24(op & 0x00FFFFFF) << 2);
        if ((op & (1u << 24)) != 0) return call(out, scope, a + 4, target, false);
        return jump(out, scope, target, false);
    }
    case 0b110: return vfp::load_store(out, scope, a, op);
    case 0b111:
        if ((op & (1u << 24)) != 0) {
            emit(out, "    SVC({}, 0x{:X}u);", scope.at(a + 4), op & 0x00FFFFFF);
            // interface 6: the thread waits in it, then goes on
            return true;
            return false;
        }
        if ((op & 0x10) != 0 && (((op >> 8) & 0xF) == 10 || ((op >> 8) & 0xF) == 11)) {
            return vfp::register_transfer(out, scope, a, op);
        }
        // cp15, where reading the TLS address is common enough to do here,
        // and so are the barriers and data cache maintenance, which change
        // nothing for code that sees memory as it is
        if ((op & 0x10) != 0) {
            uint32_t rd = (op >> 12) & 0xF;
            uint32_t low = op & 0xF;
            bool data_cache = (op & 0x0FFF0F10) == 0x0E070F10 && !(low == 5 || low == 7 || low == 13);
            if ((op & 0x0FFF0FFF) == 0x0E1D0F70 && rd != 15) {
                emit(out, "    ctx->r[{}] = ctx->tls;", rd);
                return true;
            }
            if (data_cache) return true;
            return interpret(out, scope, a, op);
        }
        return vfp::data_processing(out, scope, a, op);
    }
    return vfp::data_processing(out, scope, a, op);
}

bool extra_transfer(std::string &out, const Scope &scope, uint32_t a, uint32_t op);
bool exclusive(std::string &out, const Scope &scope, uint32_t a, uint32_t op);
bool swap(std::string &out, const Scope &scope, uint32_t a, uint32_t op);
bool multiply(std::string &out, const Scope &scope, uint32_t a, uint32_t op);
bool multiply_long(std::string &out, const Scope &scope, uint32_t a, uint32_t op);
bool clz(std::string &out, const Scope &scope, uint32_t a, uint32_t op);
bool branch_exchange(std::string &out, const Scope &scope, uint32_t a, uint32_t op);
bool saturating(std::string &out, const Scope &scope, uint32_t a, uint32_t op);
bool halfword_multiply(std::string &out, const Scope &scope, uint32_t a, uint32_t op);

// the register data processing space, with the multiplies, extra loads
// and stores and miscellaneous instructions in its holes.
bool register_space(std::string &out, const Scope &scope, uint32_t a, uint32_t op) {
    if ((op & 0x90) == 0x90) {
        if (((op >> 5) & 3) != 0) return extra_transfer(out, scope, a, op);
        if ((op & (1u << 24)) != 0) {
            return (op & (1u << 23)) != 0 ? exclusive(out, scope, a, op) : swap(out, scope, a, op);
        }
        switch ((op >> 21) & 7) {
        case 0b000:
        case 0b001: return multiply(out, scope, a, op);
        case 0b010:
        case 0b100:
        case 0b101:
        case 0b110:
        case 0b111: return multiply_long(out, scope, a, op);
        default: return interpret(out, scope, a, op);
        }
    }
    if (((op >> 23) & 3) == 0b10 && (op & (1u << 20)) == 0) {
        uint32_t kind = (op >> 4) & 0xF;
        if (kind == 0b0000) return status_register(out, scope, a, op);
        if (kind == 0b0001 && ((op >> 21) & 3) == 0b11) return clz(out, scope, a, op);
        if (kind == 0b0001 || kind == 0b0011) return branch_exchange(out, scope, a, op);
        if (kind == 0b0101) return saturating(out, scope, a, op);
        if ((kind & 0b1001) == 0b1000) return halfword_multiply(out, scope, a, op);
        return interpret(out, scope, a, op);
    }
    return data_processing(out, scope, a, op);
}

bool interpret(std::string &out, const Scope &scope, uint32_t a, uint32_t op) {
    emit(out, "    INTERPRET({}, 0x{:08X}u);", scope.at(a), op);
    scope.interpreted.push_back(a);
    return true;
}

// mrs and msr on the cpsr. titles run in system mode and only ever change
// the flags and ge this way, so reading gives those over that mode, and a
// write to anything else, or the spsr, is the interpreter's.
bool status_register(std::string &out, const Scope &scope, uint32_t a, uint32_t op) {
    if ((op & (1u << 22)) != 0) return interpret(out, scope, a, op);
    if ((op & (1u << 21)) == 0) {
        uint32_t rd = (op >> 12) & 0xF;
        if (rd == 15) return interpret(out, scope, a, op);
        emit(out,
             "    ctx->r[{}] = ((uint32_t)ctx->n << 31) | ((uint32_t)ctx->z << 30) | ((uint32_t)ctx->c << 29) "
             "| ((uint32_t)ctx->v << 28) | ((uint32_t)ctx->q << 27) | ((uint32_t)(ctx->ge & 0xF) << 16) | 0x1Fu;",
             rd);
        return true;
    }
    uint32_t mask = (op >> 16) & 0xF;
    if ((mask & 0b0011) != 0) return interpret(out, scope, a, op);
    std::string value = (op & (1u << 25)) != 0
                            ? std::format("0x{:08X}u", std::rotr(op & 0xFF, int(((op >> 8) & 0xF) * 2)))
                            : reg(scope, op & 0xF, a + 8);
    emit(out, "    {{");
    emit(out, "    uint32_t value = {};", value);
    if ((mask & 0b1000) != 0) {
        emit(out, "    ctx->n = value >> 31; ctx->z = (value >> 30) & 1; ctx->c = (value >> 29) & 1;");
        emit(out, "    ctx->v = (value >> 28) & 1; ctx->q = (value >> 27) & 1;");
    }
    if ((mask & 0b0100) != 0) emit(out, "    ctx->ge = (value >> 16) & 0xF;");
    emit(out, "    }}");
    return true;
}

// the shifter operand as b, and its carry as sc when the flags need it.
void shifter_operand(std::string &out, const Scope &scope, uint32_t a, uint32_t op, bool carry) {
    if ((op & (1u << 25)) != 0) {
        uint32_t rotate = ((op >> 8) & 0xF) * 2;
        uint32_t value = std::rotr(op & 0xFF, int(rotate));
        emit(out, "    uint32_t b = 0x{:08X}u;", value);
        if (carry) {
            if (rotate == 0) {
                emit(out, "    uint8_t sc = ctx->c;");
            } else {
                emit(out, "    uint8_t sc = {};", value >> 31);
            }
        }
        return;
    }
    uint32_t rm = op & 0xF;
    uint32_t kind = (op >> 5) & 3;
    if ((op & (1u << 4)) != 0) {
        uint32_t rs = (op >> 8) & 0xF;
        static constexpr const char *SHIFTS[4] = {"shift_lsl", "shift_lsr", "shift_asr", "shift_ror"};
        emit(out, "    uint8_t sc = ctx->c;");
        emit(out, "    uint32_t b = {}({}, {}, &sc);", SHIFTS[kind], reg(scope, rm, a + 12), reg(scope, rs, a + 12));
        return;
    }
    uint32_t n = (op >> 7) & 0x1F;
    emit(out, "    uint32_t m = {};", reg(scope, rm, a + 8));
    std::string value, carry_out;
    if (kind == 0 && n == 0) {
        value = "m", carry_out = "ctx->c";
    } else if (kind == 0) {
        value = std::format("m << {}", n), carry_out = std::format("(m >> {}) & 1", 32 - n);
    } else if (kind == 1 && n == 0) {
        value = "0", carry_out = "m >> 31";
    } else if (kind == 1) {
        value = std::format("m >> {}", n), carry_out = std::format("(m >> {}) & 1", n - 1);
    } else if (kind == 2 && n == 0) {
        value = "(uint32_t)((int32_t)m >> 31)", carry_out = "m >> 31";
    } else if (kind == 2) {
        value = std::format("(uint32_t)((int32_t)m >> {})", n), carry_out = std::format("(m >> {}) & 1", n - 1);
    } else if (n == 0) {
        // ror 0 is rrx
        value = "((uint32_t)ctx->c << 31) | (m >> 1)", carry_out = "m & 1";
    } else {
        value = std::format("(m >> {}) | (m << {})", n, 32 - n), carry_out = std::format("(m >> {}) & 1", n - 1);
    }
    emit(out, "    uint32_t b = {};", value);
    if (carry) emit(out, "    uint8_t sc = {};", carry_out);
}

bool data_processing(std::string &out, const Scope &scope, uint32_t a, uint32_t op) {
    uint32_t opcode = (op >> 21) & 0xF;
    bool set_flags = (op & (1u << 20)) != 0;
    uint32_t rn = (op >> 16) & 0xF;
    uint32_t rd = (op >> 12) & 0xF;
    bool compare = opcode >= 8 && opcode <= 11;
    if (set_flags && rd == 15 && !compare) {
        // an exception return, which also restores cpsr
        return interpret(out, scope, a, op);
    }
    bool register_shift = (op & (1u << 25)) == 0 && (op & (1u << 4)) != 0;
    bool logical = opcode == 0 || opcode == 1 || opcode == 8 || opcode == 9 || opcode >= 12;

    emit(out, "    {{");
    shifter_operand(out, scope, a, op, set_flags && logical);
    if (opcode != 13 && opcode != 15) emit(out, "    uint32_t a = {};", reg(scope, rn, register_shift ? a + 12 : a + 8));
    if (opcode >= 5 && opcode <= 7) emit(out, "    uint8_t ci = ctx->c;");
    const char *result;
    switch (opcode) {
    case 0:
    case 8: result = "a & b"; break;
    case 1:
    case 9: result = "a ^ b"; break;
    case 2:
    case 10: result = "a - b"; break;
    case 3: result = "b - a"; break;
    case 4:
    case 11: result = "a + b"; break;
    case 5: result = "a + b + ci"; break;
    case 6: result = "a - b - !ci"; break;
    case 7: result = "b - a - !ci"; break;
    case 12: result = "a | b"; break;
    case 13: result = "b"; break;
    case 14: result = "a & ~b"; break;
    default: result = "~b"; break;
    }
    emit(out, "    uint32_t r = {};", result);
    if (set_flags) {
        emit(out, "    ctx->n = r >> 31; ctx->z = r == 0;");
        switch (opcode) {
        case 2:
        case 10: emit(out, "    ctx->c = a >= b; ctx->v = ((a ^ b) & (a ^ r)) >> 31;"); break;
        case 3: emit(out, "    ctx->c = b >= a; ctx->v = ((b ^ a) & (b ^ r)) >> 31;"); break;
        case 4:
        case 11: emit(out, "    ctx->c = r < a; ctx->v = ((a ^ r) & (b ^ r)) >> 31;"); break;
        case 5: emit(out, "    ctx->c = ((uint64_t)a + b + ci) >> 32; ctx->v = ((a ^ r) & (b ^ r)) >> 31;"); break;
        case 6:
            emit(out, "    ctx->c = (uint64_t)a >= (uint64_t)b + !ci; ctx->v = ((a ^ b) & (a ^ r)) >> 31;");
            break;
        case 7:
            emit(out, "    ctx->c = (uint64_t)b >= (uint64_t)a + !ci; ctx->v = ((b ^ a) & (b ^ r)) >> 31;");
            break;
        default: emit(out, "    ctx->c = sc;"); break;
        }
    }
    bool continues;
    if (compare) {
        continues = true;
    } else if (rd == 15) {
        // mov pc, lr returns, any other write to pc jumps
        if (opcode == 13 && (op & 0x02000FFF) == 14) {
            emit(out, "    ctx->r[15] = r & ~3u; RETURN();");
        } else {
            emit(out, "    target = r & ~3u; goto dispatch;");
        }
        continues = false;
    } else {
        emit(out, "    ctx->r[{}] = r;", rd);
        continues = true;
    }
    emit(out, "    }}");
    return continues;
}

// ldr, str, ldrb and strb.
bool single_transfer(std::string &out, const Scope &scope, uint32_t a, uint32_t op) {
    uint32_t rn = (op >> 16) & 0xF;
    uint32_t rd = (op >> 12) & 0xF;
    bool pre = (op & (1u << 24)) != 0;
    bool byte = (op & (1u << 22)) != 0;
    bool load = (op & (1u << 20)) != 0;
    bool writeback = !pre || (op & (1u << 21)) != 0;
    if (writeback && rn == 15) return interpret(out, scope, a, op);
    std::string offset;
    if ((op & (1u << 25)) != 0) {
        std::string m = reg(scope, op & 0xF, a + 8);
        uint32_t kind = (op >> 5) & 3, n = (op >> 7) & 0x1F;
        if (kind == 0) {
            offset = std::format("({} << {})", m, n);
        } else if (kind == 1 && n == 0) {
            offset = "0";
        } else if (kind == 1) {
            offset = std::format("({} >> {})", m, n);
        } else if (kind == 2 && n == 0) {
            offset = std::format("(uint32_t)((int32_t){} >> 31)", m);
        } else if (kind == 2) {
            offset = std::format("(uint32_t)((int32_t){} >> {})", m, n);
        } else if (n == 0) {
            offset = std::format("(((uint32_t)ctx->c << 31) | ({} >> 1))", m);
        } else {
            offset = std::format("ror32({}, {})", m, n);
        }
    } else {
        offset = std::format("0x{:X}u", op & 0xFFF);
    }
    char sign = (op & (1u << 23)) != 0 ? '+' : '-';
    const char *address = pre ? "oa" : "base";
    emit(out, "    {{ uint32_t base = {}, oa = base {} {};", reg(scope, rn, a + 8), sign, offset);
    if (load) {
        const char *read = byte ? "mem_read8" : "mem_read32";
        emit(out, "    uint32_t v = {}(ctx, {});", read, address);
        if (writeback && rn != rd) emit(out, "    ctx->r[{}] = oa;", rn);
        if (rd == 15) {
            // ldr pc, [sp], 4 is a pop
            bool pop = rn == 13 && !pre && sign == '+' && (op & (1u << 25)) == 0 && (op & 0xFFF) == 4;
            emit(out, "    {}(v); }}", pop ? "RETURN_TO_A" : "JUMP_TO");
            return false;
        }
        emit(out, "    ctx->r[{}] = v; }}", rd);
    } else {
        // storing r15 stores the address plus 12
        std::string value = rd == 15 ? scope.at(a + 12) : std::format("ctx->r[{}]", rd);
        if (byte) {
            emit(out, "    mem_write8(ctx, {}, (uint8_t){});", address, value);
        } else {
            emit(out, "    mem_write32(ctx, {}, {});", address, value);
        }
        if (writeback) emit(out, "    ctx->r[{}] = oa;", rn);
        emit(out, "    }}");
    }
    return true;
}

// ldrh, strh, ldrsb, ldrsh, ldrd and strd.
bool extra_transfer(std::string &out, const Scope &scope, uint32_t a, uint32_t op) {
    uint32_t rn = (op >> 16) & 0xF;
    uint32_t rd = (op >> 12) & 0xF;
    bool pre = (op & (1u << 24)) != 0;
    bool load = (op & (1u << 20)) != 0;
    bool writeback = !pre || (op & (1u << 21)) != 0;
    uint32_t kind = (op >> 5) & 3;
    bool pair = kind >= 2 && !load;
    if ((writeback && rn == 15) || (load && rd == 15) || (pair && ((rd & 1) != 0 || rd == 14))) {
        return interpret(out, scope, a, op);
    }
    std::string offset = (op & (1u << 22)) != 0 ? std::format("0x{:X}u", ((op >> 4) & 0xF0) | (op & 0xF))
                                                : reg(scope, op & 0xF, a + 8);
    char sign = (op & (1u << 23)) != 0 ? '+' : '-';
    const char *address = pre ? "oa" : "base";
    std::string back = writeback ? std::format(" ctx->r[{}] = oa;", rn) : std::string();
    emit(out, "    {{ uint32_t base = {}, oa = base {} {};", reg(scope, rn, a + 8), sign, offset);
    if (kind == 1 && load) {
        emit(out, "    uint32_t v = mem_read16(ctx, {});{} ctx->r[{}] = v; }}", address, back, rd);
    } else if (kind == 1) {
        emit(out, "    mem_write16(ctx, {}, (uint16_t){});{} }}", address, reg(scope, rd, a + 8), back);
    } else if (kind == 2 && load) {
        emit(out, "    uint32_t v = (uint32_t)(int32_t)(int8_t)mem_read8(ctx, {});{} ctx->r[{}] = v; }}", address, back,
             rd);
    } else if (kind == 2) {
        emit(out,
             "    uint32_t lo = mem_read32(ctx, {}), hi = mem_read32(ctx, {} + 4);{} ctx->r[{}] = lo; ctx->r[{}] = hi; }}",
             address, address, back, rd, rd + 1);
    } else if (load) {
        emit(out, "    uint32_t v = (uint32_t)(int32_t)(int16_t)mem_read16(ctx, {});{} ctx->r[{}] = v; }}", address,
             back, rd);
    } else {
        emit(out, "    mem_write32(ctx, {}, ctx->r[{}]); mem_write32(ctx, {} + 4, ctx->r[{}]);{} }}", address, rd,
             address, rd + 1, back);
    }
    return true;
}

// ldm and stm.
bool block_transfer(std::string &out, const Scope &scope, uint32_t a, uint32_t op) {
    uint32_t rn = (op >> 16) & 0xF;
    bool pre = (op & (1u << 24)) != 0;
    bool up = (op & (1u << 23)) != 0;
    bool writeback = (op & (1u << 21)) != 0;
    bool load = (op & (1u << 20)) != 0;
    uint32_t list = op & 0xFFFF;
    // the user bank forms, empty lists and r15 as the base are rare enough
    // to leave alone
    if ((op & (1u << 22)) != 0 || list == 0 || rn == 15) return interpret(out, scope, a, op);
    uint32_t span = uint32_t(std::popcount(list)) * 4;
    std::string lowest, last;
    if (up && !pre) {
        lowest = "base", last = std::format("base + {}", span);
    } else if (up) {
        lowest = "base + 4", last = std::format("base + {}", span);
    } else if (!pre) {
        lowest = std::format("base - {}", span - 4), last = std::format("base - {}", span);
    } else {
        lowest = std::format("base - {}", span), last = std::format("base - {}", span);
    }
    emit(out, "    {{ uint32_t base = ctx->r[{}], p = {}, fin = {};", rn, lowest, last);
    std::vector<uint32_t> registers;
    for (uint32_t i = 0; i < 16; i++) {
        if ((list & (1u << i)) != 0) registers.push_back(i);
    }
    if (load) {
        if (writeback && (list & (1u << rn)) == 0) emit(out, "    ctx->r[{}] = fin;", rn);
        // r15 comes last, and where it leads is taken once the rest is in
        std::vector<std::string> destinations;
        for (uint32_t r : registers) destinations.push_back(r == 15 ? std::string("next") : std::format("ctx->r[{}]", r));
        if ((list & (1u << 15)) != 0) {
            emit(out, "    uint32_t next;");
            words(out, true, destinations);
            const char *jump = rn == 13 ? "RETURN_TO_A" : "JUMP_TO";
            emit(out, "    {}(next); }}", jump);
            return false;
        }
        words(out, true, destinations);
    } else {
        std::vector<std::string> values;
        for (uint32_t r : registers) {
            if (r == 15) {
                values.push_back(scope.at(a + 12));
            } else if (r == rn && writeback && (list & ((1u << rn) - 1)) != 0) {
                // the base when it is not the lowest register stores its new value
                values.push_back("fin");
            } else {
                values.push_back(std::format("ctx->r[{}]", r));
            }
        }
        words(out, false, values);
        if (writeback) emit(out, "    ctx->r[{}] = fin;", rn);
    }
    emit(out, "    (void)p; (void)fin; }}");
    return true;
}

// swp and swpb.
bool swap(std::string &out, const Scope &scope, uint32_t a, uint32_t op) {
    uint32_t rd = (op >> 12) & 0xF;
    if (rd == 15) return interpret(out, scope, a, op);
    bool byte = (op & (1u << 22)) != 0;
    const char *read = byte ? "mem_read8" : "mem_read32";
    const char *write = byte ? "mem_write8" : "mem_write32";
    const char *cast = byte ? "(uint8_t)" : "";
    emit(out,
         "    {{ uint32_t address = {}, source = {}, old = {}(ctx, address); {}(ctx, address, {}source); ctx->r[{}] = old; }}",
         reg(scope, (op >> 16) & 0xF, a + 8), reg(scope, op & 0xF, a + 8), read, write, cast, rd);
    return true;
}

// ldrex and strex in all their sizes. a store only goes through while
// the monitor still has its address, and a successful one clears it.
bool exclusive(std::string &out, const Scope &scope, uint32_t a, uint32_t op) {
    uint32_t rd = (op >> 12) & 0xF;
    uint32_t rm = op & 0xF;
    uint32_t size = (op >> 21) & 3;
    bool load = (op & (1u << 20)) != 0;
    // r15 as a destination, or a pair that runs into it
    if (rd == 15 || (size == 0b01 && (load ? rd : rm) >= 14)) return interpret(out, scope, a, op);
    std::string address = reg(scope, (op >> 16) & 0xF, a + 8);
    const char *read, *write, *cast;
    if (size == 0b00 || size == 0b01) {
        read = "mem_read32", write = "mem_write32", cast = "";
    } else if (size == 0b10) {
        read = "mem_read8", write = "mem_write8", cast = "(uint8_t)";
    } else {
        read = "mem_read16", write = "mem_write16", cast = "(uint16_t)";
    }
    if (load) {
        std::string value =
            size == 0b01
                ? std::format("ctx->r[{}] = mem_read32(ctx, address); ctx->r[{}] = mem_read32(ctx, address + 4);", rd,
                              rd + 1)
                : std::format("ctx->r[{}] = {}(ctx, address);", rd, read);
        emit(out, "    {{ uint32_t address = {}; ctx->exclusive = 1; ctx->exclusive_address = address; {} }}", address,
             value);
    } else {
        std::string store =
            size == 0b01
                ? std::format("mem_write32(ctx, address, ctx->r[{}]); mem_write32(ctx, address + 4, ctx->r[{}]);", rm,
                              rm + 1)
                : std::format("{}(ctx, address, {}ctx->r[{}]);", write, cast, rm);
        emit(out,
             "    {{ uint32_t address = {}; if (ctx->exclusive && ctx->exclusive_address == address) {{ {} ctx->exclusive "
             "= 0; ctx->r[{}] = 0; }} else ctx->r[{}] = 1; }}",
             address, store, rd, rd);
    }
    return true;
}

// mul and mla.
bool multiply(std::string &out, const Scope &scope, uint32_t a, uint32_t op) {
    uint32_t rd = (op >> 16) & 0xF;
    if (rd == 15) return interpret(out, scope, a, op);
    std::string product = std::format("{} * {}", reg(scope, op & 0xF, a + 8), reg(scope, (op >> 8) & 0xF, a + 8));
    std::string sum = (op & (1u << 21)) != 0 ? std::format(" + {}", reg(scope, (op >> 12) & 0xF, a + 8)) : std::string();
    const char *flags = (op & (1u << 20)) != 0 ? " ctx->n = r >> 31; ctx->z = r == 0;" : "";
    emit(out, "    {{ uint32_t r = {}{}; ctx->r[{}] = r;{} }}", product, sum, rd, flags);
    return true;
}

// umull, umlal, smull, smlal and umaal.
bool multiply_long(std::string &out, const Scope &scope, uint32_t a, uint32_t op) {
    uint32_t hi = (op >> 16) & 0xF;
    uint32_t lo = (op >> 12) & 0xF;
    if (hi == 15 || lo == 15) return interpret(out, scope, a, op);
    std::string m = reg(scope, op & 0xF, a + 8);
    std::string s = reg(scope, (op >> 8) & 0xF, a + 8);
    uint32_t kind = (op >> 21) & 7;
    std::string pair = std::format("((uint64_t)ctx->r[{}] << 32 | ctx->r[{}])", hi, lo);
    std::string value;
    switch (kind) {
    case 0b010: value = std::format("(uint64_t){} * {} + ctx->r[{}] + ctx->r[{}]", m, s, lo, hi); break;
    case 0b100: value = std::format("(uint64_t){} * {}", m, s); break;
    case 0b101: value = std::format("(uint64_t){} * {} + {}", m, s, pair); break;
    case 0b110: value = std::format("(uint64_t)((int64_t)(int32_t){} * (int32_t){})", m, s); break;
    default: value = std::format("(uint64_t)((int64_t)(int32_t){} * (int32_t){}) + {}", m, s, pair); break;
    }
    const char *flags = kind != 0b010 && (op & (1u << 20)) != 0 ? " ctx->n = r >> 63; ctx->z = r == 0;" : "";
    emit(out, "    {{ uint64_t r = {}; ctx->r[{}] = (uint32_t)r; ctx->r[{}] = (uint32_t)(r >> 32);{} }}", value, lo, hi,
         flags);
    return true;
}

// smlaxy, smlawy, smulwy, smlalxy and smulxy. the 32 bit accumulates
// wrap and set q when they overflow.
bool halfword_multiply(std::string &out, const Scope &scope, uint32_t a, uint32_t op) {
    uint32_t rd = (op >> 16) & 0xF;
    uint32_t rn = (op >> 12) & 0xF;
    uint32_t kind = (op >> 21) & 3;
    if (rd == 15 || (kind == 0b10 && rn == 15)) return interpret(out, scope, a, op);
    uint32_t x = (op >> 5) & 1, y = (op >> 6) & 1;
    std::string m = reg(scope, op & 0xF, a + 8);
    std::string s = reg(scope, (op >> 8) & 0xF, a + 8);
    std::string n = reg(scope, rn, a + 8);
    switch (kind) {
    case 0b00:
        emit(out, "    ctx->r[{}] = accumulate(ctx, HALF({}, {}) * HALF({}, {}) + (int32_t){});", rd, m, x, s, y, n);
        break;
    case 0b01: {
        std::string product = std::format("((int64_t)(int32_t){} * HALF({}, {})) >> 16", m, s, y);
        if (x != 0) {
            emit(out, "    ctx->r[{}] = (uint32_t)({});", rd, product);
        } else {
            emit(out, "    ctx->r[{}] = accumulate(ctx, ({}) + (int32_t){});", rd, product, n);
        }
        break;
    }
    case 0b10:
        emit(out,
             "    {{ uint64_t r = ((uint64_t)ctx->r[{}] << 32 | ctx->r[{}]) + (uint64_t)(HALF({}, {}) * HALF({}, {})); "
             "ctx->r[{}] = (uint32_t)r; ctx->r[{}] = (uint32_t)(r >> 32); }}",
             rd, rn, m, x, s, y, rn, rd);
        break;
    default: emit(out, "    ctx->r[{}] = (uint32_t)(HALF({}, {}) * HALF({}, {}));", rd, m, x, s, y); break;
    }
    return true;
}

// qadd, qsub, qdadd and qdsub.
bool saturating(std::string &out, const Scope &scope, uint32_t a, uint32_t op) {
    uint32_t rd = (op >> 12) & 0xF;
    if (rd == 15) return interpret(out, scope, a, op);
    std::string m = reg(scope, op & 0xF, a + 8);
    std::string n = reg(scope, (op >> 16) & 0xF, a + 8);
    std::string value;
    switch ((op >> 21) & 3) {
    case 0b00: value = std::format("saturate(ctx, (int64_t)(int32_t){} + (int32_t){})", m, n); break;
    case 0b01: value = std::format("saturate(ctx, (int64_t)(int32_t){} - (int32_t){})", m, n); break;
    case 0b10:
        value = std::format("saturate(ctx, (int64_t)(int32_t){} + (int32_t)saturate(ctx, (int64_t)(int32_t){} * 2))", m, n);
        break;
    default:
        value = std::format("saturate(ctx, (int64_t)(int32_t){} - (int32_t)saturate(ctx, (int64_t)(int32_t){} * 2))", m, n);
        break;
    }
    emit(out, "    ctx->r[{}] = {};", rd, value);
    return true;
}

bool clz(std::string &out, const Scope &scope, uint32_t a, uint32_t op) {
    uint32_t rd = (op >> 12) & 0xF;
    if (rd == 15) return interpret(out, scope, a, op);
    emit(out, "    {{ uint32_t m = {}; ctx->r[{}] = m ? __builtin_clz(m) : 32; }}", reg(scope, op & 0xF, a + 8), rd);
    return true;
}

// bx and blx through a register.
bool branch_exchange(std::string &out, const Scope &scope, uint32_t a, uint32_t op) {
    uint32_t rm = op & 0xF;
    if (((op >> 4) & 0xF) == 0b0011) {
        uint32_t next = a + 4;
        emit(out,
             "    {{ uint32_t v = {}; ctx->r[14] = {}; ctx->thumb = v & 1; ctx->r[15] = v & (ctx->thumb ? ~1u : ~3u); }}",
             reg(scope, rm, a + 8), scope.at(next));
        emit(out, "    CALL(recomp_call);");
        emit(out, "    RETURNED({});", scope.at(next));
        return true;
    }
    if (rm == 14) {
        emit(out, "    RETURN_TO_A(ctx->r[14]);");
    } else {
        emit(out, "    JUMP_TO({});", reg(scope, rm, a + 8));
    }
    return false;
}

bool parallel(std::string &out, const Scope &scope, uint32_t a, uint32_t op, uint32_t op1, uint32_t op2);
bool pack_saturate_extend(std::string &out, const Scope &scope, uint32_t a, uint32_t op, uint32_t op1, uint32_t op2);
bool dual_multiply(std::string &out, const Scope &scope, uint32_t a, uint32_t op, uint32_t op1);
bool usad8(std::string &out, const Scope &scope, uint32_t a, uint32_t op);

// the ARMv6 media instructions, decoded in zakuro-cpu's order.
bool media(std::string &out, const Scope &scope, uint32_t a, uint32_t op) {
    uint32_t op1 = (op >> 20) & 0x1F;
    uint32_t op2 = (op >> 5) & 7;
    switch (op1 >> 3) {
    case 0b00: return parallel(out, scope, a, op, op1, op2);
    case 0b01: return pack_saturate_extend(out, scope, a, op, op1, op2);
    case 0b10: return dual_multiply(out, scope, a, op, op1);
    default:
        if (op1 == 0b11000 && op2 == 0) return usad8(out, scope, a, op);
        return interpret(out, scope, a, op);
    }
}

// the parallel adds and subtracts, sadd16 to uhsub8.
bool parallel(std::string &out, const Scope &scope, uint32_t a, uint32_t op, uint32_t op1, uint32_t op2) {
    uint32_t rd = (op >> 12) & 0xF;
    if (rd == 15 || (op1 & 3) == 0 || op2 == 5 || op2 == 6) return interpret(out, scope, a, op);
    std::string n = reg(scope, (op >> 16) & 0xF, a + 8);
    std::string m = reg(scope, op & 0xF, a + 8);
    emit(out, "    ctx->r[{}] = media_parallel(ctx, {}, {}, {}, {});", rd, op1, op2, n, m);
    return true;
}

// packing, the extensions, sel, the reversals and the saturations.
bool pack_saturate_extend(std::string &out, const Scope &scope, uint32_t a, uint32_t op, uint32_t op1, uint32_t op2) {
    uint32_t rd = (op >> 12) & 0xF;
    if (rd == 15) return interpret(out, scope, a, op);
    uint32_t rn = (op >> 16) & 0xF;
    std::string n = reg(scope, rn, a + 8);
    std::string m = reg(scope, op & 0xF, a + 8);
    uint32_t shift = (op >> 7) & 0x1F;
    bool is_unsigned = (op1 & 0b100) != 0;
    uint32_t low = op1 & 7;
    std::string value;
    if (low == 0b000 && (op2 == 0 || op2 == 2 || op2 == 4 || op2 == 6) && op1 == 0b01000) {
        // pkhbt and pkhtb
        if ((op & (1u << 6)) != 0) {
            value = std::format("((uint32_t)((int32_t){} >> {}) & 0xFFFF) | ({} & 0xFFFF0000u)", m,
                                shift == 0 ? 31 : shift, n);
        } else {
            value = std::format("({} & 0xFFFF) | (({} << {}) & 0xFFFF0000u)", n, m, shift);
        }
    } else if (op2 == 0b011) {
        uint32_t rotate = ((op >> 10) & 3) * 8;
        std::string x = rotate == 0 ? m : std::format("ror32({}, {})", m, rotate);
        std::string extended;
        bool packed;
        switch (low) {
        case 0b000:
            extended = std::format(
                "(((uint32_t)(int32_t)(int8_t){} & 0xFFFF) | (((uint32_t)(int32_t)(int8_t)({} >> 16) & 0xFFFF) << 16))",
                x, x);
            packed = true;
            break;
        case 0b010: extended = std::format("(uint32_t)(int32_t)(int8_t){}", x), packed = false; break;
        case 0b011: extended = std::format("(uint32_t)(int32_t)(int16_t){}", x), packed = false; break;
        case 0b100: extended = std::format("({} & 0x00FF00FFu)", x), packed = true; break;
        case 0b110: extended = std::format("({} & 0xFF)", x), packed = false; break;
        case 0b111: extended = std::format("({} & 0xFFFF)", x), packed = false; break;
        default: return interpret(out, scope, a, op);
        }
        if (rn == 15) {
            value = extended;
        } else if (packed) {
            value = std::format("add_halves(ctx->r[{}], {})", rn, extended);
        } else {
            value = std::format("ctx->r[{}] + {}", rn, extended);
        }
    } else if (low == 0b000 && op2 == 0b101 && op1 == 0b01000) {
        value = std::format("media_select(ctx, {}, {})", n, m);
    } else if (low == 0b011 && op2 == 0b001 && op1 == 0b01011) {
        value = std::format("__builtin_bswap32({})", m);
    } else if (low == 0b011 && op2 == 0b101 && op1 == 0b01011) {
        value = std::format("((({}) & 0x00FF00FFu) << 8) | ((({}) & 0xFF00FF00u) >> 8)", m, m);
    } else if (low == 0b111 && op2 == 0b001 && op1 == 0b01111) {
        value = std::format("reverse_bits({})", m);
    } else if (low == 0b111 && op2 == 0b101 && op1 == 0b01111) {
        value = std::format("(uint32_t)(int32_t)(int16_t)(((({}) & 0xFF) << 8) | ((({}) >> 8) & 0xFF))", m, m);
    } else if ((low == 0b010 || low == 0b110) && op2 == 0b001) {
        // ssat16 and usat16
        uint32_t bits = ((op >> 16) & 0xF) + uint32_t(!is_unsigned);
        value = std::format("media_saturate16(ctx, {}, {}, {})", m, bits, unsigned(is_unsigned));
    } else if ((op2 & 1) == 0 && (((op1 >> 2) & 0b111) == 0b010 || ((op1 >> 2) & 0b111) == 0b011)) {
        // ssat and usat, of a value shifted first
        uint32_t bits = ((op >> 16) & 0x1F) + uint32_t(!is_unsigned);
        std::string shifted = (op & (1u << 6)) != 0
                                  ? std::format("(int64_t)((int32_t){} >> {})", m, shift == 0 ? 31 : shift)
                                  : std::format("(int64_t)(int32_t)({} << {})", m, shift);
        value = std::format("media_saturate(ctx, {}, {}, {})", shifted, bits, unsigned(is_unsigned));
    } else {
        return interpret(out, scope, a, op);
    }
    emit(out, "    ctx->r[{}] = {};", rd, value);
    return true;
}

// smlad, smuad, smlsd, smusd, smlald, smlsld, smmul, smmla and smmls.
bool dual_multiply(std::string &out, const Scope &scope, uint32_t a, uint32_t op, uint32_t op1) {
    uint32_t rd = (op >> 16) & 0xF;
    uint32_t ra = (op >> 12) & 0xF;
    if (rd == 15 || (op1 == 0b10100 && ra == 15)) return interpret(out, scope, a, op);
    std::string m = reg(scope, op & 0xF, a + 8);
    std::string s = reg(scope, (op >> 8) & 0xF, a + 8);
    bool bit5 = (op & (1u << 5)) != 0;
    bool subtract = (op & (1u << 6)) != 0;
    switch (op1) {
    case 0b10000:
    case 0b10100: {
        // bit 5 swaps the halves of the second operand
        std::string b = bit5 ? std::format("ror32({}, 16)", s) : s;
        char sign = subtract ? '-' : '+';
        std::string dual =
            std::format("((int64_t)(int16_t){} * (int16_t){} {} (int64_t)(int16_t)({} >> 16) * (int16_t)({} >> 16))", m,
                        b, sign, m, b);
        if (op1 == 0b10000) {
            std::string sum = ra == 15 ? dual : std::format("{} + (int32_t)ctx->r[{}]", dual, ra);
            emit(out, "    ctx->r[{}] = accumulate(ctx, {});", rd, sum);
        } else {
            emit(out,
                 "    {{ uint64_t r = ((uint64_t)ctx->r[{}] << 32 | ctx->r[{}]) + (uint64_t){}; ctx->r[{}] = (uint32_t)r; "
                 "ctx->r[{}] = (uint32_t)(r >> 32); }}",
                 rd, ra, dual, ra, rd);
        }
        break;
    }
    // the top of a full 64 bit product, bit 5 rounding it
    case 0b10101: {
        std::string product = std::format("(uint64_t)((int64_t)(int32_t){} * (int32_t){})", m, s);
        std::string acc = ra == 15 ? std::string("0") : std::format("((uint64_t)ctx->r[{}] << 32)", ra);
        char sign = subtract ? '-' : '+';
        const char *round = bit5 ? " + 0x80000000u" : "";
        emit(out, "    ctx->r[{}] = (uint32_t)(({} {} {}{}) >> 32);", rd, acc, sign, product, round);
        break;
    }
    default: return interpret(out, scope, a, op);
    }
    return true;
}

// usad8 and usada8.
bool usad8(std::string &out, const Scope &scope, uint32_t a, uint32_t op) {
    uint32_t rd = (op >> 16) & 0xF;
    uint32_t ra = (op >> 12) & 0xF;
    if (rd == 15) return interpret(out, scope, a, op);
    std::string sum = std::format("media_usad8({}, {})", reg(scope, op & 0xF, a + 8), reg(scope, (op >> 8) & 0xF, a + 8));
    std::string value = ra == 15 ? sum : std::format("{} + ctx->r[{}]", sum, ra);
    emit(out, "    ctx->r[{}] = {};", rd, value);
    return true;
}

// the cond 0xF space.
bool unconditional(std::string &out, const Scope &scope, uint32_t a, uint32_t op) {
    if ((op & 0xFE000000) == 0xFA000000) {
        // blx to Thumb
        uint32_t offset = (sign_extend_24(op & 0x00FFFFFF) << 2) | (((op >> 24) & 1) << 1);
        return call(out, scope, a + 4, ((a + 8) + offset) & ~1u, true);
    }
    if ((op & 0xFFF000F0) == 0xF5700010) {
        // clrex
        emit(out, "    ctx->exclusive = 0;");
        return true;
    }
    // pld and the barriers do nothing here
    if (((op >> 25) & 7) == 0b010 || ((op >> 25) & 7) == 0b011) return true;
    return interpret(out, scope, a, op);
}

} // namespace

} // namespace recomp::codegen::arm
