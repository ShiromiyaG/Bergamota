// Reading CRO modules and the CRS that describes the main executable,
// straight from their files. Translated from 3dsrecomp's cro.rs (Rust, MIT,
// Copyright (c) 2026 fearkov; see LICENSE-3dsrecomp).

#ifndef RECOMP_CRO_H
#define RECOMP_CRO_H

#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "discover.h"

namespace recomp::cro {

// the segment types holding code and holding zeros
constexpr uint32_t CODE = 0;
constexpr uint32_t BSS = 3;

struct Segment {
    uint32_t offset = 0;
    uint32_t size = 0;
    uint32_t kind = 0;
};

struct Relocation {
    // where the address goes, as a segment tag
    uint32_t target = 0;
    uint8_t kind = 0;
    // the segment the address points into
    uint32_t segment = 0;
    uint32_t addend = 0;

    // whether the relocation stores the address itself rather than an
    // offset or a branch
    bool is_absolute() const { return kind == 2 || kind == 38; }
};

struct Module {
    std::string name;
    std::vector<Segment> segments;
    // exported symbols by name, as segment tags
    std::vector<std::pair<std::string, uint32_t>> exports;
    // exported symbols by index, as segment tags
    std::vector<uint32_t> indexed_exports;
    // the addresses the module writes into itself when it is loaded
    std::vector<Relocation> relocations;
    // the places that get the addresses of what it imports, whose segment
    // field means nothing
    std::vector<Relocation> imports;
    // what it takes from other modules without a name, the module and the
    // segment tag there
    std::vector<std::pair<std::string, uint32_t>> anonymous_imports;

    // the addresses of the exported symbols that sit in code
    std::vector<uint32_t> code_exports() const;
    // the address a segment tag points at when it is in code
    std::optional<uint32_t> code_address(uint32_t tag) const;
    // the address a segment tag points at, given where each segment lives
    std::optional<uint32_t> resolve(uint32_t tag) const;
    // the module as it looks loaded at base, its bss after the file and its
    // relocations applied, for running it without a loader. everything it
    // imports points at stub. (the Rust's, behind its verify feature)
    std::vector<uint8_t> image(const std::vector<uint8_t> &bytes, uint32_t base, uint32_t stub) const;
    bool is_code(uint32_t segment) const;
    // the module's code as discovery sees it, at its offsets in the file
    // with the code addresses its relocations store filled in. bytes is the
    // whole file, where the stored addresses are still zero, and imported
    // the code addresses other modules take from it.
    std::optional<Program> program(const std::vector<uint8_t> &bytes, const std::vector<uint32_t> &imported) const;
};

std::optional<Module> parse(const std::vector<uint8_t> &bytes);

} // namespace recomp::cro

#endif
