// Reading CRO modules and the CRS that describes the main executable,
// straight from their files. Translated from 3dsrecomp's cro.rs (Rust, MIT,
// Copyright (c) 2026 fearkov; see LICENSE-3dsrecomp).

#include "cro.h"

#include <cstring>
#include <set>

#include "rust.h"

namespace recomp::cro {

namespace {

uint32_t word(const uint8_t *entry, size_t at) {
    return uint32_t(entry[at]) | uint32_t(entry[at + 1]) << 8 | uint32_t(entry[at + 2]) << 16 |
           uint32_t(entry[at + 3]) << 24;
}

std::optional<uint32_t> field(const std::vector<uint8_t> &bytes, size_t offset) {
    if (offset + 4 > bytes.size()) return std::nullopt;
    return word(bytes.data(), offset);
}

std::string cstring(const std::vector<uint8_t> &bytes, size_t offset) {
    if (offset > bytes.size()) return {};
    const uint8_t *tail = bytes.data() + offset;
    size_t size = bytes.size() - offset;
    size_t end = 0;
    while (end < size && tail[end] != 0) end++;
    return rust::from_utf8_lossy(tail, end);
}

// a table of fixed size entries, from the header fields holding its offset
// and its count
std::optional<std::vector<const uint8_t *>> table(const std::vector<uint8_t> &bytes, size_t offset_field,
                                                  size_t count_field, size_t entry_size) {
    auto offset = field(bytes, offset_field);
    if (!offset) return std::nullopt;
    auto count = field(bytes, count_field);
    if (!count) return std::nullopt;
    std::vector<const uint8_t *> out;
    for (size_t i = 0; i < *count; i++) {
        size_t start = size_t(*offset) + i * entry_size;
        if (start + entry_size > bytes.size()) return std::nullopt;
        out.push_back(bytes.data() + start);
    }
    return out;
}

Relocation relocation(const uint8_t *entry) {
    Relocation out;
    out.target = word(entry, 0);
    out.kind = entry[4];
    out.segment = entry[5];
    out.addend = word(entry, 8);
    return out;
}

} // namespace

std::optional<Module> parse(const std::vector<uint8_t> &bytes) {
    if (bytes.size() < 0x84 || std::memcmp(bytes.data() + 0x80, "CRO0", 4) != 0) return std::nullopt;
    Module module;
    auto segments = table(bytes, 0xC8, 0xCC, 12);
    if (!segments) return std::nullopt;
    for (const uint8_t *entry : *segments) module.segments.push_back({word(entry, 0), word(entry, 4), word(entry, 8)});
    auto exports = table(bytes, 0xD0, 0xD4, 8);
    if (!exports) return std::nullopt;
    for (const uint8_t *entry : *exports) module.exports.emplace_back(cstring(bytes, word(entry, 0)), word(entry, 4));
    auto indexed = table(bytes, 0xD8, 0xDC, 4);
    if (!indexed) return std::nullopt;
    for (const uint8_t *entry : *indexed) module.indexed_exports.push_back(word(entry, 0));
    auto relocations = table(bytes, 0x128, 0x12C, 12);
    if (!relocations) return std::nullopt;
    for (const uint8_t *entry : *relocations) module.relocations.push_back(relocation(entry));
    auto imports = table(bytes, 0xF8, 0xFC, 12);
    if (!imports) return std::nullopt;
    for (const uint8_t *entry : *imports) module.imports.push_back(relocation(entry));
    // each module imported from names its anonymous symbols, a table of a
    // segment tag and the first place it goes
    auto modules = table(bytes, 0xF0, 0xF4, 20);
    if (!modules) return std::nullopt;
    for (const uint8_t *entry : *modules) {
        std::string name = cstring(bytes, word(entry, 0));
        size_t offset = word(entry, 12), count = word(entry, 16);
        for (size_t i = 0; i < count; i++) {
            auto tag = field(bytes, offset + i * 8);
            if (!tag) return std::nullopt;
            module.anonymous_imports.emplace_back(name, *tag);
        }
    }
    auto name = field(bytes, 0xC0);
    if (!name) return std::nullopt;
    module.name = cstring(bytes, *name);
    return module;
}

std::vector<uint32_t> Module::code_exports() const {
    std::vector<uint32_t> tags;
    for (const auto &[_, tag] : exports) tags.push_back(tag);
    tags.insert(tags.end(), indexed_exports.begin(), indexed_exports.end());
    std::vector<uint32_t> out;
    for (uint32_t tag : tags) {
        size_t index = tag & 0xF;
        if (index >= segments.size() || segments[index].kind != CODE) continue;
        if (auto address = resolve(tag)) out.push_back(*address);
    }
    return out;
}

std::optional<uint32_t> Module::code_address(uint32_t tag) const {
    if (!is_code(tag & 0xF)) return std::nullopt;
    return resolve(tag);
}

std::optional<uint32_t> Module::resolve(uint32_t tag) const {
    size_t index = tag & 0xF;
    if (index >= segments.size()) return std::nullopt;
    const Segment &segment = segments[index];
    uint32_t offset = tag >> 4;
    if (offset < segment.size) return segment.offset + offset;
    return std::nullopt;
}

std::vector<uint8_t> Module::image(const std::vector<uint8_t> &bytes, uint32_t base, uint32_t stub) const {
    uint32_t length = uint32_t(bytes.size());
    uint32_t bss = length % 0x1000 == 0 ? length : length + (0x1000 - length % 0x1000);
    uint32_t bss_size = 0;
    for (const Segment &segment : segments)
        if (segment.kind == BSS) bss_size += segment.size;
    std::vector<uint8_t> image = bytes;
    image.resize(size_t(uint32_t(bss + bss_size)), 0);
    auto offset = [&](const Segment &segment) { return segment.kind == BSS ? bss : segment.offset; };
    auto store = [&](size_t at, uint32_t value) {
        if (at + 4 > image.size()) return;
        for (int i = 0; i < 4; i++) image[at + i] = uint8_t(value >> (8 * i));
    };
    for (const Relocation &relocation : relocations) {
        if (!relocation.is_absolute()) continue;
        size_t target = relocation.target & 0xF;
        if (target >= segments.size()) continue;
        if (relocation.segment >= segments.size()) continue;
        const Segment &symbol = segments[relocation.segment];
        size_t at = uint32_t(offset(segments[target]) + (relocation.target >> 4));
        store(at, base + offset(symbol) + relocation.addend);
    }
    for (const Relocation &import : imports) {
        if (!import.is_absolute()) continue;
        size_t target = import.target & 0xF;
        if (target >= segments.size()) continue;
        size_t at = uint32_t(offset(segments[target]) + (import.target >> 4));
        store(at, stub + import.addend);
    }
    return image;
}

bool Module::is_code(uint32_t segment) const { return segment < segments.size() && segments[segment].kind == CODE; }

std::optional<Program> Module::program(const std::vector<uint8_t> &bytes, const std::vector<uint32_t> &imported) const {
    const Segment *segment = nullptr;
    for (const Segment &s : segments) {
        if (s.kind == CODE && s.size > 0) {
            segment = &s;
            break;
        }
    }
    if (!segment) return std::nullopt;
    size_t from = segment->offset, to = uint32_t(segment->offset + segment->size);
    if (from > to || to > bytes.size()) return std::nullopt;
    Program program;
    recomp::Segment &text = program.text;
    text.base = segment->offset;
    text.bytes.assign(bytes.begin() + from, bytes.begin() + to);

    for (uint32_t address : code_exports()) program.seeds.emplace_back(address, Source::Export);
    for (uint32_t address : imported) program.seeds.emplace_back(address, Source::Import);
    // every word in the code that a relocation or an import fills in is
    // data, whatever it points at
    uint32_t start = text.base, end = text.end();
    auto slot = [&](const Relocation &relocation) -> std::optional<uint32_t> {
        auto target = resolve(relocation.target);
        if (target && relocation.is_absolute() && *target >= start && uint32_t(*target + 4) <= end) return target;
        return std::nullopt;
    };
    std::set<uint32_t> slots;
    for (const Relocation &relocation : relocations)
        if (auto target = slot(relocation)) slots.insert(*target);
    for (const Relocation &import : imports)
        if (auto target = slot(import)) slots.insert(*target);
    for (const Relocation &relocation : relocations) {
        if (!is_code(relocation.segment)) continue;
        uint32_t address = segments[relocation.segment].offset + relocation.addend;
        program.seeds.emplace_back(address, Source::Relocation);
        if (auto target = slot(relocation)) {
            text.write32(*target, address);
            slots.insert(*target);
        }
    }
    program.slots = std::move(slots);
    return program;
}

} // namespace recomp::cro
