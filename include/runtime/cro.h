// CRO modules: what ldr:ro does with them, rebasing, relocating and linking
// against the static module (CRS) and the modules already loaded. After
// Zakuro's cro/ (Rust, MIT, Copyright (c) 2026 fearkov; see LICENSE-zakuro),
// leaner: no save states, no diagnostics.

#ifndef RUNTIME_CRO_H
#define RUNTIME_CRO_H

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "runtime/memory.h"

namespace runtime {

class CroLoader {
public:
    struct Module {
        std::string name;
        uint32_t base, size;
        bool auto_link;
        std::map<std::string, uint32_t> exports;
        std::vector<uint32_t> indexed_exports, segments;
    };

    // ldr:ro Initialize: the static module, whose exports the others link to
    bool initialize(Memory &memory, uint32_t base, uint32_t size);
    // ldr:ro LoadCRO: the size fixing left, nullopt when it is not a CRO
    std::optional<uint32_t> load(Memory &memory, uint32_t base, uint32_t size, uint32_t data, uint32_t data_size,
                                 uint32_t bss, uint32_t bss_size, bool auto_link, uint32_t fix_level);
    void unload(Memory &memory, uint32_t base);

    const std::vector<Module> &modules() const { return modules_; }

private:
    bool rebase(Memory &memory, uint32_t base, uint32_t size, uint32_t data, uint32_t data_size, uint32_t bss,
                uint32_t bss_size, bool is_static, uint32_t &old_data);
    Module describe(Memory &memory, uint32_t base, uint32_t size, bool auto_link);
    void link(Memory &memory, const Module &module);
    void link_others_to(Memory &memory, const Module &module);
    void reset_imports(Memory &memory, uint32_t base);

    std::optional<uint32_t> crs_;
    std::vector<Module> modules_; // the CRS first
};

} // namespace runtime

#endif
