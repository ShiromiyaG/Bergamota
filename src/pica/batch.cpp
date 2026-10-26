// The vertex shader over 16 vertices at once: each instruction decoded once
// and worked on every lane, in loops the compiler turns into vector code.
// Flow that the lanes disagree on gives the batch back to the one-vertex
// runner (shader.cpp), which PICA shaders seldom need.

#include "shader.h"

#include <cmath>
#include <cstring>

namespace runtime::pica {

namespace {

enum Op : uint8_t {
    ADD = 0x00, DP3 = 0x01, DP4 = 0x02, DPH = 0x03, DST = 0x04, EX2 = 0x05, LG2 = 0x06, LITP = 0x07, MUL = 0x08,
    SGE = 0x09, SLT = 0x0A, FLR = 0x0B, MAX = 0x0C, MIN = 0x0D, RCP = 0x0E, RSQ = 0x0F, MOVA = 0x12, MOV = 0x13,
    DPHI = 0x18, DSTI = 0x19, SGEI = 0x1A, SLTI = 0x1B, BREAK = 0x20, NOP = 0x21, END = 0x22, BREAKC = 0x23,
    CALL = 0x24, CALLC = 0x25, CALLU = 0x26, IFU = 0x27, IFC = 0x28, LOOP = 0x29, EMIT = 0x2A, SETEMIT = 0x2B,
    JMPC = 0x2C, JMPU = 0x2D, CMP = 0x2E, MADI = 0x30, MAD = 0x38,
};

constexpr int N = BATCH;
using Lanes = float[N];
using Wide = Lanes[4];

// the shader's multiply: zero times infinity is zero
inline float mul(float a, float b) {
    float p = a * b;
    return p != p && a == a && b == b ? 0.0f : p;
}

} // namespace

void BatchRunner::read(const Operand &o, Wide &r) const {
    uint32_t reg = o.reg;
    if (reg < 0x20) {
        const Wide &v = reg < 0x10 ? input[reg] : temp_[reg - 0x10];
        for (int c = 0; c < 4; c++) std::memcpy(r[c], v[o.swizzle[c]], sizeof(Lanes));
    } else if (!o.index) {
        Vec4 v = reg - 0x20 < 96 ? unit_->floats[reg - 0x20] : Vec4{0, 0, 0, 0};
        for (int c = 0; c < 4; c++) {
            for (int l = 0; l < N; l++) r[c][l] = v[o.swizzle[c]];
        }
    } else {
        // an address register can differ by vertex (bone palettes)
        for (int l = 0; l < N; l++) {
            int32_t u = int32_t(reg) - 0x20 + address_[o.index - 1][l];
            Vec4 v = u >= 0 && u < 96 ? unit_->floats[u] : Vec4{0, 0, 0, 0};
            for (int c = 0; c < 4; c++) r[c][l] = v[o.swizzle[c]];
        }
    }
    if (o.negate) {
        for (int c = 0; c < 4; c++) {
            for (int l = 0; l < N; l++) r[c][l] = -r[c][l];
        }
    }
}

void BatchRunner::write(uint32_t reg, uint32_t mask, const Wide &v) {
    Wide *slot = reg < 0x10 ? &output[reg] : reg < 0x20 ? &temp_[reg - 0x10] : nullptr;
    if (!slot) return;
    for (int c = 0; c < 4; c++) {
        if (mask & (8 >> c)) std::memcpy((*slot)[c], v[c], sizeof(Lanes));
    }
}

// a condition every lane agrees on, or -1 when they do not
int BatchRunner::condition(uint32_t word) const {
    bool rx = (word >> 25) & 1, ry = (word >> 24) & 1;
    uint32_t op = (word >> 22) & 3;
    int first = -1;
    for (int l = 0; l < N; l++) {
        bool x = cond_[0][l] == rx, y = cond_[1][l] == ry;
        bool c = op == 0 ? x || y : op == 1 ? x && y : op == 2 ? x : y;
        if (first < 0) first = c;
        else if (first != int(c)) return -1;
    }
    return first;
}

bool BatchRunner::run(const ShaderUnit &unit, const Program &program) {
    unit_ = &unit;
    std::memset(output, 0, sizeof output);
    for (auto &t : temp_) {
        for (int c = 0; c < 4; c++) {
            for (int l = 0; l < N; l++) t[c][l] = c == 3 ? 1.0f : 0.0f;
        }
    }
    std::memset(address_, 0, sizeof address_);
    std::memset(cond_, 0, sizeof cond_);
    blocks_.clear();
    uint32_t pc = unit.entry;
    Wide a, b, c, r;
    for (uint32_t budget = 0; budget < 0x10000; budget++) {
        while (!blocks_.empty() && pc == blocks_.back().end) {
            Block &blk = blocks_.back();
            for (int l = 0; l < N; l++) address_[2][l] += blk.increment;
            if (blk.repeat == 0) {
                pc = blk.back;
                blocks_.pop_back();
            } else {
                blk.repeat--;
                pc = blk.start;
            }
        }
        if (pc >= program.ops.size()) return true;
        const Decoded &d = program.ops[pc];
        uint32_t w = d.word, next = pc + 1;
        uint32_t target = (w >> 10) & 0xFFF, count = w & 0xFF;
        auto enter = [&](uint32_t start, uint32_t n, uint32_t back) {
            blocks_.push_back({start + n, back, 0, 0, start, false});
            return start;
        };
        auto flag = [&](int k) { return bool(unit.bools >> ((w >> 22) & 0xF) & 1) ? k : 1 - k; };
        switch (d.op) {
        case END: return true;
        case NOP: case EMIT: case SETEMIT: break;
        case CALL: case CALLC: case CALLU: {
            int go = d.op == CALL ? 1 : d.op == CALLU ? flag(1) : condition(w);
            if (go < 0) return false;
            if (go) next = enter(target, count, pc + 1);
            break;
        }
        case JMPC: case JMPU: {
            int go = d.op == JMPC ? condition(w) : (bool(unit.bools >> ((w >> 22) & 0xF) & 1) == !(count & 1));
            if (go < 0) return false;
            if (go) next = target;
            break;
        }
        case IFC: case IFU: {
            int go = d.op == IFC ? condition(w) : flag(1);
            if (go < 0) return false;
            next = go ? enter(pc + 1, target > pc + 1 ? target - (pc + 1) : 0, target + count) : enter(target, count, target + count);
            break;
        }
        case LOOP: {
            const auto &i = unit.ints[(w >> 22) & 3];
            for (int l = 0; l < N; l++) address_[2][l] = i[1];
            blocks_.push_back({target + 1, target + 1, i[0], int8_t(i[2]), pc + 1, true});
            break;
        }
        case BREAK: case BREAKC: {
            int go = d.op == BREAK ? 1 : condition(w);
            if (go < 0) return false;
            if (go) {
                while (!blocks_.empty()) {
                    Block blk = blocks_.back();
                    blocks_.pop_back();
                    if (blk.loop) {
                        next = blk.back;
                        break;
                    }
                }
            }
            break;
        }
        case MAD: case MADI:
            read(d.src[0], a), read(d.src[1], b), read(d.src[2], c);
            for (int k = 0; k < 4; k++) {
                for (int l = 0; l < N; l++) r[k][l] = mul(a[k][l], b[k][l]) + c[k][l];
            }
            write(d.dest, d.mask, r);
            break;
        default:
            read(d.src[0], a), read(d.src[1], b);
            switch (d.op) {
            case ADD: for (int k = 0; k < 4; k++) for (int l = 0; l < N; l++) r[k][l] = a[k][l] + b[k][l]; break;
            case MUL: for (int k = 0; k < 4; k++) for (int l = 0; l < N; l++) r[k][l] = mul(a[k][l], b[k][l]); break;
            case MAX: for (int k = 0; k < 4; k++) for (int l = 0; l < N; l++) r[k][l] = a[k][l] > b[k][l] ? a[k][l] : b[k][l]; break;
            case MIN: for (int k = 0; k < 4; k++) for (int l = 0; l < N; l++) r[k][l] = a[k][l] < b[k][l] ? a[k][l] : b[k][l]; break;
            case DP3: case DP4: case DPH: case DPHI:
                for (int l = 0; l < N; l++) {
                    float s = mul(a[0][l], b[0][l]) + mul(a[1][l], b[1][l]) + mul(a[2][l], b[2][l]);
                    s += d.op == DP4 ? mul(a[3][l], b[3][l]) : d.op == DP3 ? 0.0f : b[3][l];
                    r[0][l] = r[1][l] = r[2][l] = r[3][l] = s;
                }
                break;
            case MOV: std::memcpy(r, a, sizeof r); break;
            case FLR: for (int k = 0; k < 4; k++) for (int l = 0; l < N; l++) r[k][l] = std::floor(a[k][l]); break;
            case RCP: for (int l = 0; l < N; l++) r[0][l] = r[1][l] = r[2][l] = r[3][l] = 1.0f / a[0][l]; break;
            case RSQ: for (int l = 0; l < N; l++) r[0][l] = r[1][l] = r[2][l] = r[3][l] = 1.0f / std::sqrt(a[0][l]); break;
            case EX2: for (int l = 0; l < N; l++) r[0][l] = r[1][l] = r[2][l] = r[3][l] = std::exp2(a[0][l]); break;
            case LG2: for (int l = 0; l < N; l++) r[0][l] = r[1][l] = r[2][l] = r[3][l] = std::log2(a[0][l]); break;
            case SGE: case SGEI: for (int k = 0; k < 4; k++) for (int l = 0; l < N; l++) r[k][l] = a[k][l] >= b[k][l] ? 1.0f : 0.0f; break;
            case SLT: case SLTI: for (int k = 0; k < 4; k++) for (int l = 0; l < N; l++) r[k][l] = a[k][l] < b[k][l] ? 1.0f : 0.0f; break;
            case DST: case DSTI:
                for (int l = 0; l < N; l++) r[0][l] = 1.0f, r[1][l] = mul(a[1][l], b[1][l]), r[2][l] = a[2][l], r[3][l] = b[3][l];
                break;
            case LITP:
                for (int l = 0; l < N; l++) {
                    cond_[0][l] = a[0][l] >= 0, cond_[1][l] = a[3][l] >= 0;
                    r[0][l] = std::max(a[0][l], 0.0f);
                    r[1][l] = std::clamp(a[1][l], -127.9961f, 127.9961f);
                    r[2][l] = 0.0f;
                    r[3][l] = std::max(a[3][l], 0.0f);
                }
                break;
            case MOVA:
                for (int l = 0; l < N; l++) {
                    if (d.mask & 8) address_[0][l] = int32_t(a[0][l]);
                    if (d.mask & 4) address_[1][l] = int32_t(a[1][l]);
                }
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
                uint32_t mx = (w >> 24) & 7, my = (w >> 21) & 7;
                for (int l = 0; l < N; l++) cond_[0][l] = cmp(mx, a[0][l], b[0][l]), cond_[1][l] = cmp(my, a[1][l], b[1][l]);
                pc = next;
                continue;
            }
            default: pc = next; continue;
            }
            write(d.dest, d.mask, r);
        }
        pc = next;
    }
    return true;
}

} // namespace runtime::pica
