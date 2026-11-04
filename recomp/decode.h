// Just enough ARM and Thumb decoding to follow control flow: every
// instruction that does not change where execution goes, or point at data
// inside the code, is Other. Translated from 3dsrecomp's arm.rs and thumb.rs
// (Rust, MIT, Copyright (c) 2026 fearkov; see LICENSE-3dsrecomp).

#ifndef RECOMP_DECODE_H
#define RECOMP_DECODE_H

#include <cstdint>
#include <optional>
#include <utility>

namespace recomp {

// the condition field value meaning always
constexpr uint32_t ALWAYS = 0xE;

// Rust's enum Flow, its fields in one struct: only those of kind are set
struct Flow {
    enum Kind {
        Branch,          // b or bl to a known address: target, link
        CallOtherMode,   // blx to a known address, switching state: target
        BranchRegister,  // bx or blx through a register: reg, link
        Return,          // bx lr, pop with pc, mov pc lr and the like
        JumpTable,       // add pc, pc, rm lsl 2, followed by a table of branches
        AddressTable,    // ldr pc, [pc, rm lsl 2], followed by addresses: index
        IndirectJump,    // any other write to pc not known statically
        LoadPcLiteral,   // ldr pc from a literal: literal
        LiteralLoad,     // a load from a literal pool in the code: literal, size
        Svc,
        Undefined,       // cannot be code: the path that reached it is wrong
        Other,
    } kind = Other;
    uint32_t target = 0;
    bool link = false;
    uint32_t reg = 0;   // BranchRegister's register
    uint32_t index = 0; // AddressTable's
    uint32_t literal = 0, size = 0;

    bool operator==(const Flow &) const = default;
};

struct Instruction {
    uint32_t condition = ALWAYS;
    Flow flow;
    bool is_conditional() const { return condition != ALWAYS; }
    bool operator==(const Instruction &) const = default;
};

namespace arm {
// the register and immediate of cmp rn, imm, used to size switch tables
std::optional<std::pair<uint32_t, uint32_t>> compare_immediate(uint32_t word);
Instruction decode(uint32_t word, uint32_t address);
} // namespace arm

namespace thumb {
// the instruction at address and its size in bytes; next is the halfword
// after it, which only the bl and blx pairs read
std::pair<Instruction, uint32_t> decode(uint16_t halfword, std::optional<uint16_t> next, uint32_t address);
} // namespace thumb

} // namespace recomp

#endif
