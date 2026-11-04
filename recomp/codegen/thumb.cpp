// Lowering Thumb instructions to C. Translated from 3dsrecomp's
// codegen/thumb.rs (Rust, MIT, Copyright (c) 2026 fearkov; see
// LICENSE-3dsrecomp).
//
// it follows zakuro-cpu's decoding the same way the ARM side does. r15
// reads as the address plus 4.

#include <bit>

#include "scope.h"

namespace recomp::codegen::thumb {

namespace {

#define NZ "ctx->n = r >> 31; ctx->z = r == 0;"
#define ADD_FLAGS "ctx->n = r >> 31; ctx->z = r == 0; ctx->c = r < a; ctx->v = ((a ^ r) & (b ^ r)) >> 31;"
#define SUB_FLAGS "ctx->n = r >> 31; ctx->z = r == 0; ctx->c = a >= b; ctx->v = ((a ^ b) & (a ^ r)) >> 31;"

uint32_t sign_extend(uint32_t value, uint32_t bits) {
    return uint32_t(int32_t(value << (32 - bits)) >> (32 - bits));
}

// reading a register, where r15 is the address plus 4.
std::string reg(const Scope &scope, uint32_t number, uint32_t a) {
    if (number == 15) return scope.at(a + 4);
    return std::format("ctx->r[{}]", number);
}

bool interpret(std::string &out, const Scope &scope, uint32_t a, uint32_t op) {
    emit(out, "    INTERPRET({}, 0x{:04X}u);", scope.at(a), op);
    scope.interpreted.push_back(a);
    return true;
}

bool add_subtract(std::string &out, uint32_t op);
bool shift_immediate(std::string &out, uint32_t op);
bool immediate(std::string &out, uint32_t op);
bool register_offset(std::string &out, uint32_t op);
bool high_register(std::string &out, const Scope &scope, uint32_t a, uint32_t op);
bool alu(std::string &out, uint32_t op);
bool immediate_offset(std::string &out, uint32_t op);
bool miscellaneous(std::string &out, const Scope &scope, uint32_t a, uint32_t op);
bool conditional_branch(std::string &out, const Scope &scope, uint32_t a, uint32_t op);
bool block_transfer(std::string &out, uint32_t op);

} // namespace

bool lower(std::string &out, const Scope &scope, uint32_t a, uint32_t op, uint32_t second) {
    switch (op >> 13) {
    case 0b000:
        if (((op >> 11) & 3) == 0b11) return add_subtract(out, op);
        return shift_immediate(out, op);
    case 0b001: return immediate(out, op);
    case 0b010:
        if ((op & (1u << 12)) != 0) return register_offset(out, op);
        if ((op & (1u << 11)) != 0) {
            uint32_t literal = ((a + 4) & ~3u) + (op & 0xFF) * 4;
            emit(out, "    ctx->r[{}] = mem_read32(ctx, {});", (op >> 8) & 7, scope.at(literal));
            return true;
        }
        if ((op & (1u << 10)) != 0) return high_register(out, scope, a, op);
        return alu(out, op);
    case 0b011: return immediate_offset(out, op);
    case 0b100:
        if ((op & (1u << 12)) != 0) {
            uint32_t rd = (op >> 8) & 7;
            std::string address = std::format("ctx->r[13] + 0x{:X}u", (op & 0xFF) * 4);
            if ((op & (1u << 11)) != 0) {
                emit(out, "    ctx->r[{}] = mem_read32(ctx, {});", rd, address);
            } else {
                emit(out, "    mem_write32(ctx, {}, ctx->r[{}]);", address, rd);
            }
            return true;
        } else {
            uint32_t rd = op & 7, rb = (op >> 3) & 7;
            std::string address = std::format("ctx->r[{}] + 0x{:X}u", rb, ((op >> 6) & 0x1F) * 2);
            if ((op & (1u << 11)) != 0) {
                emit(out, "    ctx->r[{}] = mem_read16(ctx, {});", rd, address);
            } else {
                emit(out, "    mem_write16(ctx, {}, (uint16_t)ctx->r[{}]);", address, rd);
            }
            return true;
        }
    case 0b101:
        if ((op & (1u << 12)) != 0) return miscellaneous(out, scope, a, op);
        {
            // add rd, pc or sp, imm
            uint32_t offset = (op & 0xFF) * 4;
            uint32_t rd = (op >> 8) & 7;
            if ((op & (1u << 11)) != 0) {
                emit(out, "    ctx->r[{}] = ctx->r[13] + 0x{:X}u;", rd, offset);
            } else {
                emit(out, "    ctx->r[{}] = {};", rd, scope.at(((a + 4) & ~3u) + offset));
            }
            return true;
        }
    case 0b110:
        if ((op & (1u << 12)) != 0) return conditional_branch(out, scope, a, op);
        return block_transfer(out, op);
    default:
        switch (op >> 11) {
        case 0b11100: return jump(out, scope, (a + 4) + (sign_extend(op & 0x7FF, 11) << 1), true);
        case 0b11110:
            if ((second >> 11) == 0b11111 || (second >> 11) == 0b11101) {
                uint32_t high = sign_extend(op & 0x7FF, 11) << 12;
                uint32_t target = (a + 4) + high + ((second & 0x7FF) << 1);
                if ((second >> 11) == 0b11111) return call(out, scope, (a + 4) | 1, target, true);
                return call(out, scope, (a + 4) | 1, target & ~3u, false);
            }
            [[fallthrough]];
        // a half of bl on its own
        default: return interpret(out, scope, a, op);
        }
    }
}

namespace {

// lsl, lsr and asr by an immediate.
bool shift_immediate(std::string &out, uint32_t op) {
    uint32_t n = (op >> 6) & 0x1F;
    uint32_t kind = (op >> 11) & 3;
    std::string value, carry;
    bool has_carry = true;
    if (kind == 0 && n == 0) {
        value = "m", has_carry = false;
    } else if (kind == 0) {
        value = std::format("m << {}", n), carry = std::format("(m >> {}) & 1", 32 - n);
    } else if (kind == 1 && n == 0) {
        value = "0", carry = "m >> 31";
    } else if (kind == 1) {
        value = std::format("m >> {}", n), carry = std::format("(m >> {}) & 1", n - 1);
    } else if (n == 0) {
        value = "(uint32_t)((int32_t)m >> 31)", carry = "m >> 31";
    } else {
        value = std::format("(uint32_t)((int32_t)m >> {})", n), carry = std::format("(m >> {}) & 1", n - 1);
    }
    std::string set_carry = has_carry ? std::format(" ctx->c = {};", carry) : std::string();
    emit(out, "    {{ uint32_t m = ctx->r[{}], r = {};{} ctx->r[{}] = r; " NZ " }}", (op >> 3) & 7, value, set_carry, op & 7);
    return true;
}

// add and sub with a register or a 3 bit immediate.
bool add_subtract(std::string &out, uint32_t op) {
    uint32_t field = (op >> 6) & 7;
    std::string b = (op & (1u << 10)) != 0 ? std::format("{}u", field) : std::format("ctx->r[{}]", field);
    bool subtract = (op & (1u << 9)) != 0;
    const char *operation = subtract ? "a - b" : "a + b";
    const char *flags = subtract ? SUB_FLAGS : ADD_FLAGS;
    emit(out, "    {{ uint32_t a = ctx->r[{}], b = {}, r = {}; ctx->r[{}] = r; {} }}", (op >> 3) & 7, b, operation, op & 7,
         flags);
    return true;
}

// mov, cmp, add and sub with an 8 bit immediate.
bool immediate(std::string &out, uint32_t op) {
    uint32_t rd = (op >> 8) & 7;
    uint32_t imm = op & 0xFF;
    switch ((op >> 11) & 3) {
    case 0: emit(out, "    ctx->r[{}] = {}u; ctx->n = 0; ctx->z = {};", rd, imm, unsigned(imm == 0)); break;
    case 1: emit(out, "    {{ uint32_t a = ctx->r[{}], b = {}u, r = a - b; " SUB_FLAGS " }}", rd, imm); break;
    case 2: emit(out, "    {{ uint32_t a = ctx->r[{}], b = {}u, r = a + b; ctx->r[{}] = r; " ADD_FLAGS " }}", rd, imm, rd); break;
    default: emit(out, "    {{ uint32_t a = ctx->r[{}], b = {}u, r = a - b; ctx->r[{}] = r; " SUB_FLAGS " }}", rd, imm, rd); break;
    }
    return true;
}

// the register to register ALU operations.
bool alu(std::string &out, uint32_t op) {
    uint32_t rd = op & 7;
    uint32_t rs = (op >> 3) & 7;
    std::string body;
    switch ((op >> 6) & 0xF) {
    case 0x0: body = std::format("r = a & b; ctx->r[{}] = r; " NZ, rd); break;
    case 0x1: body = std::format("r = a ^ b; ctx->r[{}] = r; " NZ, rd); break;
    case 0x2:
    case 0x3:
    case 0x4:
    case 0x7: {
        uint32_t kind = (op >> 6) & 0xF;
        const char *shift = kind == 0x2 ? "shift_lsl" : kind == 0x3 ? "shift_lsr" : kind == 0x4 ? "shift_asr" : "shift_ror";
        body = std::format("uint8_t sc = ctx->c; r = {}(a, b, &sc); ctx->r[{}] = r; " NZ " ctx->c = sc;", shift, rd);
        break;
    }
    case 0x5:
        body = std::format("uint8_t ci = ctx->c; r = a + b + ci; ctx->r[{}] = r; " NZ
                           " ctx->c = ((uint64_t)a + b + ci) >> 32; ctx->v = ((a ^ r) & (b ^ r)) >> 31;",
                           rd);
        break;
    case 0x6:
        body = std::format("uint8_t ci = ctx->c; r = a - b - !ci; ctx->r[{}] = r; " NZ
                           " ctx->c = (uint64_t)a >= (uint64_t)b + !ci; ctx->v = ((a ^ b) & (a ^ r)) >> 31;",
                           rd);
        break;
    case 0x8: body = "r = a & b; " NZ; break;
    // neg is rsb 0
    case 0x9: body = std::format("r = 0 - b; ctx->r[{}] = r; " NZ " ctx->c = b == 0; ctx->v = (b & r) >> 31;", rd); break;
    case 0xA: body = "r = a - b; " SUB_FLAGS; break;
    case 0xB: body = "r = a + b; " ADD_FLAGS; break;
    case 0xC: body = std::format("r = a | b; ctx->r[{}] = r; " NZ, rd); break;
    case 0xD: body = std::format("r = a * b; ctx->r[{}] = r; " NZ, rd); break;
    case 0xE: body = std::format("r = a & ~b; ctx->r[{}] = r; " NZ, rd); break;
    default: body = std::format("r = ~b; ctx->r[{}] = r; " NZ, rd); break;
    }
    emit(out, "    {{ uint32_t a = ctx->r[{}], b = ctx->r[{}], r; {} }}", rd, rs, body);
    return true;
}

// add, cmp and mov on the high registers, and bx and blx.
bool high_register(std::string &out, const Scope &scope, uint32_t a, uint32_t op) {
    uint32_t rd = (op & 7) | ((op >> 4) & 8);
    uint32_t rs = (op >> 3) & 0xF;
    std::string source = reg(scope, rs, a);
    switch ((op >> 8) & 3) {
    case 0:
        if (rd == 15) {
            // writing pc keeps Thumb
            emit(out, "    target = ({} + {}) & ~1u; goto dispatch;", reg(scope, rd, a), source);
            return false;
        }
        emit(out, "    ctx->r[{}] = {} + {};", rd, reg(scope, rd, a), source);
        return true;
    case 1: emit(out, "    {{ uint32_t a = {}, b = {}, r = a - b; " SUB_FLAGS " }}", reg(scope, rd, a), source); return true;
    case 2:
        if (rd == 15 && rs == 14) {
            emit(out, "    ctx->r[15] = ctx->r[14] & ~1u; RETURN();");
            return false;
        }
        if (rd == 15) {
            emit(out, "    target = {} & ~1u; goto dispatch;", source);
            return false;
        }
        emit(out, "    ctx->r[{}] = {};", rd, source);
        return true;
    default:
        if ((op & (1u << 7)) != 0) {
            uint32_t next = a + 2;
            emit(out,
                 "    {{ uint32_t v = {}; ctx->r[14] = {} | 1; ctx->thumb = v & 1; ctx->r[15] = v & (ctx->thumb ? ~1u : "
                 "~3u); }}",
                 source, scope.at(next));
            emit(out, "    CALL(recomp_call);");
            emit(out, "    RETURNED_T({});", scope.at(next));
            return true;
        }
        if (rs == 14) {
            emit(out, "    RETURN_TO_T(ctx->r[14]);");
            return false;
        }
        emit(out, "    JUMP_TO({});", source);
        return false;
    }
}

// loads and stores with a register offset.
bool register_offset(std::string &out, uint32_t op) {
    uint32_t rd = op & 7;
    std::string address = std::format("ctx->r[{}] + ctx->r[{}]", (op >> 3) & 7, (op >> 6) & 7);
    bool is_signed = (op & (1u << 9)) != 0;
    uint32_t kind = (op >> 10) & 3;
    if (is_signed) {
        switch (kind) {
        case 0: emit(out, "    mem_write16(ctx, {}, (uint16_t)ctx->r[{}]);", address, rd); break;
        case 1: emit(out, "    ctx->r[{}] = (uint32_t)(int32_t)(int8_t)mem_read8(ctx, {});", rd, address); break;
        case 2: emit(out, "    ctx->r[{}] = mem_read16(ctx, {});", rd, address); break;
        default: emit(out, "    ctx->r[{}] = (uint32_t)(int32_t)(int16_t)mem_read16(ctx, {});", rd, address); break;
        }
    } else {
        switch (kind) {
        case 0: emit(out, "    mem_write32(ctx, {}, ctx->r[{}]);", address, rd); break;
        case 1: emit(out, "    mem_write8(ctx, {}, (uint8_t)ctx->r[{}]);", address, rd); break;
        case 2: emit(out, "    ctx->r[{}] = mem_read32(ctx, {});", rd, address); break;
        default: emit(out, "    ctx->r[{}] = mem_read8(ctx, {});", rd, address); break;
        }
    }
    return true;
}

// ldr, str, ldrb and strb with an immediate offset.
bool immediate_offset(std::string &out, uint32_t op) {
    uint32_t rd = op & 7;
    bool byte = (op & (1u << 12)) != 0;
    uint32_t offset = ((op >> 6) & 0x1F) * (byte ? 1 : 4);
    std::string address = std::format("ctx->r[{}] + 0x{:X}u", (op >> 3) & 7, offset);
    bool load = (op & (1u << 11)) != 0;
    if (load && !byte) {
        emit(out, "    ctx->r[{}] = mem_read32(ctx, {});", rd, address);
    } else if (load) {
        emit(out, "    ctx->r[{}] = mem_read8(ctx, {});", rd, address);
    } else if (!byte) {
        emit(out, "    mem_write32(ctx, {}, ctx->r[{}]);", address, rd);
    } else {
        emit(out, "    mem_write8(ctx, {}, (uint8_t)ctx->r[{}]);", address, rd);
    }
    return true;
}

bool push_pop(std::string &out, uint32_t op);

bool miscellaneous(std::string &out, const Scope &scope, uint32_t a, uint32_t op) {
    uint32_t rd = op & 7;
    std::string m = std::format("ctx->r[{}]", (op >> 3) & 7);
    switch ((op >> 8) & 0xF) {
    case 0b0000: {
        char sign = (op & (1u << 7)) != 0 ? '-' : '+';
        emit(out, "    ctx->r[13] = ctx->r[13] {} 0x{:X}u;", sign, (op & 0x7F) * 4);
        return true;
    }
    case 0b0010: {
        std::string value;
        switch ((op >> 6) & 3) {
        case 0: value = std::format("(uint32_t)(int32_t)(int16_t){}", m); break;
        case 1: value = std::format("(uint32_t)(int32_t)(int8_t){}", m); break;
        case 2: value = std::format("({} & 0xFFFF)", m); break;
        default: value = std::format("({} & 0xFF)", m); break;
        }
        emit(out, "    ctx->r[{}] = {};", rd, value);
        return true;
    }
    case 0b1010: {
        std::string value;
        switch ((op >> 6) & 3) {
        case 0: value = std::format("__builtin_bswap32({})", m); break;
        case 1: value = std::format("(({} & 0x00FF00FFu) << 8) | (({} & 0xFF00FF00u) >> 8)", m, m); break;
        case 3: value = std::format("(uint32_t)(int32_t)(int16_t)((({} & 0xFF) << 8) | (({} >> 8) & 0xFF))", m, m); break;
        default: return interpret(out, scope, a, op);
        }
        emit(out, "    ctx->r[{}] = {};", rd, value);
        return true;
    }
    // cps, setend and the hints do nothing here
    case 0b0110:
    case 0b1111: return true;
    case 0b0100:
    case 0b0101:
    case 0b1100:
    case 0b1101: return push_pop(out, op);
    default: return interpret(out, scope, a, op);
    }
}

bool push_pop(std::string &out, uint32_t op) {
    uint32_t list = op & 0xFF;
    bool extra = (op & (1u << 8)) != 0;
    uint32_t count = uint32_t(std::popcount(list)) + uint32_t(extra);
    std::vector<std::string> registers;
    for (uint32_t r = 0; r < 8; r++) {
        if ((list & (1u << r)) != 0) registers.push_back(std::format("ctx->r[{}]", r));
    }
    if ((op & (1u << 11)) != 0) {
        emit(out, "    {{ uint32_t p = ctx->r[13];");
        if (extra) {
            // where pc leads is taken once the rest is in
            emit(out, "    uint32_t next;");
            registers.push_back("next");
        }
        words(out, true, registers);
        emit(out, "    ctx->r[13] += {}u;", count * 4);
        if (extra) {
            emit(out, "    RETURN_TO_T(next); }}");
            return false;
        }
        emit(out, "    (void)p; }}");
    } else {
        emit(out, "    {{ uint32_t start = ctx->r[13] - {}u, p = start;", count * 4);
        if (extra) registers.push_back("ctx->r[14]");
        words(out, false, registers);
        emit(out, "    ctx->r[13] = start; (void)p; }}");
    }
    return true;
}

// ldmia and stmia.
bool block_transfer(std::string &out, uint32_t op) {
    uint32_t rb = (op >> 8) & 7;
    uint32_t list = op & 0xFF;
    if (list == 0) {
        emit(out, "    ctx->r[{}] += 0x40u;", rb);
        return true;
    }
    bool load = (op & (1u << 11)) != 0;
    emit(out, "    {{ uint32_t p = ctx->r[{}], fin = p + {}u;", rb, uint32_t(std::popcount(list)) * 4);
    std::vector<std::string> registers;
    for (uint32_t r = 0; r < 8; r++) {
        if ((list & (1u << r)) != 0) registers.push_back(std::format("ctx->r[{}]", r));
    }
    words(out, load, registers);
    if (!(load && (list & (1u << rb)) != 0)) emit(out, "    ctx->r[{}] = fin;", rb);
    emit(out, "    (void)fin; }}");
    return true;
}

// conditional branches, with svc and undefined in the last two conditions.
bool conditional_branch(std::string &out, const Scope &scope, uint32_t a, uint32_t op) {
    uint32_t condition = (op >> 8) & 0xF;
    switch (condition) {
    case 0xF: emit(out, "    SVC({}, 0x{:X}u);", scope.at(a + 2), op & 0xFF); return true; // interface 6: the thread waits in it, then goes on
    case 0xE: return interpret(out, scope, a, op);
    default: {
        uint32_t target = (a + 4) + (sign_extend(op & 0xFF, 8) << 1);
        emit(out, "    if ({}) {{", CONDITIONS[condition]);
        jump(out, scope, target, true);
        emit(out, "    }}");
        return true;
    }
    }
}

} // namespace

#undef NZ
#undef ADD_FLAGS
#undef SUB_FLAGS

} // namespace recomp::codegen::thumb
