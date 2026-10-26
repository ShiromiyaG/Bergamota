// The PICA's shader units, run on the CPU: a program decoded once per
// upload into operations with their operands worked out, then run per
// vertex. After Zakuro's zakuro-gpu shader/ (Rust, MIT, Copyright (c) 2026
// fearkov; see LICENSE-zakuro), written anew.

#include "shader.h"

#include <cmath>

namespace runtime::pica {

namespace {

enum Op : uint8_t {
    ADD = 0x00, DP3 = 0x01, DP4 = 0x02, DPH = 0x03, DST = 0x04, EX2 = 0x05, LG2 = 0x06, LITP = 0x07, MUL = 0x08,
    SGE = 0x09, SLT = 0x0A, FLR = 0x0B, MAX = 0x0C, MIN = 0x0D, RCP = 0x0E, RSQ = 0x0F, MOVA = 0x12, MOV = 0x13,
    DPHI = 0x18, DSTI = 0x19, SGEI = 0x1A, SLTI = 0x1B, BREAK = 0x20, NOP = 0x21, END = 0x22, BREAKC = 0x23,
    CALL = 0x24, CALLC = 0x25, CALLU = 0x26, IFU = 0x27, IFC = 0x28, LOOP = 0x29, EMIT = 0x2A, SETEMIT = 0x2B,
    JMPC = 0x2C, JMPU = 0x2D, CMP = 0x2E, MADI = 0x30, MAD = 0x38,
};

// the shader's multiply: zero times infinity is zero, not NaN
inline float mul(float a, float b) {
    float p = a * b;
    return std::isnan(p) && !std::isnan(a) && !std::isnan(b) ? 0.0f : p;
}

} // namespace

void Program::decode(const ShaderUnit &unit) {
    for (uint32_t pc = 0; pc < ops.size(); pc++) {
        uint32_t word = unit.program[pc];
        Decoded &d = ops[pc];
        uint8_t raw = uint8_t(word >> 26);
        d.word = word;
        d.op = raw >= MAD ? MAD : raw >= MADI ? MADI : raw == 0x2F ? CMP : raw;
        uint32_t desc;
        uint32_t src[3] = {};
        uint32_t index = 0;
        if (d.op == MAD || d.op == MADI) {
            desc = unit.descriptors[word & 0x1F];
            d.dest = (word >> 24) & 0x1F;
            src[0] = (word >> 17) & 0x1F;
            if (d.op == MADI) src[1] = (word >> 12) & 0x1F, src[2] = (word >> 5) & 0x7F;
            else src[1] = (word >> 10) & 0x7F, src[2] = (word >> 5) & 0x1F;
            index = (word >> 22) & 3;
            // the address register goes with the wide source
            d.src[1].index = d.op == MAD ? index : 0;
            d.src[2].index = d.op == MADI ? index : 0;
            d.src[0].index = 0;
        } else {
            desc = unit.descriptors[word & 0x7F];
            d.dest = (word >> 21) & 0x1F;
            bool inverted = d.op == DPHI || d.op == DSTI || d.op == SGEI || d.op == SLTI;
            if (inverted) src[0] = (word >> 14) & 0x1F, src[1] = (word >> 7) & 0x7F;
            else src[0] = (word >> 12) & 0x7F, src[1] = (word >> 7) & 0x1F;
            index = (word >> 19) & 3;
            d.src[0].index = inverted ? 0 : index;
            d.src[1].index = inverted ? index : 0;
            d.src[2].index = 0;
        }
        d.mask = desc & 0xF;
        static constexpr int SHIFT[3] = {5, 14, 23};
        for (int s = 0; s < 3; s++) {
            uint32_t pattern = (desc >> SHIFT[s]) & 0xFF;
            d.src[s].reg = uint8_t(src[s]);
            d.src[s].negate = (desc >> (SHIFT[s] - 1)) & 1;
            for (int c = 0; c < 4; c++) d.src[s].swizzle[c] = uint8_t((pattern >> (6 - c * 2)) & 3);
        }
    }
    version = unit.version;
}

Vec4 Runner::read(const Operand &o) const {
    uint32_t reg = o.reg;
    Vec4 v{0, 0, 0, 0};
    if (reg < 0x10) v = input[reg];
    else if (reg < 0x20) v = temp_[reg - 0x10];
    else {
        int32_t uniform = int32_t(reg) - 0x20 + (o.index ? address_[o.index - 1] : 0);
        if (uniform >= 0 && uniform < 96) v = unit_->floats[uniform];
    }
    Vec4 r{v[o.swizzle[0]], v[o.swizzle[1]], v[o.swizzle[2]], v[o.swizzle[3]]};
    if (o.negate) r = {-r[0], -r[1], -r[2], -r[3]};
    return r;
}

void Runner::write(uint32_t reg, uint32_t mask, const Vec4 &v) {
    Vec4 *slot = reg < 0x10 ? &output[reg] : reg < 0x20 ? &temp_[reg - 0x10] : nullptr;
    if (!slot) return;
    // the mask's top bit is x
    for (int c = 0; c < 4; c++) {
        if (mask & (8 >> c)) (*slot)[c] = v[c];
    }
}

bool Runner::condition(uint32_t word) const {
    bool x = cond_[0] == bool((word >> 25) & 1), y = cond_[1] == bool((word >> 24) & 1);
    switch ((word >> 22) & 3) {
    case 0: return x || y;
    case 1: return x && y;
    case 2: return x;
    default: return y;
    }
}

void Runner::run(const ShaderUnit &unit, const Program &program) {
    unit_ = &unit;
    for (Vec4 &o : output) o = {0, 0, 0, 0};
    for (Vec4 &t : temp_) t = {0, 0, 0, 1};
    address_[0] = address_[1] = address_[2] = 0;
    cond_[0] = cond_[1] = false;
    blocks_.clear();
    uint32_t pc = unit.entry;
    for (uint32_t budget = 0; budget < 0x10000; budget++) {
        // blocks ending here: loops go round again, the rest return
        while (!blocks_.empty() && pc == blocks_.back().end) {
            Block &b = blocks_.back();
            address_[2] += b.increment;
            if (b.repeat == 0) {
                pc = b.back;
                blocks_.pop_back();
            } else {
                b.repeat--;
                pc = b.start;
            }
        }
        if (pc >= program.ops.size()) return;
        const Decoded &d = program.ops[pc];
        uint32_t w = d.word, next = pc + 1;
        uint32_t target = (w >> 10) & 0xFFF, count = w & 0xFF;
        auto enter = [&](uint32_t start, uint32_t n, uint32_t back) {
            blocks_.push_back({start + n, back, 0, 0, start, false});
            return start;
        };
        switch (d.op) {
        case END: return;
        case NOP: case EMIT: case SETEMIT: break; // the geometry stage's, not run here
        case CALL: case CALLC: case CALLU: {
            bool go = d.op == CALL || (d.op == CALLU ? bool(unit.bools >> ((w >> 22) & 0xF) & 1) : condition(w));
            if (go) next = enter(target, count, pc + 1);
            break;
        }
        case JMPC: case JMPU: {
            bool go = d.op == JMPC ? condition(w) : bool(unit.bools >> ((w >> 22) & 0xF) & 1) == !(count & 1);
            if (go) next = target;
            break;
        }
        case IFC: case IFU: {
            bool go = d.op == IFC ? condition(w) : bool(unit.bools >> ((w >> 22) & 0xF) & 1);
            // the body to the else block, then past it; or the else block
            next = go ? enter(pc + 1, target > pc + 1 ? target - (pc + 1) : 0, target + count) : enter(target, count, target + count);
            break;
        }
        case LOOP: {
            const auto &i = unit.ints[(w >> 22) & 3];
            address_[2] = i[1];
            blocks_.push_back({target + 1, target + 1, i[0], int8_t(i[2]), pc + 1, true});
            break;
        }
        case BREAK: case BREAKC:
            if (d.op == BREAK || condition(w)) {
                // out of the innermost loop, and whatever is open inside it
                while (!blocks_.empty()) {
                    Block b = blocks_.back();
                    blocks_.pop_back();
                    if (b.loop) {
                        next = b.back;
                        break;
                    }
                }
            }
            break;
        case MAD: case MADI: {
            Vec4 a = read(d.src[0]), b = read(d.src[1]), c = read(d.src[2]);
            write(d.dest, d.mask, {mul(a[0], b[0]) + c[0], mul(a[1], b[1]) + c[1], mul(a[2], b[2]) + c[2], mul(a[3], b[3]) + c[3]});
            break;
        }
        default: {
            Vec4 a = read(d.src[0]), b = read(d.src[1]), r;
            switch (d.op) {
            case ADD: r = {a[0] + b[0], a[1] + b[1], a[2] + b[2], a[3] + b[3]}; break;
            case MUL: r = {mul(a[0], b[0]), mul(a[1], b[1]), mul(a[2], b[2]), mul(a[3], b[3])}; break;
            // as the hardware: max(0, NaN) is NaN, max(NaN, 0) is 0
            case MAX: for (int c = 0; c < 4; c++) r[c] = a[c] > b[c] ? a[c] : b[c]; break;
            case MIN: for (int c = 0; c < 4; c++) r[c] = a[c] < b[c] ? a[c] : b[c]; break;
            case DP3: r.fill(mul(a[0], b[0]) + mul(a[1], b[1]) + mul(a[2], b[2])); break;
            case DP4: r.fill(mul(a[0], b[0]) + mul(a[1], b[1]) + mul(a[2], b[2]) + mul(a[3], b[3])); break;
            case DPH: case DPHI: r.fill(mul(a[0], b[0]) + mul(a[1], b[1]) + mul(a[2], b[2]) + b[3]); break;
            case MOV: r = a; break;
            case FLR: r = {std::floor(a[0]), std::floor(a[1]), std::floor(a[2]), std::floor(a[3])}; break;
            case RCP: r.fill(1.0f / a[0]); break;
            case RSQ: r.fill(1.0f / std::sqrt(a[0])); break;
            case EX2: r.fill(std::exp2(a[0])); break;
            case LG2: r.fill(std::log2(a[0])); break;
            case SGE: case SGEI: for (int c = 0; c < 4; c++) r[c] = a[c] >= b[c] ? 1.0f : 0.0f; break;
            case SLT: case SLTI: for (int c = 0; c < 4; c++) r[c] = a[c] < b[c] ? 1.0f : 0.0f; break;
            case DST: case DSTI: r = {1.0f, mul(a[1], b[1]), a[2], b[3]}; break;
            case LITP:
                cond_[0] = a[0] >= 0, cond_[1] = a[3] >= 0;
                r = {std::max(a[0], 0.0f), std::clamp(a[1], -127.9961f, 127.9961f), 0.0f, std::max(a[3], 0.0f)};
                break;
            case MOVA:
                if (d.mask & 8) address_[0] = int32_t(a[0]);
                if (d.mask & 4) address_[1] = int32_t(a[1]);
                pc = next;
                continue;
            case CMP: {
                auto cmp = [](uint32_t mode, float x, float y) {
                    switch (mode) {
                    case 0: return x == y;
                    case 1: return x != y;
                    case 2: return x < y;
                    case 3: return x <= y;
                    case 4: return x > y;
                    case 5: return x >= y;
                    default: return true;
                    }
                };
                cond_[0] = cmp((w >> 24) & 7, a[0], b[0]);
                cond_[1] = cmp((w >> 21) & 7, a[1], b[1]);
                pc = next;
                continue;
            }
            default: pc = next; continue; // not one the PICA has
            }
            write(d.dest, d.mask, r);
        }
        }
        pc = next;
    }
}

} // namespace runtime::pica
