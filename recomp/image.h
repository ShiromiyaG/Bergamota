// The executable image of a title, split into its segments at the addresses
// the title expects to run at. Translated from 3dsrecomp's image.rs (Rust,
// MIT, Copyright (c) 2026 fearkov; see LICENSE-3dsrecomp).

#ifndef RECOMP_IMAGE_H
#define RECOMP_IMAGE_H

#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace recomp {

struct Program;
struct ExHeader;
class Title;

// bytes at an address
struct Segment {
    uint32_t base = 0;
    std::vector<uint8_t> bytes;

    uint32_t end() const { return base + uint32_t(bytes.size()); }
    bool contains(uint32_t address) const { return address >= base && address < end(); }
    std::optional<uint32_t> read32(uint32_t address) const;
    std::optional<uint16_t> read16(uint32_t address) const;
    // address has to be inside, as in the Rust
    void write32(uint32_t address, uint32_t value);
    // every aligned word in the segment, with its address
    std::vector<std::pair<uint32_t, uint32_t>> words() const;
};

struct Image {
    uint32_t entry = 0;
    Segment text, rodata, data;

    // the title's own code; false with why
    static bool from_title(const Title &title, Image &out, std::string &error);
    // the image of code, decompressed, at the addresses header gives: the
    // segments one after another, each padded to whole pages
    static Image from_code(const ExHeader &header, const std::vector<uint8_t> &code);
    // the executable as discovery sees it, from the entry point, the exports
    // and what modules take from it without a name; any word of the data
    // segments that lands in the code is taken for a pointer
    Program into_program(const std::vector<uint32_t> &exports, const std::vector<uint32_t> &imported) const;
};

} // namespace recomp

#endif
