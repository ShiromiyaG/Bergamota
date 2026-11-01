// Loading a game: its exheader and code.bin (as ctrtool extracts them, the
// update applied) mapped as the 3DS's loader maps them, and the code
// 3dsrecomp made for it. Translated from Zakuro's loader.rs and
// zakuro-fs's exheader (Rust, MIT, Copyright (c) 2026 fearkov; see
// LICENSE-zakuro).

#ifndef RUNTIME_LOADER_H
#define RUNTIME_LOADER_H

#include <cstdint>
#include <filesystem>
#include <map>
#include <string>
#include <unordered_map>
#include <vector>

#include "recomp.h"
#include "runtime/cro.h"
#include "runtime/system.h"

namespace runtime {

struct Segment {
    uint32_t address, pages, size;
};

struct ExHeader {
    uint64_t program_id;
    bool compressed;
    Segment text, rodata, data;
    uint32_t stack_size, bss_size;
    uint8_t priority;
    uint32_t app_bytes; // from the system mode
    Region region;
    uint16_t kernel_version;
};

// the places in the executable's code the interpreter ran for want of C
// (System::interpreted_at, odd for Thumb), added to the hints 3dsrecomp
// build reads, <its library folder>/<program id>.hints (APPDATAdsrecomp
// on Windows), as Zakuro writes them: the next recompile takes them in. how
// many were new, or -1 with why in error
// where 3dsrecomp build installs a title's library, <its library folder>/
// <program id>.dll (.so, .dylib), as Zakuro and 3dsrecomp look for it
std::filesystem::path installed_library(uint64_t program_id);

int save_hints(const std::map<uint32_t, uint64_t> &interpreted, const ExHeader &exheader, std::string &error);

// false with why in error when it is not one
bool parse_exheader(const std::vector<uint8_t> &bytes, ExHeader &out, std::string &error);

// the code 3dsrecomp made: the executable's functions and each module's,
// a module's found once it is placed where ldr:ro loaded it
class Library {
public:
    // from a library built apart (3dsrecomp build, zakuro --recompile)
    bool open(const std::string &path, std::string &error);
    // from tables at hand (code linked in, tests); origins may be null
    void linked(const Entry *entries, size_t count, const Module *modules, size_t module_count,
                const Origins *origins);
    // skips the entries of stale functions, which the interpreter runs
    Code lookup(uint32_t address) const;
    // tells the code where module name was loaded; 0 when it goes away.
    // with memory, the module's code there is checked (see check)
    bool place(const std::string &name, uint32_t base, Memory *memory = nullptr);
    // a jump to from goes to to: a module's function the decomp replaces,
    // whose jump the module was not recompiled with and would otherwise run
    // in the interpreter on every call (System::set_redirect with redirect).
    // place drops a module's when it moves or goes away
    void alias(uint32_t from, uint32_t to) { aliases_[from] = to; }
    // where a jump to address goes: to its alias, or itself
    uint32_t redirect(uint32_t address) const;
    // checks the functions of unit (0 the executable, i + 1 module i)
    // against the code in memory, its offsets added to base: those whose
    // bytes changed since it was recompiled (a mod, an update, a jump to the
    // decomp) are stale. how many are
    size_t check(size_t unit, uint32_t base, Memory &memory);
    bool has_module(const std::string &name) const;
    bool has_origins() const { return origins_ != nullptr; }
    uint64_t program_id() const { return program_id_; }
    size_t entry_count() const { return count_; }
    // every function's host code and its name (0x00123456, or a module's
    // Battle.cro+0x1234), for the sampler
    void functions(std::vector<std::pair<uintptr_t, std::string>> &out) const;
    size_t module_count() const { return module_count_; }

private:
    struct Placed {
        uint32_t base, size;
        size_t index;
    };
    bool stale(size_t unit, size_t entry) const;
    uint64_t program_id_ = 0;
    const Entry *entries_ = nullptr;
    size_t count_ = 0;
    const Module *modules_ = nullptr;
    size_t module_count_ = 0;
    // one per unit, from generation 3 on
    const Origins *origins_ = nullptr;
    std::vector<Placed> placed_;
    std::unordered_map<uint32_t, uint32_t> aliases_;
    // the last answers of lookup, by address: the game jumps through it on
    // every indirect call and return, and the search was a third of its time
    // in the intro's movie. emptied whenever an answer can change
    struct Cached {
        uint32_t address = ~0u;
        Code code = nullptr;
    };
    static constexpr size_t CACHE_SIZE = 1 << 16;
    mutable std::vector<Cached> cache_;
    void forget() const { cache_.assign(CACHE_SIZE, Cached{}); }
    Code search(uint32_t address) const;
};

struct Decomp;

// maps the code, the bss and the main stack, and starts the main thread;
// with a decomp, the jumps to it written into the text and its segment
// mapped
void load(System &system, const ExHeader &exheader, const std::vector<uint8_t> &code,
          const Decomp *decomp = nullptr);
// after load: the executable's code checked against the library, and the
// decomp's recompiled code placed at its base; false when the library was
// not recompiled with the decomp
bool link_code(Library &library, Memory &memory, const Decomp *decomp, std::string &error);
// ldr:ro loaded module name at base, or unloads it (base 0): the decomp's
// jumps and addresses in it first, then the library told and the module's
// code checked, so a function hooked there runs in the interpreter and jumps
void module_changed(Library &library, Memory &memory, const Decomp *decomp, const CroLoader &cro,
                    const std::string &name, uint32_t base);

// the main stack ends where the shared memory region starts, as on the 3DS
constexpr uint32_t STACK_TOP = map::SHARED_MEMORY_VADDR;

} // namespace runtime

#endif
