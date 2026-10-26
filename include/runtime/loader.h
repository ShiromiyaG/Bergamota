// Loading a game: its exheader and code.bin (as ctrtool extracts them, the
// update applied) mapped as the 3DS's loader maps them, and the code
// 3dsrecomp made for it. Translated from Zakuro's loader.rs and
// zakuro-fs's exheader (Rust, MIT, Copyright (c) 2026 fearkov; see
// LICENSE-zakuro).

#ifndef RUNTIME_LOADER_H
#define RUNTIME_LOADER_H

#include <cstdint>
#include <string>
#include <vector>

#include "recomp.h"
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

// false with why in error when it is not one
bool parse_exheader(const std::vector<uint8_t> &bytes, ExHeader &out, std::string &error);

// the code 3dsrecomp made: the executable's functions and each module's,
// a module's found once it is placed where ldr:ro loaded it
class Library {
public:
    // from a library built apart (3dsrecomp build, zakuro --recompile)
    bool open(const std::string &path, std::string &error);
    Code lookup(uint32_t address) const;
    // tells the code where module name was loaded; 0 when it goes away
    bool place(const std::string &name, uint32_t base);
    uint64_t program_id() const { return program_id_; }
    size_t entry_count() const { return count_; }
    size_t module_count() const { return module_count_; }

private:
    struct Placed {
        uint32_t base, size;
        size_t index;
    };
    uint64_t program_id_ = 0;
    const Entry *entries_ = nullptr;
    size_t count_ = 0;
    const Module *modules_ = nullptr;
    size_t module_count_ = 0;
    std::vector<Placed> placed_;
};

// maps the code, the bss and the main stack, and starts the main thread
void load(System &system, const ExHeader &exheader, const std::vector<uint8_t> &code);

// the main stack ends where the shared memory region starts, as on the 3DS
constexpr uint32_t STACK_TOP = map::SHARED_MEMORY_VADDR;

} // namespace runtime

#endif
