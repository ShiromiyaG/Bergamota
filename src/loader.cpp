// Loading a game. Translated from Zakuro's loader.rs, zakuro-fs's ncch.rs
// and 3dsrecomp's abi, Library::check among it (Rust, MIT, Copyright (c)
// 2026 fearkov; see LICENSE-zakuro).

#include "runtime/loader.h"

#include <algorithm>
#include <cstdio>
#include <cstring>

#include "runtime/decomp.h"

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
    // generation 2 laid the table out differently, nothing was made with it
    // but tests
    auto *generation = static_cast<const uint32_t *>(symbol(library, "recomp_generation"));
    origins_ = generation && *generation >= 3 ? static_cast<const Origins *>(symbol(library, "recomp_origins")) : nullptr;
    return true;
}

void Library::linked(const Entry *entries, size_t count, const Module *modules, size_t module_count,
                     const Origins *origins) {
    entries_ = entries;
    count_ = count;
    modules_ = modules;
    module_count_ = module_count;
    origins_ = origins;
    placed_.clear();
}

bool Library::stale(size_t unit, size_t entry) const {
    if (!origins_) return false;
    const Origins &o = origins_[unit];
    if (!o.owners) return false;
    uint32_t owner = o.owners[entry];
    return owner != NO_ORIGIN && o.stale[owner] != 0;
}

Code Library::lookup(uint32_t address) const {
    auto find = [&](const Entry *entries, size_t count, size_t unit, uint32_t address) -> Code {
        const Entry *end = entries + count;
        const Entry *it = std::lower_bound(entries, end, address,
                                           [](const Entry &e, uint32_t a) { return e.address < a; });
        if (it == end || it->address != address || stale(unit, size_t(it - entries))) return nullptr;
        return it->code;
    };
    if (Code code = find(entries_, count_, 0, address)) {
        return code;
    }
    for (const Placed &p : placed_) {
        if (address - p.base < p.size) {
            return find(modules_[p.index].entries, modules_[p.index].count, p.index + 1, address - p.base);
        }
    }
    return nullptr;
}

uint32_t Library::redirect(uint32_t address) const {
    if (aliases_.empty()) return address;
    auto alias = aliases_.find(address);
    return alias == aliases_.end() ? address : alias->second;
}

bool Library::place(const std::string &name, uint32_t base, Memory *memory) {
    for (size_t i = 0; i < module_count_; i++) {
        if (name != modules_[i].name) continue;
        *modules_[i].base = base; // the code reads it on entry
        for (const Placed &p : placed_) {
            if (p.index != i) continue;
            for (auto it = aliases_.begin(); it != aliases_.end();) {
                it = it->first - p.base < p.size ? aliases_.erase(it) : std::next(it);
            }
        }
        placed_.erase(std::remove_if(placed_.begin(), placed_.end(), [&](const Placed &p) { return p.index == i; }),
                      placed_.end());
        if (base) placed_.push_back(Placed{base, modules_[i].size, i});
        if (base && memory) {
            if (size_t stale = check(i + 1, base, *memory)) {
                std::printf("%zu functions of %s run in the interpreter: changed since it was recompiled\n", stale,
                            name.c_str());
            }
        }
        return true;
    }
    return false;
}

