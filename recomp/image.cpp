// The executable image of a title, split into its segments at the addresses
// the title expects to run at. Translated from 3dsrecomp's image.rs (Rust,
// MIT, Copyright (c) 2026 fearkov; see LICENSE-3dsrecomp).

#include "image.h"

#include <cassert>
#include <cstring>

#include "discover.h"
#include "rom.h"

namespace recomp {

namespace {

constexpr size_t PAGE_SIZE = 0x1000;

uint32_t le32(const uint8_t *bytes) {
    return uint32_t(bytes[0]) | uint32_t(bytes[1]) << 8 | uint32_t(bytes[2]) << 16 | uint32_t(bytes[3]) << 24;
}

} // namespace

std::optional<uint32_t> Segment::read32(uint32_t address) const {
    if (address < base) return std::nullopt;
    size_t offset = address - base;
    if (offset + 4 > bytes.size()) return std::nullopt;
    return le32(bytes.data() + offset);
}

void Segment::write32(uint32_t address, uint32_t value) {
    size_t offset = uint32_t(address - base);
    assert(offset + 4 <= bytes.size());
    for (int i = 0; i < 4; i++) bytes[offset + i] = uint8_t(value >> (8 * i));
}

std::optional<uint16_t> Segment::read16(uint32_t address) const {
    if (address < base) return std::nullopt;
    size_t offset = address - base;
    if (offset + 2 > bytes.size()) return std::nullopt;
    return uint16_t(bytes[offset] | bytes[offset + 1] << 8);
}

std::vector<std::pair<uint32_t, uint32_t>> Segment::words() const {
    std::vector<std::pair<uint32_t, uint32_t>> out;
    out.reserve(bytes.size() / 4);
    for (size_t i = 0; i + 4 <= bytes.size(); i += 4) out.emplace_back(base + uint32_t(i), le32(bytes.data() + i));
    return out;
}

bool Image::from_title(const Title &title, Image &out, std::string &error) {
    std::vector<uint8_t> code;
    if (!title.code(code, error)) return false;
    out = from_code(title.exheader, code);
    return true;
}

// the segments sit one after another in code, each padded to whole pages,
// the same way the loader maps them
Image Image::from_code(const ExHeader &header, const std::vector<uint8_t> &code) {
    auto segment = [&](const CodeSetInfo &info, size_t offset) {
        Segment out;
        out.base = info.address;
        size_t end = offset + info.size;
        if (offset <= end && end <= code.size()) out.bytes.assign(code.begin() + offset, code.begin() + end);
        return out;
    };
    size_t rodata_offset = size_t(header.text.num_pages) * PAGE_SIZE;
    size_t data_offset = rodata_offset + size_t(header.rodata.num_pages) * PAGE_SIZE;
    Image image;
    image.entry = header.text.address;
    image.text = segment(header.text, 0);
    image.rodata = segment(header.rodata, rodata_offset);
    image.data = segment(header.data, data_offset);
    return image;
}

// nothing says where the executable keeps pointers, so any word in the data
// segments that lands in the code is taken for one
Program Image::into_program(const std::vector<uint32_t> &exports, const std::vector<uint32_t> &imported) const {
    Program program;
    program.text = text;
    program.seeds.emplace_back(entry, Source::Entry);
    for (uint32_t address : exports) program.seeds.emplace_back(address, Source::Export);
    for (uint32_t address : imported) program.seeds.emplace_back(address, Source::Import);
    for (const Segment *segment : {&rodata, &data})
        for (auto [_, value] : segment->words())
            if (text.contains(value & ~1u)) program.seeds.emplace_back(value, Source::Pointer);
    program.slots = std::nullopt;
    return program;
}

} // namespace recomp
