// gsp::Gpu. After Zakuro's services/gsp.rs.
// Zakuro: Rust, MIT, Copyright (c) 2026 fearkov; see LICENSE-zakuro.

#include "runtime/services.h"

#include "runtime/fs.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <cstdio>

namespace runtime {

namespace {
constexpr uint32_t GSP_SHARED_SIZE = 0x1000;
enum : uint8_t { PSC0, PSC1, PDC0, PDC1, PPF, P3D, DMA };
} // namespace

// where the game mapped the shared block, 0 before it did
uint32_t Services::gsp_shared(System &system) const {
    if (!gsp_.shared) return 0;
    auto *block = std::get_if<SharedMemory>(system.kernel.object(*gsp_.shared));
    return block && block->mapped_at ? *block->mapped_at : 0;
}

// an address the game hands gsp, in VRAM or its linear heap, as the
// physical one the GPU takes
uint32_t Services::gsp_physical(System &system, uint32_t vaddr) const {
    uint32_t linear = system.memory.linear_base;
    if (vaddr >= map::VRAM_VADDR && vaddr < map::VRAM_VADDR + map::VRAM_SIZE) return map::VRAM_PADDR + (vaddr - map::VRAM_VADDR);
    if (vaddr >= linear && vaddr - linear < 0x10000000) return map::FCRAM_PADDR + (vaddr - linear);
    return vaddr;
}

// a range of the game's memory made right for something other than the GPU
void Services::gpu_sync(System &system, uint32_t vaddr, uint32_t size) {
    if (gpu && size) gpu->sync(gsp_physical(system, vaddr), size);
}

// the interrupt for a GPU command just given: now, or once a GPU that runs
// beside the game got to it
void Services::gsp_done(System &system, uint8_t id) {
    if (gpu && gpu->deferred()) gpu->after(id);
    else gsp_interrupt(system, id);
}

// an interrupt into the client's queue (in the shared block), and its event
void Services::gsp_interrupt(System &system, uint8_t id) {
    uint32_t queue = gsp_shared(system);
    if (!queue) return;
    Memory &m = system.memory;
    uint32_t index = m.read8(queue), count = m.read8(queue + 1);
    if (count >= 0x34) {
        m.write8(queue + 2, 1); // full: the error flag, and it is dropped
        return;
    }
    m.write8(queue + 0xC + (index + count) % 0x34, id);
    m.write8(queue + 1, uint8_t(count + 1));
    if (gsp_.event) system.kernel.signal_event(*gsp_.event);
}

// the screens take the pictures queued for them in the shared block; a
// ring held until now runs
void Services::gsp_refresh(System &system) {
    uint32_t base = gsp_shared(system);
    if (!base) return;
    Memory &m = system.memory;
    for (uint32_t screen = 0; screen < 2; screen++) {
        uint32_t record = base + 0x200 + screen * 0x40, head = m.read32(record);
        if (!((head >> 8) & 0xFF)) continue;
        uint32_t index = head & 0xFF, entry = record + 4 + index * 0x1C;
        gpu->framebuffer(screen, index, gsp_physical(system, m.read32(entry + 4)), gsp_physical(system, m.read32(entry + 8)),
                         m.read32(entry + 0xC), m.read32(entry + 0x10));
        m.write32(record, index); // taken: the flag clears
        if (screen == 0) pictures++;
    }
    gsp_.waiting[0].reset();
    gsp_.waiting[1].reset();
    if (gsp_.held) {
        gsp_.held = false;
        gsp_queue(system);
    }
}

// the GX commands the game queued in the shared block's ring
void Services::gsp_queue(System &system) {
    uint32_t base = gsp_shared(system);
    if (!base) return;
    Memory &m = system.memory;
    uint32_t ring = base + 0x800;
    for (;;) {
        uint32_t control = m.read32(ring), index = control & 0xFF, count = (control >> 8) & 0xFF;
        if (!count) break;
        uint32_t entry = ring + 0x20 + (index % 15) * 0x20, id = m.read32(entry) & 0xFF, d[7];
        for (uint32_t i = 0; i < 7; i++) d[i] = m.read32(entry + 4 + i * 4);
        m.write32(ring, (index + 1) % 15 | (count - 1) << 8); // taken before it runs
        switch (id) {
        case 0x00: { // RequestDma(source, destination, size)
            gpu_sync(system, d[0], d[2]);
            gpu_sync(system, d[1], d[2]);
            std::vector<uint8_t> data(d[2]);
            m.read_bytes(d[0], data.data(), d[2]);
            m.write_bytes(d[1], data.data(), d[2]);
            gsp_interrupt(system, DMA);
            break;
        }
        case 0x01: // ProcessCommandList(address, size)
            gpu->command_list(gsp_physical(system, d[0] & ~7u), d[1] & ~3u);
            gsp_done(system, P3D);
            break;
        case 0x02: // MemoryFill: two banks
            for (uint32_t bank = 0; bank < 2; bank++) {
                uint32_t start = d[bank * 3], value = d[bank * 3 + 1], end = d[bank * 3 + 2];
                if (!start || end <= start) continue;
                uint32_t control2 = d[6] >> (bank * 16), width = (control2 & 0x300) == 0 ? 2 : (control2 & 0x300) == 0x100 ? 3 : 4;
                uint32_t p = gsp_physical(system, start);
                gpu->memory_fill(p, p + (end - start), value, width);
                gsp_done(system, bank ? PSC1 : PSC0);
            }
            break;
        case 0x03: // DisplayTransfer
            gpu->display_transfer(gsp_physical(system, d[0]), gsp_physical(system, d[1]), d[2], d[3], d[4]);
            gsp_done(system, PPF);
            break;
        case 0x04: // TextureCopy
            gpu->texture_copy(gsp_physical(system, d[0]), gsp_physical(system, d[1]), d[2], d[3], d[4]);
            gsp_done(system, PPF);
            break;
        default: break; // FlushCacheRegions and the rest
        }
    }
}

bool Services::gsp(System &system, CommandBuffer &b) {
    Kernel &k = system.kernel;
    Memory &m = system.memory;
    uint32_t command = b.command();
    switch (command) {
    case 0x0001: case 0x0002: { // WriteHWRegs(offset, size, <buffer>), WithMask(…, <mask>)
        uint32_t offset = b.get(1), size = std::min(b.get(2), 0x80u), source = b.get(4), mask = b.get(6);
        for (uint32_t i = 0; offset < 0x420000 && i < size / 4; i++) {
            uint32_t value = m.read32(source + i * 4), reg = offset + i * 4;
            if (command == 0x0002) {
                uint32_t mk = m.read32(mask + i * 4);
                value = (gpu->read_register(reg) & ~mk) | (value & mk);
            }
            gpu->write_register(reg, value);
        }
        b.reply(command);
        return true;
    }
    case 0x0003: { // WriteHWRegRepeat: one register, many times
        for (uint32_t i = 0; i < b.get(2) / 4; i++) gpu->write_register(b.get(1), m.read32(b.get(4) + i * 4));
        b.reply(command);
        return true;
    }
    case 0x0004: { // ReadHWRegs(offset, size)
        for (uint32_t i = 0; i < b.get(2) / 4; i++) m.write32(b.get(4) + i * 4, gpu->read_register(b.get(1) + i * 4));
        b.reply(command);
        return true;
    }
    case 0x0005: // SetBufferSwap(screen, active, left, right, stride, format)
        gpu->framebuffer(b.get(1), b.get(2), gsp_physical(system, b.get(3)), gsp_physical(system, b.get(4)), b.get(5), b.get(6));
        b.reply(command);
        return true;
    case 0x0006: // SetCommandList
        gpu->command_list(gsp_physical(system, b.get(1) & ~7u), b.get(2) & ~3u);
        gsp_done(system, P3D);
        b.reply(command);
        return true;
    case 0x0007: { // RequestDma
        gpu_sync(system, b.get(1), b.get(3));
        gpu_sync(system, b.get(2), b.get(3));
        std::vector<uint8_t> data(b.get(3));
        m.read_bytes(b.get(1), data.data(), b.get(3));
        m.write_bytes(b.get(2), data.data(), b.get(3));
        gsp_interrupt(system, DMA);
        b.reply(command);
        return true;
    }
    case 0x000C: { // TriggerCmdReqQueue: unless the game drew past what the screens can show
        uint32_t base = gsp_shared(system);
        if (base && !gsp_.held) {
            for (uint32_t screen = 0; screen < 2; screen++) {
                uint32_t record = base + 0x200 + screen * 0x40, head = m.read32(record);
                if (!((head >> 8) & 0xFF)) continue;
                std::pair<uint32_t, uint32_t> now{head & 0xFF, m.read32(record + 4 + (head & 0xFF) * 0x1C + 4)};
                if (!gsp_.waiting[screen]) gsp_.waiting[screen] = now;
                else if (*gsp_.waiting[screen] != now) gsp_.held = true;
            }
        }
        if (!gsp_.held) gsp_queue(system);
        b.reply(command);
        return true;
    }
    case 0x000D: // SetDisplayTransfer
        gpu->display_transfer(gsp_physical(system, b.get(1)), gsp_physical(system, b.get(2)), b.get(3), b.get(4), b.get(5));
        gsp_done(system, PPF);
        b.reply(command);
        return true;
    case 0x000E: // SetTextureCopy
        gpu->texture_copy(gsp_physical(system, b.get(1)), gsp_physical(system, b.get(2)), b.get(3), b.get(4), b.get(5));
        gsp_done(system, PPF);
        b.reply(command);
        return true;
    case 0x000F: // SetMemoryFill(start0, value0, end0, start1, value1, end1, control0, control1)
        for (uint32_t bank = 0; bank < 2; bank++) {
            uint32_t start = b.get(1 + bank * 3), value = b.get(2 + bank * 3), end = b.get(3 + bank * 3), control = b.get(7 + bank);
            if (!start || end <= start) continue;
            uint32_t width = (control & 0x300) == 0 ? 2 : (control & 0x300) == 0x100 ? 3 : 4, p = gsp_physical(system, start);
            gpu->memory_fill(p, p + (end - start), value, width);
            gsp_done(system, bank ? PSC1 : PSC0);
        }
        b.reply(command);
        return true;
    case 0x0013: { // RegisterInterruptRelayQueue(flags, event): the client's slot and the shared block
        if (gsp_.event) k.release_hold(*gsp_.event);
        gsp_.event = k.resolve(b.get(3));
        if (gsp_.event) k.hold(*gsp_.event);
        if (!gsp_.shared) {
            auto block = m.allocate(Region::Base, GSP_SHARED_SIZE);
            std::memset(m.host(block->paddr, GSP_SHARED_SIZE), 0, GSP_SHARED_SIZE);
            gsp_.shared = k.insert(SharedMemory{"GSP", 0, GSP_SHARED_SIZE, block->paddr, std::nullopt});
            k.hold(*gsp_.shared);
        }
        // 0x2A07 with the success: the GPU is set up already
        b.set(0, header(command, 2, 2));
        b.set(1, 0x2A07);
        b.set(2, 0); // the first client's slot
        b.set(3, descriptor::handles(1));
        b.set(4, k.create_handle(*gsp_.shared));
        return true;
    }
    case 0x0014: // UnregisterInterruptRelayQueue
        if (gsp_.event) k.release_hold(*gsp_.event);
        gsp_.event.reset();
        b.reply(command);
        return true;
    case 0x0012: // GetPerfLog
        b.reply(command, {0});
        return true;
    case 0x0008: case 0x0009: case 0x001F: // the caches: memory is coherent here
    case 0x000A: case 0x000B: // RegisterInterruptEvents, SetLcdForceBlack
    case 0x0010: case 0x0011: case 0x0015: case 0x0016: case 0x0017: // QoS, perf log, rights
    case 0x0019: case 0x001A: case 0x001B: case 0x001C: case 0x001E: // VRAM areas, reset, LED, priorities
        b.reply(command);
        return true;
    default:
        return false;
    }
}

} // namespace runtime
