// Functions written by hand to run instead of the ones the recompiler
// generates, found in C files by the RECOMP_OVERRIDE marks recomp.h gives
// them. Translated from 3dsrecomp's overrides.rs (Rust, MIT, Copyright (c)
// 2026 fearkov; see LICENSE-3dsrecomp).

#ifndef RECOMP_OVERRIDES_H
#define RECOMP_OVERRIDES_H

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace recomp {

// one function written by hand
struct Override {
    // the module whose function it replaces, none for the executable
    std::optional<std::string> module;
    // what it replaces, an address or an offset in the module, odd for Thumb
    uint32_t address = 0;
    // the C function, as the mark names it
    std::string name;
    // the name the generated function it replaces goes by for it
    std::string original;
    bool operator==(const Override &) const = default;
};

// a C file of overrides
struct OverrideFile {
    std::filesystem::path path;
    std::string source;
    std::vector<Override> overrides;
};

// the C files at path, a file or a folder of them (sorted); false with why
bool load_overrides(const std::filesystem::path &path, std::vector<OverrideFile> &out, std::string &error);
// the overrides a C source marks; false with why
bool parse_overrides(const std::string &source, std::vector<Override> &out, std::string &error);

} // namespace recomp

#endif
