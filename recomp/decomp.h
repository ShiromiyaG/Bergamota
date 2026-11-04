// Code written apart from the title, compiled for its ARM11 and linked at a
// fixed address of its own, which replaces some of the title's functions. It
// is recompiled like a module the title loaded at that address, and each
// function it replaces gets a jump to the new code in place of its first
// instructions. Translated from 3dsrecomp's decomp.rs (Rust, MIT, Copyright
// (c) 2026 fearkov; see LICENSE-3dsrecomp).

#ifndef RECOMP_DECOMP_H
#define RECOMP_DECOMP_H

#include <cstdint>
#include <filesystem>
#include <string>
#include <utility>
#include <vector>

#include "discover.h"

namespace recomp {

struct Decomp {
    // the name its recompiled code is known by, as a module's is
    static constexpr const char *NAME = "decomp";

    // where the code is loaded and runs
    uint32_t base = 0;
    std::vector<uint8_t> code;
    // the functions in it, odd for Thumb
    std::vector<uint32_t> entries;
    // the title's functions it replaces and the code that takes over, the
    // first odd when it is Thumb
    std::vector<std::pair<uint32_t, uint32_t>> hooks;

    // reads dir/decomp.bin and dir/decomp.txt, which say `base 0x...`, and
    // `entry 0x...` and `hook 0x<old> 0x<new>` on lines of their own; the
    // lines for the host (module_hook, module_import, symbol, patch, option)
    // are let through. false with why
    static bool read(const std::filesystem::path &dir, Decomp &out, std::string &error);
    // the jump that replaces a function's first instructions: to the new
    // code in ARM state, from either state. an ARM function's is
    // `ldr pc, [pc, #-4]` and the address, a Thumb function's switches first
    // with `bx pc`, which lands on the next whole word.
    static std::vector<uint8_t> stub(uint32_t old, uint32_t replacement);
    // writes the jumps over the functions in text, the executable's code
    void patch(Segment &text) const;
    // the code as a module's program: offsets from where it is loaded
    Program program() const;
};

} // namespace recomp

#endif
