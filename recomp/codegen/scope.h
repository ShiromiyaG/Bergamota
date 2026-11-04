// What the files of codegen/ share: what the lowering of one instruction may
// refer to, the emit helper, calls, jumps and the words of a transfer, and
// the pieces of recomp_abi the tables use. Translated from 3dsrecomp's
// codegen/mod.rs and abi/src/lib.rs (Rust, MIT, Copyright (c) 2026 fearkov;
// see LICENSE-3dsrecomp).

#ifndef RECOMP_CODEGEN_SCOPE_H
#define RECOMP_CODEGEN_SCOPE_H

#include <cstddef>
#include <cstdint>
#include <format>
#include <iterator>
#include <map>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace recomp::codegen {

// writeln! into a string: the format strings are Rust's, which std::format
// reads the same way for what they use ({}, {:X}, {:08X}, {:03}, {{ and }})
template <class... Args>
inline void emit(std::string &out, std::format_string<Args...> format, Args &&...args) {
    std::format_to(std::back_inserter(out), format, std::forward<Args>(args)...);
    out.push_back('\n');
}

// recomp_abi's: the code generator's version, raised when the code it makes
// runs much better or does more than before
constexpr uint32_t GENERATION = 4;
// an entry's owner when code written by hand runs it
constexpr uint32_t NO_ORIGIN = UINT32_MAX;
// the start a stale function gets, which nothing starts at
constexpr uint32_t NO_START = UINT32_MAX;

// the hash of the bytes a function was made from, 64-bit FNV-1a over its
// spans one after another
struct CodeHash {
    uint64_t hash = 0xCBF29CE484222325ull;
    void add(const uint8_t *bytes, size_t size) {
        for (size_t i = 0; i < size; i++) hash = (hash ^ bytes[i]) * 0x00000100000001B3ull;
    }
    uint64_t value() const { return hash; }
};

// the conditions 0 to 13 as recomp.h spells them
inline constexpr const char *CONDITIONS[14] = {
    "C_EQ", "C_NE", "C_CS", "C_CC", "C_MI", "C_PL", "C_VS", "C_VC", "C_HI", "C_LS", "C_GE", "C_LT", "C_GT", "C_LE",
};

// what the lowering of one instruction may refer to
struct Scope {
    // the labels of the function being written
    const std::set<uint32_t> &labels;
    // the C function that runs each function, by entry with bit 0 set for
    // Thumb
    const std::map<uint32_t, std::string> &functions;
    // what the names of the functions start with, which keeps modules apart
    const std::string &prefix;
    // whether the code moves, so that addresses are offsets from where the
    // host loaded it
    bool relative;
    // the instructions left to the interpreter, which can end the code's
    // run, so that it gets entered again right after them
    std::vector<uint32_t> &interpreted;

    // an address as C
    std::string at(uint32_t address) const {
        if (relative) return std::format("(module_base + 0x{:X}u)", address);
        return std::format("0x{:08X}u", address);
    }

    // the C function that runs the function at target, if there is one
    const std::string *function(uint32_t target, bool thumb) const {
        auto found = functions.find(target | uint32_t(thumb));
        return found == functions.end() ? nullptr : &found->second;
    }
};

// a function's C name
std::string name(const std::string &prefix, uint32_t entry, bool thumb);

// a call, with link the return address as lr holds it, bit 0 set when the
// caller is Thumb
bool call(std::string &out, const Scope &scope, uint32_t link, uint32_t target, bool thumb);

// the words a load or store multiple moves, from p up, a destination for
// each when loading and a value for each when storing, both as C
void words(std::string &out, bool load, const std::vector<std::string> &words);

// a branch without link, to a label of this function if it is one
bool jump(std::string &out, const Scope &scope, uint32_t target, bool thumb);

// the pieces spans cut the code into, in order and without overlaps
std::vector<std::pair<uint32_t, uint32_t>> pieces(std::vector<std::pair<uint32_t, uint32_t>> spans);

// each function's number: the one it had before, else the next free one
std::map<uint32_t, uint32_t> numbered(const std::map<uint32_t, uint32_t> *before, const std::vector<uint32_t> &entries);

namespace arm {
// writes the C for the instruction at address, returning whether execution
// can carry on to the next one
bool lower(std::string &out, const Scope &scope, uint32_t address, uint32_t op);
} // namespace arm

namespace thumb {
// writes the C for the instruction at address, second being the halfword
// after it, returning whether execution can carry on to the next one
bool lower(std::string &out, const Scope &scope, uint32_t a, uint32_t op, uint32_t second);
} // namespace thumb

namespace vfp {
bool data_processing(std::string &out, const Scope &scope, uint32_t a, uint32_t op);
bool register_transfer(std::string &out, const Scope &scope, uint32_t a, uint32_t op);
bool load_store(std::string &out, const Scope &scope, uint32_t a, uint32_t op);
} // namespace vfp

namespace locals {
// what puts the default SYNC_OUT, SYNC_IN, BUDGET_LEFT, page tables and
// fpscr back after a function, for whatever follows it in the file
extern const std::string_view RESET;
// the body with the registers, flags and fpscr it uses in locals
std::string keep(std::string_view body);
} // namespace locals

} // namespace recomp::codegen

#endif