size_t Library::check(size_t unit, uint32_t base, Memory &memory) {
    if (!origins_ || unit > module_count_) return 0;
    const Origins &o = origins_[unit];
    if (!o.piece_count || !o.count) return 0;
    const Piece *pieces = o.pieces;
    uint32_t first = pieces[0].start, last = pieces[o.piece_count - 1].end;
    std::vector<uint8_t> code(last - first);
    memory.read_bytes(base + first, code.data(), uint32_t(code.size()));
    // how many of the pieces before each one changed, 64-bit FNV-1a
    std::vector<uint32_t> changed_before(o.piece_count + 1, 0);
    for (uint32_t i = 0; i < o.piece_count; i++) {
        uint64_t hash = 0xCBF29CE484222325ull;
        for (uint32_t at = pieces[i].start; at < pieces[i].end; at++) {
            hash = (hash ^ code[at - first]) * 0x100000001B3ull;
        }
        changed_before[i + 1] = changed_before[i] + (hash != pieces[i].hash);
    }
    size_t stale = 0;
    for (uint32_t f = 0; f < o.count; f++) {
        const Origin &function = o.functions[f];
        bool changed = false;
        for (uint32_t r = function.first; r < function.first + function.count && !changed; r++) {
            changed = changed_before[o.runs[r].end] > changed_before[o.runs[r].first];
        }
        o.stale[f] = changed;
        o.starts[f] = changed ? NO_START : function.start;
        stale += changed;
    }
    return stale;
}

bool Library::has_module(const std::string &name) const {
    for (size_t i = 0; i < module_count_; i++) {
        if (name == modules_[i].name) return true;
    }
    return false;
}

bool link_code(Library &library, Memory &memory, const Decomp *decomp, std::string &error) {
    if (decomp && !library.has_module(DECOMP_MODULE)) {
        error = "the library was recompiled without the decomp: build it again with 3dsrecomp --decomp";
        return false;
    }
    if (decomp && !library.has_origins() && !decomp->module_hooks.empty()) {
        std::fprintf(stderr, "decomp: the library is too old to check, the modules' %zu functions stay the game's\n",
                     decomp->module_hooks.size());
    }
    if (size_t stale = library.check(0, 0, memory)) {
        std::printf("%zu functions of the executable run in the interpreter: changed since it was recompiled\n", stale);
    }
    if (decomp) library.place(DECOMP_MODULE, decomp->base, &memory);
    return true;
}

void module_changed(Library &library, Memory &memory, const Decomp *decomp, const CroLoader &cro,
                    const std::string &name, uint32_t base) {
    if (decomp) {
        if (base) {
            for (const auto &module : cro.modules()) {
                if (module.base != base) continue;
                if (size_t hooked = hook_module(memory, *decomp, module)) {
                    std::printf("decomp: %zu functions of %s hooked at 0x%08X\n", hooked, name.c_str(), base);
                }
            }
        } else {
            unhook_module(memory, *decomp, name);
        }
    }
    library.place(name, base, &memory);
    // the module was recompiled without the jumps to the decomp: its hooked
    // functions go straight to the decomp's (ARM ones; a Thumb jump changes
    // state and keeps to the interpreter)
    if (decomp && base) {
        std::string bare = name.size() > 4 && name.compare(name.size() - 4, 4, ".cro") == 0
                               ? name.substr(0, name.size() - 4)
                               : name;
        for (const ModuleHook &hook : decomp->module_hooks) {
            if (hook.module == bare && !(hook.offset & 1)) library.alias(base + hook.offset, hook.target);
        }
    }
}

void load(System &system, const ExHeader &exheader, const std::vector<uint8_t> &code_bin, const Decomp *decomp) {
    // the jumps to the decomp, as 3dsrecomp --decomp wrote them before
    // recompiling, so the code in memory is the code recompiled
    std::vector<uint8_t> patched;
    if (decomp) {
        patched = code_bin;
        size_t text = std::min<size_t>(patched.size(), size_t(exheader.text.pages) * map::PAGE_SIZE);
        std::vector<uint8_t> text_bytes(patched.begin(), patched.begin() + text);
        patch_text(*decomp, exheader.text.address, text_bytes);
        std::copy(text_bytes.begin(), text_bytes.end(), patched.begin());
    }
    const std::vector<uint8_t> &code = decomp ? patched : code_bin;
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
    if (decomp) {
        std::string error;
        if (!map_decomp(memory, exheader.region, *decomp, error)) std::fprintf(stderr, "decomp: %s\n", error.c_str());
    }
    system.kernel.start(exheader.text.address, STACK_TOP, exheader.priority);
}

} // namespace runtime
