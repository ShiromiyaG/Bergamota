// Code written apart from the title that replaces some of its functions:
// decomp.bin and decomp.txt read, the jumps written over the functions it
// replaces. Translated from 3dsrecomp's decomp.rs (Rust, MIT, Copyright (c)
// 2026 fearkov; see LICENSE-3dsrecomp).

#include "decomp.h"

#include <algorithm>

#include "rust.h"

namespace recomp {

bool Decomp::read(const std::filesystem::path &dir, Decomp &out, std::string &error) {
    auto read = [&](const char *name, std::vector<uint8_t> &bytes) {
        std::string why;
        if (rust::read_file(dir / name, bytes, why)) return true;
        error = "could not read " + rust::display(dir / name) + ", " + why;
        return false;
    };
    Decomp decomp;
    if (!read("decomp.bin", decomp.code)) return false;
    std::vector<uint8_t> raw;
    if (!read("decomp.txt", raw)) return false;
    std::string text = rust::from_utf8_lossy(raw);
    for (std::string_view untrimmed : rust::lines(text)) {
        std::string_view line = rust::trim(untrimmed);
        if (line.empty() || line[0] == '#') continue;
        std::vector<std::string_view> words = rust::split_whitespace(line);
        auto number = [&](size_t i, uint32_t &value) {
            if (i >= words.size()) {
                error = "decomp.txt: '" + std::string(line) + "' is short";
                return false;
            }
            std::string_view word = words[i], digits = word;
            while (digits.substr(0, 2) == "0x") digits.remove_prefix(2);
            auto parsed = rust::parse_u32(digits, 16);
            if (!parsed) {
                error = "decomp.txt: '" + std::string(word) + "' is not an address";
                return false;
            }
            value = *parsed;
            return true;
        };
        std::string_view kind = words[0];
        if (kind == "base") {
            if (!number(1, decomp.base)) return false;
        } else if (kind == "entry") {
            uint32_t entry;
            if (!number(1, entry)) return false;
            decomp.entries.push_back(entry);
        } else if (kind == "hook") {
            uint32_t old, replacement;
            if (!number(1, old) || !number(2, replacement)) return false;
            decomp.hooks.emplace_back(old, replacement);
        } else if (kind == "module_hook" || kind == "module_import" || kind == "symbol" || kind == "patch" ||
                   kind == "option") {
            // a module's function, hooked by the emulator as the module
            // loads; a variable the host reads or writes; a word the host
            // changes as the game loads; an option the host shows and writes
        } else {
            error = "decomp.txt: '" + std::string(kind) + "' is not a line it knows";
            return false;
        }
    }
    if (decomp.base == 0) {
        error = "decomp.txt has no base";
        return false;
    }
    out = std::move(decomp);
    return true;
}

std::vector<uint8_t> Decomp::stub(uint32_t old, uint32_t replacement) {
    constexpr uint32_t LDR_PC = 0xE51FF004;
    constexpr uint16_t BX_PC = 0x4778;
    constexpr uint16_t NOP = 0x46C0;
    std::vector<uint8_t> bytes;
    auto put32 = [&](uint32_t value) {
        for (int i = 0; i < 4; i++) bytes.push_back(uint8_t(value >> (8 * i)));
    };
    auto put16 = [&](uint16_t value) {
        bytes.push_back(uint8_t(value));
        bytes.push_back(uint8_t(value >> 8));
    };
    if ((old & 1) == 0) {
        put32(LDR_PC);
    } else {
        put16(BX_PC);
        // the ARM code starts at the next whole word: after a nop from a
        // word's start, straight after the bx from half a word in
        if ((old & 2) == 0) put16(NOP);
        put32(LDR_PC);
    }
    put32(replacement & ~1u);
    return bytes;
}

void Decomp::patch(Segment &text) const {
    for (const auto &[old, replacement] : hooks) {
        uint32_t at = old & ~1u;
        std::vector<uint8_t> jump = stub(old, replacement);
        size_t offset = uint32_t(at - text.base);
        if (offset + jump.size() <= text.bytes.size())
            std::copy(jump.begin(), jump.end(), text.bytes.begin() + ptrdiff_t(offset));
    }
}

Program Decomp::program() const {
    Program program;
    for (uint32_t address : entries) program.seeds.emplace_back(address - base, Source::Export);
    program.text.base = 0;
    program.text.bytes = code;
    program.slots = std::set<uint32_t>();
    return program;
}

} // namespace recomp
