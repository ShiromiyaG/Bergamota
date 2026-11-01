// ldr:ro, loading the game's modules. After Zakuro's services/ldr_ro.rs.
// Zakuro: Rust, MIT, Copyright (c) 2026 fearkov; see LICENSE-zakuro.

#include "runtime/services.h"

#include "runtime/fs.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <cstdio>

namespace runtime {

bool Services::ldr_ro(System &system, CommandBuffer &b) {
    Memory &m = system.memory;
    uint32_t command = b.command();
    constexpr uint32_t NOT_LOADED = 0xD9012402;
    switch (command) {
    case 0x0001: { // Initialize(CRS buffer, size, address)
        uint32_t source = b.get(1), size = b.get(2), address = b.get(3);
        uint32_t base = address && address != source && m.mirror(address, source, size) ? address : source;
        if (cro.initialize(m, base, size)) b.reply(command);
        else b.reply_error(command, NOT_LOADED);
        return true;
    }
    case 0x0002: case 0x0003: // LoadCRR, UnloadCRR: a signature check nobody needs
    case 0x0006: case 0x0007: case 0x0008: // Link, Unlink, Shutdown: linked on loading already
        b.reply(command);
        return true;
    case 0x0004: case 0x0009: { // LoadCRO(buffer, address, size, data, 0, data size, bss, bss size, auto link, fix level, crr)
        uint32_t source = b.get(1), address = b.get(2), size = b.get(3);
        // the module runs from memory of its own: ldr:ro takes the buffer's
        // pages from the game, which may reuse them while the module runs
        uint32_t base = source;
        if (address && address != source) {
            if (auto block = m.allocate(Region::System, size)) {
                std::vector<uint8_t> bytes(size);
                m.read_bytes(source, bytes.data(), size);
                std::memcpy(m.host(block->paddr, size), bytes.data(), size);
                m.map(address, block->paddr, block->size, READ | WRITE | EXECUTE, MemoryState::Code);
                copies_[address] = Copy{source, *block, size};
                base = address;
            }
        }
        auto fixed = cro.load(m, base, size, b.get(4), b.get(6), b.get(7), b.get(8), (b.get(9) & 0xFF) != 0, b.get(10));
        if (!fixed) {
            b.reply_error(command, NOT_LOADED);
            return true;
        }
        auto copy = copies_.find(base);
        if (copy != copies_.end()) {
            // the game copies the module's relocated data out of its buffer
            // once this returns: on the console the two are the same memory
            std::vector<uint8_t> bytes(size);
            m.read_bytes(base, bytes.data(), size);
            m.write_bytes(copy->second.buffer, bytes.data(), size);
            // what fixing left stays, the rest goes back
            uint32_t kept = (*fixed ? *fixed : size) + map::PAGE_MASK & ~map::PAGE_MASK;
            copy->second.fixed = *fixed ? *fixed : size;
            if (kept < copy->second.block.size) {
                Block tail{copy->second.block.paddr + kept, copy->second.block.size - kept};
                m.unmap(base, copy->second.block.size);
                m.map(base, copy->second.block.paddr, kept, READ | WRITE | EXECUTE, MemoryState::Code);
                copy->second.block.size = kept;
                m.free(tail);
            }
        }
        if (on_module) on_module(cro.modules().back().name, base);
        b.reply(command, {*fixed});
        return true;
    }
    case 0x0005: { // UnloadCRO(address)
        uint32_t address = b.get(1);
        for (const auto &module : cro.modules()) {
            if (module.base == address && on_module) on_module(module.name, 0);
        }
        cro.unload(m, address);
        auto copy = copies_.find(address);
        if (copy != copies_.end()) {
            // the module, as ldr:ro left it, back in the game's buffer
            uint32_t size = std::min(copy->second.fixed, copy->second.block.size);
            std::vector<uint8_t> bytes(size);
            m.read_bytes(address, bytes.data(), size);
            m.write_bytes(copy->second.buffer, bytes.data(), size);
            m.unmap(address, copy->second.block.size);
            m.free(copy->second.block);
            copies_.erase(copy);
        }
        b.reply(command);
        return true;
    }
    default:
        return false;
    }
}

} // namespace runtime
