// Loading a game. Translated from Zakuro's loader.rs, zakuro-fs's ncch.rs
// and 3dsrecomp's abi (Rust, MIT, Copyright (c) 2026 fearkov; see
// LICENSE-zakuro).

#include "runtime/loader.h"

#include <algorithm>
#include <cstring>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#else
#include <dlfcn.h>
#endif

namespace runtime {

namespace {

template <typename T> T get(const std::vector<uint8_t> &b, size_t at) {
    T v;
    std::memcpy(&v, b.data() + at, sizeof v);
    return v;
}

Segment segment(const std::vector<uint8_t> &b, size_t at) {
    return Segment{get<uint32_t>(b, at), get<uint32_t>(b, at + 4), get<uint32_t>(b, at + 8)};
}

void *symbol(void *library, const char *name) {
#ifdef _WIN32
    return reinterpret_cast<void *>(GetProcAddress(static_cast<HMODULE>(library), name));
#else
    return dlsym(library, name);
#endif
}

} // namespace

bool parse_exheader(const std::vector<uint8_t> &b, ExHeader &out, std::string &error) {
    if (b.size() < 0x400) {
        error = "the exheader is too short";
        return false;
    }
    out.compressed = b[0x0D] & 1;
    out.text = segment(b, 0x10);
    out.stack_size = get<uint32_t>(b, 0x1C);
    out.rodata = segment(b, 0x20);
    out.data = segment(b, 0x30);
    out.bss_size = get<uint32_t>(b, 0x3C);
    out.program_id = get<uint64_t>(b, 0x200);
    out.priority = b[0x20F];
    // the Old 3DS system mode, in flag 0's top bits
    static const uint32_t modes[] = {64, 0, 96, 80, 72, 32};
    uint8_t mode = b[0x20E] >> 4;
    out.app_bytes = (mode < 6 && modes[mode] ? modes[mode] : 64) << 20;
    // the kernel capabilities: the kernel version and the memory type
    out.region = Region::Application;
    out.kernel_version = 0;
    for (int i = 0; i < 28; i++) {
        uint32_t desc = get<uint32_t>(b, 0x370 + i * 4);
        if (desc == 0xFFFFFFFF) continue;
        uint32_t tag = desc >> 20;
        if ((tag & 0xFE0) == 0xFC0) {
            out.kernel_version = static_cast<uint16_t>(desc);
        } else if ((tag & 0xFF8) == 0xFF0) {
            uint32_t type = (desc >> 8) & 0xF;
            out.region = type == 2 ? Region::System : type == 3 ? Region::Base : Region::Application;
        }
    }
    if (out.compressed) {
        error = "the code is compressed: extract it with ctrtool's --decompresscode";
        return false;
    }
    return true;
}

bool Library::open(const std::string &path, std::string &error) {
#ifdef _WIN32
    void *library = LoadLibraryA(path.c_str());
#else
    void *library = dlopen(path.c_str(), RTLD_NOW);
#endif
    if (!library) {
        error = "could not load " + path;
        return false;
    }
    auto *abi = static_cast<const uint32_t *>(symbol(library, "recomp_abi"));
    auto *program = static_cast<const uint64_t *>(symbol(library, "recomp_program_id"));
    auto *count = static_cast<const uint32_t *>(symbol(library, "recomp_entry_count"));
    auto *modules = static_cast<const uint32_t *>(symbol(library, "recomp_module_count"));
    entries_ = static_cast<const Entry *>(symbol(library, "recomp_entries"));
    modules_ = static_cast<const Module *>(symbol(library, "recomp_modules"));
    if (!abi || !count || !entries_) {
        error = path + " is not code 3dsrecomp made";
        return false;
    }
    if (*abi != RECOMP_ABI) {
        error = path + " was made for interface " + std::to_string(*abi) + ", the runtime speaks " +
                std::to_string(RECOMP_ABI) + ": recompile the game";
        return false;
    }
    program_id_ = program ? *program : 0;
    count_ = *count;
    module_count_ = modules ? *modules : 0;
    return true;
}

Code Library::lookup(uint32_t address) const {
    auto find = [](const Entry *entries, size_t count, uint32_t address) -> Code {
        const Entry *end = entries + count;
        const Entry *it = std::lower_bound(entries, end, address,
                                           [](const Entry &e, uint32_t a) { return e.address < a; });
        return it != end && it->address == address ? it->code : nullptr;
    };
    if (Code code = find(entries_, count_, address)) {
        return code;
    }
    for (const Placed &p : placed_) {
        if (address - p.base < p.size) {
            return find(modules_[p.index].entries, modules_[p.index].count, address - p.base);
        }
    }
    return nullptr;
}

bool Library::place(const std::string &name, uint32_t base) {
    for (size_t i = 0; i < module_count_; i++) {
        if (name != modules_[i].name) continue;
        *modules_[i].base = base; // the code reads it on entry
        placed_.erase(std::remove_if(placed_.begin(), placed_.end(), [&](const Placed &p) { return p.index == i; }),
                      placed_.end());
        if (base) placed_.push_back(Placed{base, modules_[i].size, i});
        return true;
    }
    return false;
}

void load(System &system, const ExHeader &exheader, const std::vector<uint8_t> &code) {
    Memory &memory = system.memory;
    memory.linear_base = exheader.kernel_version >= 0x22C ? map::LINEAR_VADDR : map::LINEAR_VADDR_OLD;
    // VRAM and the DSP's memory are mapped into every process
    memory.map(map::VRAM_VADDR, map::VRAM_PADDR, map::VRAM_SIZE, RW, MemoryState::Static);
    memory.map(map::DSP_RAM_VADDR, map::DSP_RAM_PADDR, map::DSP_RAM_SIZE, RW, MemoryState::Static);

    struct Part {
        Segment segment;
        uint32_t permission, offset;
    };
    const Part parts[] = {
        {exheader.text, READ | EXECUTE, 0},
        {exheader.rodata, READ, exheader.text.pages * map::PAGE_SIZE},
        {exheader.data, RW, (exheader.text.pages + exheader.rodata.pages) * map::PAGE_SIZE},
    };
    for (const Part &part : parts) {
        if (!part.segment.pages) continue;
        uint32_t size = part.segment.pages * map::PAGE_SIZE;
        auto block = memory.allocate(exheader.region, size, true);
        if (!block) continue;
        uint8_t *host = memory.host(block->paddr, size);
        std::memset(host, 0, size);
        size_t end = std::min<size_t>(code.size(), part.offset + size);
        if (part.offset < end) std::memcpy(host, code.data() + part.offset, end - part.offset);
        memory.map(part.segment.address, block->paddr, size, part.permission, MemoryState::Code);
    }

    // the bss: right where .data's bytes end, the rest in pages of its own
    uint32_t bss_start = exheader.data.address + exheader.data.pages * map::PAGE_SIZE;
    uint32_t bss_end = exheader.data.address + exheader.data.size + exheader.bss_size;
    if (bss_end > bss_start) {
        uint32_t size = (bss_end - bss_start + map::PAGE_MASK) & ~map::PAGE_MASK;
        if (auto block = memory.allocate(exheader.region, size, true)) {
            std::memset(memory.host(block->paddr, size), 0, size);
            memory.map(bss_start, block->paddr, size, RW, MemoryState::Private);
        }
    }

    uint32_t stack = (std::max(exheader.stack_size, 0x4000u) + map::PAGE_MASK) & ~map::PAGE_MASK;
    if (auto block = memory.allocate(exheader.region, stack, true)) {
        std::memset(memory.host(block->paddr, stack), 0, stack);
        memory.map(STACK_TOP - stack, block->paddr, stack, RW, MemoryState::Locked);
    }
    memory.heap_top = map::HEAP_VADDR;
    system.kernel.start(exheader.text.address, STACK_TOP, exheader.priority);
}

} // namespace runtime
