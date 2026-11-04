// Gateway codes and IPS patches. The code types as Azahar (Citra) runs them:
// writes, conditions on a word or a halfword (with a mask), the offset and
// the data register, a loop, the button condition and a run of bytes.

#include "runtime/cheats.h"

#include <cctype>
#include <cstdlib>
#include <sstream>

namespace runtime {

namespace {

bool hex_word(const std::string &s, uint32_t &out) {
    if (s.size() != 8) return false;
    for (char c : s) {
        if (!std::isxdigit(static_cast<unsigned char>(c))) return false;
    }
    out = uint32_t(std::strtoul(s.c_str(), nullptr, 16));
    return true;
}

uint32_t read(Memory &memory, uint32_t a, int size) {
    return size == 4 ? memory.read32(a) : size == 2 ? memory.read16(a) : memory.read8(a);
}

// a write the way a cheat device makes it: to any mapped page, the
// read-only ones (code, constants) too
void poke(Memory &memory, uint32_t a, uint32_t value, int size, uint32_t text, uint32_t text_end, CheatRun &run) {
    uint32_t mask = size == 4 ? 0xFFFFFFFFu : size == 2 ? 0xFFFFu : 0xFFu;
    value &= mask;
    if (read(memory, a, size) == value) return;
    if (size == 4) memory.cpu_write32(a, value);
    else if (size == 2) memory.cpu_write16(a, uint16_t(value));
    else memory.cpu_write8(a, uint8_t(value));
    if (read(memory, a, size) != value) {
        // a page the game cannot write: byte by byte into its words
        for (int i = 0; i < size; i++) {
            uint32_t at = a + uint32_t(i), word = at & ~3u, shift = (at & 3) * 8;
            uint32_t old = memory.read32(word);
            memory.write32_privileged(word, (old & ~(0xFFu << shift)) | ((value >> (i * 8)) & 0xFF) << shift);
        }
    }
    run.writes++;
    if (a < text_end && a + uint32_t(size) > text) run.code = true;
}

} // namespace

std::vector<Cheat> parse_cheats(const std::string &text) {
    std::vector<Cheat> out;
    std::istringstream lines(text);
    std::string line;
    while (std::getline(lines, line)) {
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ' || line.back() == '\t')) line.pop_back();
        size_t start = line.find_first_not_of(" \t");
        if (start == std::string::npos) continue;
        line = line.substr(start);
        if (line[0] == '[') {
            size_t end = line.rfind(']');
            out.push_back(Cheat{line.substr(1, end == std::string::npos || end == 0 ? std::string::npos : end - 1)});
            continue;
        }
        if (out.empty()) continue;
        if (line == "*citra_enabled") {
            out.back().enabled = true;
            continue;
        }
        std::istringstream words(line);
        std::string a, b;
        uint32_t first, second;
        if (words >> a >> b && hex_word(a, first) && hex_word(b, second)) out.back().lines.emplace_back(first, second);
        // anything else (notes, "*" lines) is the file's
    }
    return out;
}

void run_cheat(const Cheat &cheat, Memory &memory, uint32_t buttons, uint32_t text, uint32_t text_end, CheatRun &run) {
    uint32_t offset = 0, data = 0;
    // conditions not met, nested: their lines are skipped to the D0 that
    // ends them
    int skipping = 0;
    size_t loop_start = 0;
    uint32_t loop_count = 0;
    bool looping = false;
    const auto &lines = cheat.lines;
    for (size_t i = 0; i < lines.size(); i++) {
        auto [first, value] = lines[i];
        uint32_t type = first >> 28, address = first & 0x0FFFFFFF;
        uint32_t sub = first >> 24; // the D codes
        bool condition = (type >= 3 && type <= 0xA) || sub == 0xDD;
        if (skipping) {
            if (condition) skipping++;
            else if (sub == 0xD0) skipping--;
            else if (sub == 0xD2) skipping = 0, offset = 0, data = 0, looping = false;
            else if (type == 0xE) i += (value + 7) / 8; // its bytes are not lines
            continue;
        }
        auto test = [&](bool met) {
            if (!met) skipping = 1;
        };
        switch (type) {
        case 0x0: poke(memory, address + offset, value, 4, text, text_end, run); break;
        case 0x1: poke(memory, address + offset, value, 2, text, text_end, run); break;
        case 0x2: poke(memory, address + offset, value, 1, text, text_end, run); break;
        // the word there below, above, equal to or not the value
        case 0x3: test(memory.read32(address + offset) < value); break;
        case 0x4: test(memory.read32(address + offset) > value); break;
        case 0x5: test(memory.read32(address + offset) == value); break;
        case 0x6: test(memory.read32(address + offset) != value); break;
        // the halfword, the mask's bits (the value's top half) left out
        case 0x7:
        case 0x8:
        case 0x9:
        case 0xA: {
            uint16_t there = uint16_t(memory.read16(address + offset) & ~(value >> 16));
            uint16_t want = uint16_t(value);
            test(type == 7 ? there < want : type == 8 ? there > want : type == 9 ? there == want : there != want);
            break;
        }
        case 0xB: offset = memory.read32(address + offset); break;
        case 0xC:
            loop_start = i + 1;
            loop_count = value;
            looping = true;
            break;
        case 0xD:
            switch (sub) {
            case 0xD0: break; // the end of a condition that held
            case 0xD1:
                if (looping && loop_count) {
                    loop_count--;
                    i = loop_start - 1;
                } else {
                    looping = false;
                }
                break;
            case 0xD2:
                offset = data = 0;
                looping = false;
                break;
            case 0xD3: offset = value; break;
            case 0xD4: data += value; break;
            case 0xD5: data = value; break;
            case 0xD6: poke(memory, value + offset, data, 4, text, text_end, run), offset += 4; break;
            case 0xD7: poke(memory, value + offset, data, 2, text, text_end, run), offset += 2; break;
            case 0xD8: poke(memory, value + offset, data, 1, text, text_end, run), offset += 1; break;
            case 0xD9: data = memory.read32(value + offset); break;
            case 0xDA: data = memory.read16(value + offset); break;
            case 0xDB: data = memory.read8(value + offset); break;
            case 0xDC: offset += value; break;
            case 0xDD: test((buttons & value) == value); break;
            default: break;
            }
            break;
        case 0xE: {
            // value bytes from the lines after it, each line's two words in
            // order, little-endian
            uint32_t at = address + offset;
            for (uint32_t n = 0; n < value; n++) {
                size_t line = i + 1 + n / 8;
                if (line >= lines.size()) break;
                uint32_t word = (n % 8) < 4 ? lines[line].first : lines[line].second;
                poke(memory, at + n, word >> ((n % 4) * 8), 1, text, text_end, run);
            }
            i += (value + 7) / 8;
            break;
        }
        default: break; // the F codes (and others) Azahar leaves alone too
        }
    }
}

bool apply_ips(const std::vector<uint8_t> &patch, std::vector<uint8_t> &data, std::string &error) {
    auto at = [&](size_t i, int n) {
        uint32_t v = 0;
        for (int k = 0; k < n; k++) v = v << 8 | patch[i + size_t(k)];
        return v;
    };
    if (patch.size() < 8 || std::string(patch.begin(), patch.begin() + 5) != "PATCH") {
        error = "not an IPS patch";
        return false;
    }
    size_t i = 5;
    for (;;) {
        if (i + 3 > patch.size()) {
            error = "the IPS patch ends before its EOF";
            return false;
        }
        if (std::string(patch.begin() + long(i), patch.begin() + long(i) + 3) == "EOF") {
            i += 3;
            if (i + 3 <= patch.size()) data.resize(at(i, 3)); // the size to cut to
            return true;
        }
        if (i + 5 > patch.size()) break;
        uint32_t offset = at(i, 3), size = at(i + 3, 2);
        i += 5;
        if (size) {
            if (i + size > patch.size()) break;
            if (data.size() < offset + size) data.resize(offset + size);
            std::copy(patch.begin() + long(i), patch.begin() + long(i + size), data.begin() + offset);
            i += size;
        } else {
            // a run of one byte
            if (i + 3 > patch.size()) break;
            uint32_t count = at(i, 2);
            uint8_t value = patch[i + 2];
            i += 3;
            if (data.size() < offset + count) data.resize(offset + count);
            std::fill(data.begin() + offset, data.begin() + offset + count, value);
        }
    }
    error = "the IPS patch is cut short";
    return false;
}

} // namespace runtime
