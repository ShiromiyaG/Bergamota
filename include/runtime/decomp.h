// The decomp: C written apart from the game, compiled for its ARM11 and
// linked at an address of its own (decomp-tools' build.py writes
// decomp.bin and decomp.txt), that replaces some of the game's functions.
// Each replaced function starts with a jump to it: those of the executable
// from the load on, a module's as the module loads. 3dsrecomp --decomp
// recompiles it as the module "decomp". Translated from Zakuro's decomp.rs
// (Rust, MIT, Copyright (c) 2026 fearkov; see LICENSE-zakuro).

#ifndef RUNTIME_DECOMP_H
#define RUNTIME_DECOMP_H

#include <cstdint>
#include <map>
#include <string>
#include <utility>
#include <vector>

#include "runtime/cro.h"
#include "runtime/decomp_options.h"
#include "runtime/memory.h"

namespace runtime {

// the name its recompiled code is known by, as a module's is
constexpr const char *DECOMP_MODULE = "decomp";

// the least the decomp's segment takes, whatever its size now
constexpr uint32_t DECOMP_ROOM = 0x100000;

// a function of a module the decomp replaces: its offset from where the
// module loads (odd for Thumb), the code that takes over, what it returns
struct ModuleHook {
    std::string module;
    uint32_t offset, target;
    std::string kind;
};

// an address in a module the decomp uses, written to the word slot once the
// module loads: offset from the module's start, or from segment's
struct ModuleImport {
    std::string module;
    uint32_t offset, slot;
    int segment; // -1: from where the module loads
};

struct Decomp {
    uint32_t base = 0;
    std::vector<uint8_t> code;
    // the executable's functions replaced and the code that takes over, the
    // first odd for Thumb
    std::vector<std::pair<uint32_t, uint32_t>> hooks;
    // what each replaced function returns, by its address
    std::map<uint32_t, std::string> returns;
    std::vector<ModuleHook> module_hooks;
    std::vector<ModuleImport> module_imports;
    // variables of the decomp's the host reads or writes, by name
    std::map<std::string, uint32_t> symbols;
    // words of the executable's changed as it loads (a vtable's entry
    // pointed at a port's wrapper): address, value
    std::vector<std::pair<uint32_t, uint32_t>> patches;
    // the port's options, in the order the code has them
    std::vector<DecompOption> options;
};

// decomp.txt's lines: base, entry, hook, module_hook, module_import, symbol,
// patch, option
bool parse_decomp(const std::string &text, Decomp &out, std::string &error);
// dir/decomp.bin and dir/decomp.txt, dir in UTF-8
bool read_decomp(const std::string &dir, Decomp &out, std::string &error);

// the jump over a function's first instructions: ldr pc, [pc, #-4] and the
// address, from Thumb a bx pc first, which lands on the next whole word
std::vector<uint8_t> decomp_stub(uint32_t old, uint32_t target);
// the jumps written into the text segment's bytes, which start at address
// text; how many hooks fell outside it and were left alone
size_t patch_text(const Decomp &decomp, uint32_t text, std::vector<uint8_t> &bytes);
// an address in a module loaded at base with those segments
uint32_t module_address(uint32_t base, const std::vector<uint32_t> &segments, uint32_t offset, int segment);

// the decomp's segment mapped at its base, its code in it
bool map_decomp(Memory &memory, Region region, const Decomp &decomp, std::string &error);
// a module was loaded: the addresses the decomp uses in it, and the jumps
// over the functions it replaces; how many functions were hooked
size_t hook_module(Memory &memory, const Decomp &decomp, const CroLoader::Module &module);
// a module is going away: a call into it from the decomp goes nowhere
void unhook_module(Memory &memory, const Decomp &decomp, const std::string &name);

} // namespace runtime

#endif
