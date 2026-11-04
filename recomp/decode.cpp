// Just enough ARM and Thumb decoding to follow control flow. Translated from
// 3dsrecomp's arm.rs and thumb.rs (Rust, MIT, Copyright (c) 2026 fearkov;
// see LICENSE-3dsrecomp).

#include "decode.h"

namespace recomp {

namespace {

constexpr uint32_t PC = 15;
constexpr uint32_t LR = 14;
constexpr uint32_t SP = 13;

Flow of(Flow::Kind kind) {
    Flow flow;
    flow.kind = kind;
    return flow;
}

Flow branch(uint32_t target, bool link) {
    Flow flow = of(Flow::Branch);
    flow.target = target;
    flow.link = link;
    return flow;
}

Flow call_other_mode(uint32_t target) {
    Flow flow = of(Flow::CallOtherMode);
    flow.target = target;
    return flow;
}

Flow branch_register(uint32_t reg, bool link) {
    Flow flow = of(Flow::BranchRegister);
    flow.reg = reg;
    flow.link = link;
    return flow;
}

Flow address_table(uint32_t index) {
    Flow flow = of(Flow::AddressTable);
    flow.index = index;
    return flow;
}

Flow load_pc_literal(uint32_t literal) {
    Flow flow = of(Flow::LoadPcLiteral);
    flow.literal = literal;
    return flow;
}

Flow literal_load(uint32_t literal, uint32_t size) {
    Flow flow = of(Flow::LiteralLoad);
    flow.literal = literal;
    flow.size = size;
    return flow;
}

uint32_t rotate_right(uint32_t value, uint32_t amount) {
    amount &= 31;
    return amount == 0 ? value : (value >> amount) | (value << (32 - amount));
}

} // namespace

namespace arm {

namespace {

int32_t sign_extend_24(uint32_t value) { return int32_t(value << 8) >> 8; }

// ldr and str with a word or byte
Flow single_transfer(uint32_t word, uint32_t pc, uint32_t rd, uint32_t rn) {
    bool load = (word & (1u << 20)) != 0;
    bool register_offset = (word & (1u << 25)) != 0;
    if (!load) return of(Flow::Other);
    // ldr pc, [pc, rm lsl 2] with an immediate shift and no writeback
    if (rd == PC && rn == PC && register_offset && (word & 0x01700FF0) == 0x01100100) return address_table(word & 0xF);
    if (rn == PC && !register_offset && (word & (1u << 24)) != 0) {
        uint32_t offset = word & 0xFFF;
        uint32_t literal = (word & (1u << 23)) != 0 ? pc + offset : pc - offset;
        uint32_t size = (word & (1u << 22)) != 0 ? 1 : 4;
        return rd == PC ? load_pc_literal(literal) : literal_load(literal, size);
    }
    if (rd == PC) {
        // ldr pc, [sp], 4 is a pop
        return rn == SP && (word & (1u << 24)) == 0 ? of(Flow::Return) : of(Flow::IndirectJump);
    }
    return of(Flow::Other);
}

Flow data_processing(uint32_t word, uint32_t rd, uint32_t rn) {
    bool immediate = (word & (1u << 25)) != 0;
    // multiplies and the extra loads and stores share this space
    if (!immediate && (word & 0x90) == 0x90) return of(Flow::Other);
    uint32_t opcode = (word >> 21) & 0xF;
    bool sets_flags = (word & (1u << 20)) != 0;
    // tst teq cmp cmn write no register, and without s they are misc instructions
    if ((opcode >= 8 && opcode <= 11) || rd != PC) return of(Flow::Other);
    uint32_t rm = word & 0xF;
    uint32_t shift = (word >> 4) & 0xFF;
    // mov pc, lr
    if (opcode == 0xD && !immediate && rm == LR && shift == 0) return of(Flow::Return);
    // add pc, pc, rm lsl 2
    if (opcode == 0x4 && !immediate && rn == PC && shift == 0x10 && !sets_flags) return of(Flow::JumpTable);
    return of(Flow::IndirectJump);
}

} // namespace

std::optional<std::pair<uint32_t, uint32_t>> compare_immediate(uint32_t word) {
    if ((word & 0x0FF0F000) != 0x03500000) return std::nullopt;
    uint32_t rotate = ((word >> 8) & 0xF) * 2;
    return std::pair<uint32_t, uint32_t>((word >> 16) & 0xF, rotate_right(word & 0xFF, rotate));
}

Instruction decode(uint32_t word, uint32_t address) {
    uint32_t condition = word >> 28;
    uint32_t pc = address + 8;
    uint32_t rd = (word >> 12) & 0xF;
    uint32_t rn = (word >> 16) & 0xF;

    Flow flow;
    if (condition == 0xF) {
        // the unconditional space, only blx immediate matters here
        if ((word & 0x0E000000) == 0x0A000000) {
            uint32_t half = (word >> 24) & 1;
            int32_t offset = int32_t(uint32_t(sign_extend_24(word & 0x00FFFFFF)) << 2) + int32_t(half << 1);
            flow = call_other_mode(pc + uint32_t(offset));
        } else {
            flow = of(Flow::Other);
        }
    } else if ((word & 0x0E000000) == 0x0A000000) {
        int32_t offset = int32_t(uint32_t(sign_extend_24(word & 0x00FFFFFF)) << 2);
        flow = branch(pc + uint32_t(offset), (word & (1u << 24)) != 0);
    } else if ((word & 0x0FFFFFD0) == 0x012FFF10) {
        uint32_t reg = word & 0xF;
        bool link = (word & (1u << 5)) != 0;
        flow = reg == LR && !link ? of(Flow::Return) : branch_register(reg, link);
    } else if ((word & 0x0F000000) == 0x0F000000) {
        flow = of(Flow::Svc);
    } else if ((word & 0x0C000000) == 0x04000000) {
        flow = single_transfer(word, pc, rd, rn);
    } else if ((word & 0x0E000000) == 0x08000000) {
        // ldm with pc in the list
        bool load = (word & (1u << 20)) != 0;
        if (load && (word & (1u << 15)) != 0)
            flow = rn == SP ? of(Flow::Return) : of(Flow::IndirectJump);
        else
            flow = of(Flow::Other);
    } else if ((word & 0x0F300E00) == 0x0D100A00 && rn == PC) {
        // vldr from a literal
        uint32_t offset = (word & 0xFF) * 4;
        uint32_t literal = (word & (1u << 23)) != 0 ? (pc & ~3u) + offset : (pc & ~3u) - offset;
        uint32_t size = (word & (1u << 8)) != 0 ? 8 : 4;
        flow = literal_load(literal, size);
    } else if ((word & 0x0C000000) == 0) {
        flow = data_processing(word, rd, rn);
    } else {
        flow = of(Flow::Other);
    }
    return Instruction{condition, flow};
}

} // namespace arm

namespace thumb {

namespace {

constexpr uint16_t THUMB_LR = 14;

int32_t sign_extend(uint32_t value, uint32_t bits) { return int32_t(value << (32 - bits)) >> (32 - bits); }

Instruction always(Flow flow) { return Instruction{ALWAYS, flow}; }

Flow other(uint16_t halfword) {
    // pop with pc
    if ((halfword & 0xFF00) == 0xBD00) return of(Flow::Return);
    // bx and blx through a register
    if ((halfword & 0xFF07) == 0x4700) {
        uint32_t reg = uint32_t((halfword >> 3) & 0xF);
        bool link = (halfword & 0x80) != 0;
        return uint16_t(reg) == THUMB_LR && !link ? of(Flow::Return) : branch_register(reg, link);
    }
    // mov and add on high registers with pc as the destination
    if ((halfword & 0xFC87) == 0x4487) {
        uint16_t source = (halfword >> 3) & 0xF;
        bool mov = (halfword & 0x0300) == 0x0200;
        return mov && source == THUMB_LR ? of(Flow::Return) : of(Flow::IndirectJump);
    }
    return of(Flow::Other);
}

} // namespace

// the ARM11 is ARMv6K, so this is Thumb-1, where only the bl and blx pairs
// take 32 bits
std::pair<Instruction, uint32_t> decode(uint16_t halfword, std::optional<uint16_t> next, uint32_t address) {
    uint32_t pc = address + 4;
    uint32_t word = halfword;

    switch (halfword >> 11) {
    // conditional branch, with svc and undefined in the last two conditions
    case 0b11010:
    case 0b11011: {
        uint32_t condition = (word >> 8) & 0xF;
        Flow flow;
        if (condition == 0xF)
            flow = of(Flow::Svc);
        else if (condition == 0xE)
            flow = of(Flow::Undefined);
        else
            flow = branch(pc + uint32_t(int32_t(uint32_t(sign_extend(word & 0xFF, 8)) << 1)), false);
        return {Instruction{condition, flow}, 2};
    }
    case 0b11100: {
        uint32_t target = pc + uint32_t(int32_t(uint32_t(sign_extend(word & 0x7FF, 11)) << 1));
        return {always(branch(target, false)), 2};
    }
    // the first half of bl or blx carries the upper offset
    case 0b11110: {
        int32_t high = int32_t(uint32_t(sign_extend(word & 0x7FF, 11)) << 12);
        if (!next) return {always(of(Flow::Undefined)), 2};
        uint16_t second = *next;
        int32_t low = int32_t(second & 0x7FF) << 1;
        uint32_t target = pc + uint32_t(high + low);
        switch (second >> 11) {
        case 0b11111: return {always(branch(target, true)), 4};
        case 0b11101: return {always(call_other_mode(target & ~3u)), 4};
        default: return {always(of(Flow::Undefined)), 2};
        }
    }
    // a lone second half means the decoding lost track
    case 0b11111:
    case 0b11101: return {always(of(Flow::Undefined)), 2};
    // ldr rd, [pc, imm]
    case 0b01001: {
        uint32_t literal = (pc & ~3u) + (word & 0xFF) * 4;
        return {always(literal_load(literal, 4)), 2};
    }
    default: return {always(other(halfword)), 2};
    }
}

} // namespace thumb

} // namespace recomp
