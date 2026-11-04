// Lowering VFPv2 to C. Translated from 3dsrecomp's codegen/vfp.rs (Rust,
// MIT, Copyright (c) 2026 fearkov; see LICENSE-3dsrecomp).
//
// it follows zakuro-cpu's VFP, which computes singles as f32 and doubles as
// f64, rounding once, and flushes results to zero when fpscr asks.

#include <algorithm>

#include "scope.h"

namespace recomp::codegen::vfp {

namespace {

bool interpret(std::string &out, const Scope &scope, uint32_t a, uint32_t op) {
    emit(out, "    INTERPRET({}, 0x{:08X}u);", scope.at(a), op);
    scope.interpreted.push_back(a);
    return true;
}

uint32_t sreg(uint32_t vd, bool extra) { return ((vd << 1) | uint32_t(extra)) & 31; }

uint32_t dreg(uint32_t vd, bool extra) { return (uint32_t(extra) << 4) | vd; }

// a register of either size as C, doubles taking the two singles at 2N.
std::string get(bool is_double, uint32_t number) {
    if (is_double) return std::format("vfp_d(ctx, {})", number & 15);
    return std::format("vfp_s(ctx, {})", number);
}

std::string set(bool is_double, uint32_t number, std::string_view value) {
    if (is_double) return std::format("vfp_set_d(ctx, {}, {});", number & 15, value);
    return std::format("vfp_set_s(ctx, {}, {});", number, value);
}

// writes body, or when fpscr has the instruction working on a short
// vector, which only a destination past the first bank can, the loop
// over it. operation numbers the arithmetic the way vfp_apply does, and
// registers are d, n and m.
bool scalar(std::string &out, bool is_double, uint32_t operation, uint32_t d, uint32_t n, uint32_t m,
            const std::string &body) {
    uint32_t bank = is_double ? 4 : 8;
    if (d < bank) {
        emit(out, "    {{ {} }}", body);
    } else {
        emit(out, "    if (UNLIKELY(*ctx->fpscr & FPSCR_LEN)) vfp_vector(ctx, {}, {}, {}, {}, {}); else {{ {} }}", operation,
             unsigned(is_double), d, n, m, body);
    }
    return true;
}

bool extension(std::string &out, const Scope &scope, uint32_t a, uint32_t op, bool is_double, uint32_t rd, uint32_t rm) {
    bool top = (op & (1u << 7)) != 0;
    uint32_t kind = (op >> 16) & 0xF;
    // vmov, vabs, vneg and vsqrt, moves and signs change bits untouched,
    // a double's too, which can be two singles reading as a subnormal
    if (kind == 0b0000 || kind == 0b0001) {
        bool second = ((op >> 16) & 1) != 0;
        std::string body;
        if (is_double) {
            uint32_t d = 2 * (rd & 15), m = 2 * (rm & 15);
            std::string high;
            if (!second && !top) {
                high = std::format("ctx->vfp[{}]", m + 1);
            } else if (!second) {
                high = std::format("ctx->vfp[{}] & 0x7FFFFFFFu", m + 1);
            } else if (!top) {
                high = std::format("ctx->vfp[{}] ^ 0x80000000u", m + 1);
            }
            if (high.empty()) {
                body = std::format("double v = {}; {}", get(true, rm), set(true, rd, "sqrt(v)"));
            } else {
                body = std::format("uint32_t low = ctx->vfp[{}], high = {}; ctx->vfp[{}] = low; ctx->vfp[{}] = high;", m,
                                   high, d, d + 1);
            }
        } else if (!second && !top) {
            body = std::format("ctx->vfp[{}] = ctx->vfp[{}];", rd, rm);
        } else if (!second) {
            body = std::format("ctx->vfp[{}] = ctx->vfp[{}] & 0x7FFFFFFFu;", rd, rm);
        } else if (!top) {
            body = std::format("ctx->vfp[{}] = ctx->vfp[{}] ^ 0x80000000u;", rd, rm);
        } else {
            body = set(false, rd, std::format("sqrtf({})", get(false, rm)));
        }
        return scalar(out, is_double, 9 + ((uint32_t(second) << 1) | uint32_t(top)), rd, rd, rm, body);
    }
    // vcmp against a register or zero, singles compared as doubles
    if (kind == 0b0100) {
        emit(out, "    vfp_compare(ctx, {}, {});", get(is_double, rd), get(is_double, rm));
        return true;
    }
    if (kind == 0b0101) {
        emit(out, "    vfp_compare(ctx, {}, 0.0);", get(is_double, rd));
        return true;
    }
    // vcvt between single and double
    if (kind == 0b0111 && top) {
        uint32_t vd = (op >> 12) & 0xF, vm = op & 0xF;
        bool d_bit = (op & (1u << 22)) != 0, m_bit = (op & (1u << 5)) != 0;
        if (is_double) {
            emit(out, "    {}", set(false, sreg(vd, d_bit), std::format("(float){}", get(true, rm))));
        } else {
            emit(out, "    {}", set(true, dreg(vd, d_bit), std::format("(double){}", get(false, sreg(vm, m_bit)))));
        }
        return true;
    }
    // vcvt from an integer, bit 7 set for signed
    if (kind == 0b1000) {
        uint32_t source = sreg(op & 0xF, (op & (1u << 5)) != 0);
        std::string integer =
            top ? std::format("(double)(int32_t)ctx->vfp[{}]", source) : std::format("(double)ctx->vfp[{}]", source);
        std::string value = is_double ? integer : std::format("(float){}", integer);
        emit(out, "    {}", set(is_double, rd, value));
        return true;
    }
    // vcvt to an integer rounding toward zero, the other rounding modes
    // are rare enough for the interpreter
    if ((kind == 0b1100 || kind == 0b1101) && top) {
        std::string source = is_double ? get(true, rm) : std::format("(double){}", get(false, rm));
        const char *convert = ((op >> 16) & 1) != 0 ? "vfp_to_s32" : "vfp_to_u32";
        uint32_t destination = sreg((op >> 12) & 0xF, (op & (1u << 22)) != 0);
        emit(out, "    ctx->vfp[{}] = {}({});", destination, convert, source);
        return true;
    }
    return interpret(out, scope, a, op);
}

bool move_pair(std::string &out, const Scope &scope, uint32_t a, uint32_t op, bool is_double);

} // namespace

// the cdp space of coprocessors 10 and 11, the arithmetic, the unary
// operations, comparisons and conversions.
bool data_processing(std::string &out, const Scope &scope, uint32_t a, uint32_t op) {
    uint32_t coprocessor = (op >> 8) & 0xF;
    if (coprocessor != 10 && coprocessor != 11) return interpret(out, scope, a, op);
    bool is_double = coprocessor == 11;
    uint32_t vd = (op >> 12) & 0xF, vn = (op >> 16) & 0xF, vm = op & 0xF;
    bool d_bit = (op & (1u << 22)) != 0, n_bit = (op & (1u << 7)) != 0, m_bit = (op & (1u << 5)) != 0;
    uint32_t rd, rn, rm;
    if (is_double) {
        rd = dreg(vd, d_bit), rn = dreg(vn, n_bit), rm = dreg(vm, m_bit);
    } else {
        rd = sreg(vd, d_bit), rn = sreg(vn, n_bit), rm = sreg(vm, m_bit);
    }
    bool negate = (op & (1u << 6)) != 0;
    uint32_t operation;
    const char *expression;
    switch (((op >> 20) & 0b1011) << 1 | uint32_t(negate)) {
    case 0b0000 << 1 | 0: operation = 0, expression = "acc + a * b"; break;
    case 0b0000 << 1 | 1: operation = 1, expression = "acc - a * b"; break;
    case 0b0001 << 1 | 0: operation = 2, expression = "-acc + a * b"; break;
    case 0b0001 << 1 | 1: operation = 3, expression = "-acc - a * b"; break;
    case 0b0010 << 1 | 0: operation = 4, expression = "a * b"; break;
    case 0b0010 << 1 | 1: operation = 5, expression = "-(a * b)"; break;
    case 0b0011 << 1 | 0: operation = 6, expression = "a + b"; break;
    case 0b0011 << 1 | 1: operation = 7, expression = "a - b"; break;
    case 0b1000 << 1 | 0: operation = 8, expression = "a / b"; break;
    case 0b1011 << 1 | 1: return extension(out, scope, a, op, is_double, rd, rm);
    default: return interpret(out, scope, a, op);
    }
    // a vector runs its operand m as a scalar when m is in the first bank,
    // which only matters to the interpreter
    const char *kind = is_double ? "double" : "float";
    std::string body = std::format("{} a = {}, b = {}, acc = {}; {}", kind, get(is_double, rn), get(is_double, rm),
                                   get(is_double, rd), set(is_double, rd, expression));
    return scalar(out, is_double, operation, rd, rn, rm, body);
}

// the mrc and mcr space of coprocessors 10 and 11, moves between core and
// VFP registers and to and from fpscr.
bool register_transfer(std::string &out, const Scope &scope, uint32_t a, uint32_t op) {
    uint32_t coprocessor = (op >> 8) & 0xF;
    bool load = (op & (1u << 20)) != 0;
    uint32_t rt = (op >> 12) & 0xF;
    bool vfp = coprocessor == 10 || coprocessor == 11;
    uint32_t kind = (op >> 21) & 7;
    // vmrs and vmsr on fpscr
    if (vfp && kind == 7 && ((op >> 16) & 0xF) == 1) {
        if (!load) {
            emit(out, "    *ctx->fpscr = {};", rt == 15 ? scope.at(a + 8) : std::format("ctx->r[{}]", rt));
        } else if (rt == 15) {
            emit(out, "    {{ uint32_t f = *ctx->fpscr; ctx->n = f >> 31; ctx->z = (f >> 30) & 1; ctx->c = (f >> 29) & 1; "
                      "ctx->v = (f >> 28) & 1; }}");
        } else {
            emit(out, "    ctx->r[{}] = *ctx->fpscr;", rt);
        }
        return true;
    }
    // vmov between a core register and a single
    if (vfp && kind == 0 && rt != 15) {
        uint32_t sn = sreg((op >> 16) & 0xF, (op & (1u << 7)) != 0);
        if (load) {
            emit(out, "    ctx->r[{}] = ctx->vfp[{}];", rt, sn);
        } else {
            emit(out, "    ctx->vfp[{}] = ctx->r[{}];", sn, rt);
        }
        return true;
    }
    return interpret(out, scope, a, op);
}

// the ldc and stc space of coprocessors 10 and 11, vldr, vstr, vldm and
// vstm, and the moves between two core registers and a double.
bool load_store(std::string &out, const Scope &scope, uint32_t a, uint32_t op) {
    uint32_t coprocessor = (op >> 8) & 0xF;
    if (coprocessor != 10 && coprocessor != 11) return interpret(out, scope, a, op);
    bool is_double = coprocessor == 11;
    if (((op >> 21) & 0x7F) == 0b1100010) return move_pair(out, scope, a, op, is_double);
    bool load = (op & (1u << 20)) != 0;
    bool pre = (op & (1u << 24)) != 0;
    bool up = (op & (1u << 23)) != 0;
    bool writeback = (op & (1u << 21)) != 0;
    uint32_t rn = (op >> 16) & 0xF;
    if (writeback && rn == 15) return interpret(out, scope, a, op);
    uint32_t offset = (op & 0xFF) * 4;
    uint32_t vd = (op >> 12) & 0xF;
    bool d_bit = (op & (1u << 22)) != 0;
    uint32_t first = is_double ? dreg(vd, d_bit) : sreg(vd, d_bit);
    uint32_t count = pre && !writeback ? 1 : is_double ? (op & 0xFF) / 2 : op & 0xFF;
    std::string base = rn == 15 ? scope.at(a + 8) : std::format("ctx->r[{}]", rn);
    std::string start;
    if (up && pre && !writeback) {
        start = std::format("base + 0x{:X}u", offset);
    } else if (up) {
        start = "base";
    } else {
        start = std::format("base - 0x{:X}u", offset);
    }
    emit(out, "    {{ uint32_t base = {}, p = {};", base, start);
    // a double is two words, the singles at 2N and 2N + 1
    std::vector<std::string> registers;
    for (uint32_t i = 0; i < std::max(count, 1u); i++) {
        if (is_double) {
            registers.push_back(std::format("ctx->vfp[{}]", ((first + i) * 2) & 31));
            registers.push_back(std::format("ctx->vfp[{}]", ((first + i) * 2 + 1) & 31));
        } else {
            registers.push_back(std::format("ctx->vfp[{}]", (first + i) & 31));
        }
    }
    words(out, load, registers);
    if (writeback) {
        char sign = up ? '+' : '-';
        emit(out, "    ctx->r[{}] = base {} 0x{:X}u;", rn, sign, offset);
    }
    emit(out, "    (void)p; }}");
    return true;
}

namespace {

// vmov between two core registers and a double or two singles.
bool move_pair(std::string &out, const Scope &scope, uint32_t a, uint32_t op, bool is_double) {
    bool load = (op & (1u << 20)) != 0;
    uint32_t rt = (op >> 12) & 0xF;
    uint32_t rt2 = (op >> 16) & 0xF;
    if (rt == 15 || rt2 == 15) return interpret(out, scope, a, op);
    uint32_t vm = op & 0xF;
    bool m_bit = (op & (1u << 5)) != 0;
    uint32_t low, high;
    if (is_double) {
        uint32_t dm = dreg(vm, m_bit);
        low = (dm * 2) & 31, high = (dm * 2 + 1) & 31;
    } else {
        uint32_t sm = sreg(vm, m_bit);
        low = sm, high = (sm + 1) & 31;
    }
    if (load) {
        emit(out, "    ctx->r[{}] = ctx->vfp[{}]; ctx->r[{}] = ctx->vfp[{}];", rt, low, rt2, high);
    } else {
        emit(out, "    ctx->vfp[{}] = ctx->r[{}]; ctx->vfp[{}] = ctx->r[{}];", low, rt, high, rt2);
    }
    return true;
}

} // namespace

} // namespace recomp::codegen::vfp
