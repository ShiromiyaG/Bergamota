// The decomp's segment and its jumps. Translated from Zakuro's decomp.rs and
// loader.rs (Rust, MIT, Copyright (c) 2026 fearkov; see LICENSE-zakuro).

#include "runtime/decomp.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <sstream>

namespace runtime {

namespace {

// path in UTF-8, as the settings keep them
bool read_file(const std::string &path, std::vector<uint8_t> &out) {
    std::ifstream file(std::filesystem::path(std::u8string(path.begin(), path.end())), std::ios::binary);
    if (!file) return false;
    out.assign(std::istreambuf_iterator<char>(file), {});
    return true;
}

std::string without_cro(std::string name) {
    if (name.size() > 4 && name.compare(name.size() - 4, 4, ".cro") == 0) name.resize(name.size() - 4);
    return name;
}

// bytes written over code, read-only pages too, the way the CRO loader
// relocates: word by word, the rest of each word kept
void write_code(Memory &memory, uint32_t address, const std::vector<uint8_t> &bytes) {
    for (size_t i = 0; i < bytes.size(); i++) {
        uint32_t at = address + uint32_t(i), word = at & ~3u, shift = (at & 3) * 8;
        uint32_t value = memory.read32(word);
        memory.write32_privileged(word, (value & ~(0xFFu << shift)) | uint32_t(bytes[i]) << shift);
    }
}

} // namespace

bool parse_decomp(const std::string &text, Decomp &out, std::string &error) {
    std::istringstream lines(text);
    std::string line;
    while (std::getline(lines, line)) {
        std::istringstream in(line);
        std::vector<std::string> words;
        for (std::string word; in >> word;) words.push_back(word);
        if (words.empty() || words[0][0] == '#') continue;
        bool ok = true;
        auto word = [&](size_t i) -> std::string {
            if (i < words.size()) return words[i];
            if (ok) error = "decomp.txt: '" + line + "' is short";
            ok = false;
            return {};
        };
        auto number = [&](size_t i) -> uint32_t {
            std::string w = word(i);
            if (!ok) return 0;
            std::string digits = w.rfind("0x", 0) == 0 ? w.substr(2) : w;
            char *end = nullptr;
            unsigned long value = digits.empty() ? 0 : std::strtoul(digits.c_str(), &end, 16);
            if (digits.empty() || *end || value > 0xFFFFFFFFul) {
                error = "decomp.txt: '" + w + "' is not an address";
                ok = false;
            }
            return uint32_t(value);
        };
        const std::string &kind = words[0];
        if (kind == "base") {
            out.base = number(1);
        } else if (kind == "hook") {
            uint32_t old = number(1), target = number(2);
            out.hooks.emplace_back(old, target);
            if (words.size() > 3) out.returns[old] = words[3];
        } else if (kind == "module_hook") {
            ModuleHook hook{without_cro(word(1)), number(2), number(3), words.size() > 4 ? words[4] : ""};
            out.module_hooks.push_back(hook);
        } else if (kind == "module_import") {
            ModuleImport import{without_cro(word(1)), number(2), number(3), -1};
            if (words.size() > 4 && words[4].rfind("seg", 0) == 0) import.segment = std::atoi(words[4].c_str() + 3);
            out.module_imports.push_back(import);
        } else if (kind == "symbol") {
            std::string name = word(1);
            out.symbols[name] = number(2);
        } else if (kind == "patch") {
            uint32_t at = number(1), value = number(2);
            out.patches.emplace_back(at, value);
        } else if (kind == "option") {
            DecompOption option;
            if (parse_option(line, option)) {
                out.options.push_back(option);
            } else {
                error = "decomp.txt: '" + line + "' is not an option it knows";
                ok = false;
            }
        }
        // entry lines are for 3dsrecomp, anything else for a newer host
        if (!ok) return false;
    }
    if (out.base == 0 || out.base % map::PAGE_SIZE != 0) {
        error = "decomp.txt needs a base on a page";
        return false;
    }
    return true;
}

bool read_decomp(const std::string &dir, Decomp &out, std::string &error) {
    std::vector<uint8_t> text;
    if (!read_file(dir + "/decomp.bin", out.code)) {
        error = "could not read " + dir + "/decomp.bin";
        return false;
    }
    if (!read_file(dir + "/decomp.txt", text)) {
        error = "could not read " + dir + "/decomp.txt";
        return false;
    }
    return parse_decomp(std::string(text.begin(), text.end()), out, error);
}

std::vector<uint8_t> decomp_stub(uint32_t old, uint32_t target) {
    constexpr uint32_t LDR_PC = 0xE51FF004;
    std::vector<uint8_t> bytes;
    auto put = [&](uint32_t value, int size) {
        for (int i = 0; i < size; i++) bytes.push_back(uint8_t(value >> (i * 8)));
    };
    if (old & 1) {
        put(0x4778, 2); // bx pc
        // the ARM code starts at the next whole word: after a nop from a
        // word's start, straight after the bx from half a word in
        if (!(old & 2)) put(0x46C0, 2);
    }
    put(LDR_PC, 4);
    put(target & ~1u, 4);
    return bytes;
}

size_t patch_text(const Decomp &decomp, uint32_t text, std::vector<uint8_t> &bytes) {
    size_t left = 0;
    for (const auto &[old, target] : decomp.hooks) {
        std::vector<uint8_t> stub = decomp_stub(old, target);
        uint32_t at = (old & ~1u) - text;
        if (size_t(at) + stub.size() > bytes.size()) {
            std::fprintf(stderr, "decomp: 0x%08X is not in the code, left alone\n", old);
            left++;
            continue;
        }
        std::memcpy(bytes.data() + at, stub.data(), stub.size());
    }
    return left;
}

uint32_t module_address(uint32_t base, const std::vector<uint32_t> &segments, uint32_t offset, int segment) {
    uint32_t from = segment < 0 ? base : size_t(segment) < segments.size() ? segments[segment] : 0;
    return from + offset;
}

bool map_decomp(Memory &memory, Region region, const Decomp &decomp, std::string &error) {
    uint32_t size = std::max<uint32_t>((uint32_t(decomp.code.size()) + map::PAGE_MASK) & ~map::PAGE_MASK, DECOMP_ROOM);
    auto block = memory.allocate(region, size, true);
    if (!block) {
        error = "no memory for the decomp's " + std::to_string(size >> 10) + " KiB";
        return false;
    }
    uint8_t *host = memory.host(block->paddr, size);
    std::memset(host, 0, size);
    std::memcpy(host, decomp.code.data(), decomp.code.size());
    memory.map(decomp.base, block->paddr, size, READ | WRITE | EXECUTE, MemoryState::Code);
    std::printf("decomp: %zu KiB at 0x%08X, %zu functions replaced, %zu in modules\n", decomp.code.size() >> 10,
                decomp.base, decomp.hooks.size(), decomp.module_hooks.size());
    return true;
}

size_t hook_module(Memory &memory, const Decomp &decomp, const CroLoader::Module &module) {
    std::string name = without_cro(module.name);
    for (const ModuleImport &import : decomp.module_imports) {
        if (import.module != name) continue;
        memory.write32_privileged(import.slot,
                                  module_address(module.base, module.segments, import.offset, import.segment));
    }
    size_t hooked = 0;
    for (const ModuleHook &hook : decomp.module_hooks) {
        if (hook.module != name) continue;
        uint32_t old = module.base + hook.offset;
        write_code(memory, old & ~1u, decomp_stub(old, hook.target));
        hooked++;
    }
    return hooked;
}

void unhook_module(Memory &memory, const Decomp &decomp, const std::string &name) {
    std::string bare = without_cro(name);
    for (const ModuleImport &import : decomp.module_imports) {
        if (import.module == bare) memory.write32_privileged(import.slot, 0);
    }
}

} // namespace runtime
