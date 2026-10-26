// The guest's memory and the memory syscalls. Translated from Zakuro's
// memory/mod.rs, memory/physical.rs, memory/config.rs and kernel/svc.rs
// (Rust, MIT, Copyright (c) 2026 fearkov; see LICENSE-zakuro).

#include "runtime/memory.h"

#include <algorithm>
#include <cstring>

namespace runtime {

using namespace map;

namespace {

uint32_t align_up(uint32_t value, uint32_t align) { return (value + align - 1) & ~(align - 1); }

void put32(uint8_t *page, uint32_t offset, uint32_t value) { std::memcpy(page + offset, &value, 4); }
void put64(uint8_t *page, uint32_t offset, uint64_t value) { std::memcpy(page + offset, &value, 8); }

constexpr uint32_t MEMOP_FREE = 1, MEMOP_RESERVE = 2, MEMOP_ALLOC = 3, MEMOP_MAP = 4, MEMOP_UNMAP = 5,
                   MEMOP_PROTECT = 6, MEMOP_LINEAR = 0x10000;
constexpr uint32_t INVALID_ADDRESS = 0xE0E01BF5, MISALIGNED_SIZE = 0xE0E01BF2;

} // namespace

Memory::Memory(bool new3ds, uint32_t app_bytes)
    : fcram_(new3ds ? FCRAM_SIZE_NEW3DS : FCRAM_SIZE_OLD3DS), vram_(VRAM_SIZE), dsp_(DSP_RAM_SIZE),
      axi_(AXI_WRAM_SIZE), read_(PAGE_COUNT, nullptr), write_(PAGE_COUNT, nullptr) {
    uint32_t total = static_cast<uint32_t>(fcram_.size());
    uint32_t app = std::min(app_bytes, total);
    // the retail kernel gives SYSTEM 32 MiB and BASE the rest
    uint32_t system = std::min(total - app, 32u << 20);
    uint32_t base = total - app - system;
    allocated_.assign(total / PAGE_SIZE, false);
    regions_[0] = {0, app / PAGE_SIZE};
    regions_[1] = {app / PAGE_SIZE, system / PAGE_SIZE};
    regions_[2] = {(app + system) / PAGE_SIZE, base / PAGE_SIZE};
}

std::optional<Block> Memory::allocate(Region region, uint32_t size, bool top) {
    uint32_t needed = align_up(size, PAGE_SIZE) / PAGE_SIZE;
    if (needed == 0) {
        return Block{FCRAM_PADDR, 0};
    }
    auto [start, count] = regions_[static_cast<int>(region)];
    uint32_t run = 0;
    for (uint32_t k = 0; k < count; k++) {
        // the code, stack and heap from the top, the linear heap from the
        // bottom, so the linear heap's addresses are the console's
        uint32_t i = top ? start + count - 1 - k : start + k;
        if (allocated_[i]) {
            run = 0;
            continue;
        }
        if (++run == needed) {
            uint32_t first = top ? i : i + 1 - needed;
            std::fill(allocated_.begin() + first, allocated_.begin() + first + needed, true);
            return Block{FCRAM_PADDR + first * PAGE_SIZE, needed * PAGE_SIZE};
        }
    }
    return std::nullopt;
}

void Memory::free(Block block) {
    if (block.paddr < FCRAM_PADDR) {
        return;
    }
    size_t first = (block.paddr - FCRAM_PADDR) / PAGE_SIZE;
    size_t end = std::min(first + block.size / PAGE_SIZE, allocated_.size());
    std::fill(allocated_.begin() + first, allocated_.begin() + end, false);
}

uint32_t Memory::region_used(Region region) const {
    auto [start, count] = regions_[static_cast<int>(region)];
    return static_cast<uint32_t>(std::count(allocated_.begin() + start, allocated_.begin() + start + count, true)) *
           PAGE_SIZE;
}

uint8_t *Memory::host(uint32_t paddr, uint32_t size) {
    auto in = [&](uint32_t base, std::vector<uint8_t> &store) -> uint8_t * {
        uint64_t offset = paddr - base;
        return offset + size <= store.size() ? store.data() + offset : nullptr;
    };
    if (paddr >= VRAM_PADDR && paddr < VRAM_PADDR + VRAM_SIZE) return in(VRAM_PADDR, vram_);
    if (paddr >= DSP_RAM_PADDR && paddr < DSP_RAM_PADDR + DSP_RAM_SIZE) return in(DSP_RAM_PADDR, dsp_);
    if (paddr >= AXI_WRAM_PADDR && paddr < AXI_WRAM_PADDR + AXI_WRAM_SIZE) return in(AXI_WRAM_PADDR, axi_);
    if (paddr >= FCRAM_PADDR) return in(FCRAM_PADDR, fcram_);
    return nullptr;
}

void Memory::map(uint32_t vaddr, uint32_t paddr, uint32_t size, uint32_t permission, MemoryState state) {
    // services hand out blocks sized to their contents: rounded up to pages
    vaddr &= ~PAGE_MASK;
    size = align_up(size, PAGE_SIZE);
    for (uint32_t page = 0; page < size / PAGE_SIZE; page++) {
        uint8_t *at = host(paddr + page * PAGE_SIZE, PAGE_SIZE);
        if (!at) {
            continue;
        }
        size_t index = (vaddr >> PAGE_BITS) + page;
        if (permission & READ) read_[index] = at;
        if (permission & WRITE) write_[index] = at;
    }
    mappings_[vaddr] = Mapping{vaddr, size, paddr, permission, state};
}

void Memory::unmap(uint32_t vaddr, uint32_t size) {
    for (uint32_t page = 0; page < size / PAGE_SIZE; page++) {
        size_t index = (vaddr >> PAGE_BITS) + page;
        read_[index] = write_[index] = nullptr;
    }
    // a mapping the range covers in part keeps the rest, at its own pages
    uint64_t end = uint64_t(vaddr) + size;
    std::vector<Mapping> touched;
    for (auto &[base, m] : mappings_) {
        if (base < end && vaddr < uint64_t(m.base) + m.size) touched.push_back(m);
    }
    for (const Mapping &m : touched) {
        mappings_.erase(m.base);
        if (m.base < vaddr) {
            Mapping left = m;
            left.size = vaddr - m.base;
            mappings_[m.base] = left;
        }
        uint64_t m_end = uint64_t(m.base) + m.size;
        if (m_end > end) {
            Mapping right = m;
            right.base = static_cast<uint32_t>(end);
            right.size = static_cast<uint32_t>(m_end - end);
            right.paddr = m.paddr + (right.base - m.base);
            mappings_[right.base] = right;
        }
    }
}

const Mapping *Memory::mapping_at(uint32_t vaddr) const {
    auto it = mappings_.upper_bound(vaddr);
    if (it == mappings_.begin()) return nullptr;
    --it;
    return vaddr - it->second.base < it->second.size ? &it->second : nullptr;
}

Mapping Memory::query(uint32_t vaddr) const {
    if (const Mapping *m = mapping_at(vaddr)) return *m;
    // the free gap around it, which allocators walking the space expect
    uint32_t start = 0, end = 0xFFFFF000;
    auto it = mappings_.upper_bound(vaddr);
    if (it != mappings_.end()) end = it->first;
    if (it != mappings_.begin()) {
        --it;
        start = it->second.base + it->second.size;
    }
    return Mapping{start, end - start, 0, 0, MemoryState::Free};
}

// the slow paths, where the generated code's page tables have nothing
uint8_t Memory::read8(uint32_t a) {
    if (uint8_t *page = read_[a >> PAGE_BITS]) return page[a & PAGE_MASK];
    faults_[a & ~PAGE_MASK]++;
    return 0;
}

uint16_t Memory::read16(uint32_t a) { return read8(a) | read8(a + 1) << 8; }

uint32_t Memory::read32(uint32_t a) {
    uint8_t *page = read_[a >> PAGE_BITS];
    if (page && (a & PAGE_MASK) <= PAGE_SIZE - 4) {
        uint32_t v;
        std::memcpy(&v, page + (a & PAGE_MASK), 4);
        return v;
    }
    return read16(a) | uint32_t(read16(a + 2)) << 16;
}

void Memory::write8(uint32_t a, uint8_t v) {
    if (uint8_t *page = write_[a >> PAGE_BITS]) page[a & PAGE_MASK] = v;
    else faults_[a & ~PAGE_MASK]++;
}

void Memory::write16(uint32_t a, uint16_t v) {
    write8(a, uint8_t(v));
    write8(a + 1, uint8_t(v >> 8));
}

void Memory::write32(uint32_t a, uint32_t v) {
    uint8_t *page = write_[a >> PAGE_BITS];
    if (page && (a & PAGE_MASK) <= PAGE_SIZE - 4) {
        std::memcpy(page + (a & PAGE_MASK), &v, 4);
        return;
    }
    write16(a, uint16_t(v));
    write16(a + 2, uint16_t(v >> 16));
}

void Memory::read_bytes(uint32_t a, void *out, uint32_t size) {
    auto *o = static_cast<uint8_t *>(out);
    for (uint32_t i = 0; i < size; i++) o[i] = read8(a + i);
}

void Memory::write_bytes(uint32_t a, const void *in, uint32_t size) {
    auto *b = static_cast<const uint8_t *>(in);
    for (uint32_t i = 0; i < size; i++) write8(a + i, b[i]);
}

std::string Memory::read_cstring(uint32_t a, uint32_t max) {
    std::string s;
    for (uint32_t i = 0; i < max; i++) {
        char c = static_cast<char>(read8(a + i));
        if (!c) break;
        s.push_back(c);
    }
    return s;
}

void Memory::init_system_pages(uint64_t boot_clock_ms) {
    uint8_t *config = config_page();
    std::memset(config, 0, PAGE_SIZE);
    // firmware 11.x, what every retail console ended on
    config[0x01] = 0x34;
    config[0x02] = 0x02;
    put32(config, 0x10, 2);       // syscore version
    config[0x14] = 1;             // production
    config[0x15] = 1;             // retail unit
    put32(config, 0x18, 0xF297);  // CTR SDK version
    put32(config, 0x40, regions_[0].second * PAGE_SIZE);
    put32(config, 0x44, regions_[1].second * PAGE_SIZE);
    put32(config, 0x48, regions_[2].second * PAGE_SIZE);
    config[0x61] = 0x34;
    config[0x62] = 0x02;
    put32(config, 0x64, 2);
    put32(config, 0x68, 0xF297);

    uint8_t *shared = shared_page();
    std::memset(shared, 0, PAGE_SIZE);
    shared[0x04] = 1;                  // retail hardware
    shared[0x05] = 1;                  // Old 3DS
    const uint8_t mac[6] = {0x40, 0xF4, 0x07, 0x00, 0x00, 0x01};
    std::memcpy(shared + 0x60, mac, 6);
    shared[0x85] = 1 | 5 << 2;         // on the adapter, full
    update_clock(boot_clock_ms, 0);

    // both pages are mapped read-only, at their fixed addresses
    map(CONFIG_MEM_VADDR, AXI_WRAM_PADDR, PAGE_SIZE, READ, MemoryState::Shared);
    map(SHARED_PAGE_VADDR, AXI_WRAM_PADDR + PAGE_SIZE, PAGE_SIZE, READ, MemoryState::Shared);
}

void Memory::update_clock(uint64_t boot_clock_ms, uint64_t ticks) {
    // the clock at boot moved on by the ticks since: titles extrapolate from
    // it with the tick, so it has to follow the ticks, not the host
    uint64_t date_time = boot_clock_ms + ticks / (CPU_CLOCK_HZ / 1000);
    uint8_t *shared = shared_page();
    for (uint32_t slot : {0x20u, 0x40u}) {
        put64(shared, slot, date_time);
        put64(shared, slot + 0x08, ticks);
        put32(shared, slot + 0x10, static_cast<uint32_t>(CPU_CLOCK_HZ));
        put32(shared, slot + 0x14, 0);
        put64(shared, slot + 0x18, 0);
    }
}

void memory_syscall(Memory &memory, Kernel &kernel, Thread &thread, uint32_t number, Region region) {
    Context &c = thread.context;
    switch (number) {
    case 0x01: { // ControlMemory: r0 operation, r1 addr0, r2 addr1, r3 size, r4 permission
        uint32_t op = c.r[0] & 0xFF, addr0 = c.r[1], addr1 = c.r[2], size = c.r[3];
        uint32_t permission = (c.r[4] & 7) | READ;
        if (size & PAGE_MASK) {
            c.r[0] = MISALIGNED_SIZE;
            return;
        }
        if (op == MEMOP_ALLOC && (c.r[0] & MEMOP_LINEAR)) {
            auto block = memory.allocate(region, size);
            if (!block) {
                c.r[0] = result::OUT_OF_MEMORY;
                return;
            }
            // the guest works out physical addresses from these: the offset
            // has to be exact
            uint32_t vaddr = memory.linear_base + (block->paddr - FCRAM_PADDR);
            memory.map(vaddr, block->paddr, block->size, permission, MemoryState::Continuous);
            c.r[0] = result::SUCCESS;
            c.r[1] = vaddr;
        } else if (op == MEMOP_ALLOC) {
            uint32_t base = addr0 ? addr0 : memory.heap_top;
            auto block = memory.allocate(region, size, true);
            if (!block) {
                c.r[0] = result::OUT_OF_MEMORY;
                return;
            }
            memory.map(base, block->paddr, block->size, permission, MemoryState::Private);
            memory.heap_top = std::max(memory.heap_top, base + block->size);
            c.r[0] = result::SUCCESS;
            c.r[1] = base;
        } else if (op == MEMOP_FREE) {
            // the pages behind each heap or linear mapping the range covers
            uint64_t end = uint64_t(addr0) + size;
            std::vector<Block> freed;
            for (auto &[base, m] : memory.mappings()) {
                if ((m.state != MemoryState::Private && m.state != MemoryState::Continuous) || base >= end ||
                    addr0 >= uint64_t(m.base) + m.size) {
                    continue;
                }
                uint32_t start = std::max(m.base, addr0);
                uint64_t stop = std::min<uint64_t>(uint64_t(m.base) + m.size, end);
                freed.push_back(Block{m.paddr + (start - m.base), static_cast<uint32_t>(stop - start)});
            }
            for (Block b : freed) memory.free(b);
            memory.unmap(addr0, size);
            c.r[0] = result::SUCCESS;
            c.r[1] = addr0;
        } else if (op == MEMOP_MAP) {
            const Mapping *m = memory.mapping_at(addr1);
            if (!m) {
                c.r[0] = INVALID_ADDRESS;
            } else {
                memory.map(addr0, m->paddr + (addr1 - m->base), size, permission | RW, MemoryState::Aliased);
                c.r[0] = result::SUCCESS;
            }
            c.r[1] = addr0;
        } else if (op == MEMOP_UNMAP) {
            memory.unmap(addr0, size);
            c.r[0] = result::SUCCESS;
            c.r[1] = addr0;
        } else if (op == MEMOP_PROTECT || op == MEMOP_RESERVE) {
            c.r[0] = result::SUCCESS; // permissions are not enforced
            c.r[1] = addr0;
        } else {
            c.r[0] = result::INVALID_ENUM_VALUE;
        }
        return;
    }
    case 0x02: { // QueryMemory: r2 address
        Mapping m = memory.query(c.r[2]);
        c.r[0] = result::SUCCESS;
        c.r[1] = m.base;
        c.r[2] = m.size;
        c.r[3] = m.permission;
        c.r[4] = static_cast<uint32_t>(m.state);
        c.r[5] = 0;
        return;
    }
    case 0x1E: { // CreateMemoryBlock: r1 address, r2 size
        uint32_t addr = c.r[1], size = c.r[2], paddr;
        if (addr == 0) {
            // the kernel takes the pages from BASE
            auto block = memory.allocate(Region::Base, size);
            if (!block) {
                c.r[0] = result::OUT_OF_MEMORY;
                return;
            }
            std::memset(memory.host(block->paddr, block->size), 0, block->size);
            paddr = block->paddr;
        } else {
            const Mapping *m = memory.mapping_at(addr);
            if (!m) {
                c.r[0] = INVALID_ADDRESS;
                return;
            }
            paddr = m->paddr + (addr - m->base);
        }
        c.r[0] = result::SUCCESS;
        c.r[1] = kernel.create_handle(kernel.insert(SharedMemory{"MemoryBlock", addr, size, paddr, std::nullopt}));
        return;
    }
    case 0x1F: { // MapMemoryBlock: r0 handle, r1 address, r2 own permission
        auto id = kernel.resolve(c.r[0]);
        auto *block = id ? std::get_if<SharedMemory>(kernel.object(*id)) : nullptr;
        if (!block) {
            c.r[0] = result::INVALID_HANDLE;
            return;
        }
        uint32_t addr = c.r[1];
        if (addr == 0 && block->address != 0) {
            addr = block->address; // a block the system keeps where it is
        } else if (addr == 0) {
            // the first place in the shared memory region it fits whole
            uint32_t pages = align_up(block->size, PAGE_SIZE) / PAGE_SIZE;
            addr = SHARED_MEMORY_VADDR;
            for (;;) {
                bool free = true;
                for (uint32_t p = 0; p < pages && free; p++) free = !memory.mapping_at(addr + p * PAGE_SIZE);
                if (free) break;
                addr += PAGE_SIZE;
            }
        }
        memory.map(addr, block->paddr, block->size, (c.r[2] & 7) | READ, MemoryState::Shared);
        block->mapped_at = addr;
        c.r[0] = result::SUCCESS;
        return;
    }
    case 0x20: { // UnmapMemoryBlock: r0 handle, r1 address
        auto id = kernel.resolve(c.r[0]);
        if (auto *block = id ? std::get_if<SharedMemory>(kernel.object(*id)) : nullptr) {
            memory.unmap(c.r[1], align_up(block->size, PAGE_SIZE));
        }
        c.r[0] = result::SUCCESS;
        return;
    }
    case 0x2B: { // GetProcessInfo: r2 type
        uint64_t value = 0;
        if (c.r[2] == 2) value = memory.region_used(region);
        // what turns a linear address into a physical one
        if (c.r[2] == 20) value = FCRAM_PADDR - memory.linear_base;
        c.r[0] = result::SUCCESS;
        c.r[1] = static_cast<uint32_t>(value);
        c.r[2] = static_cast<uint32_t>(value >> 32);
        return;
    }
    default:
        c.r[0] = result::NOT_IMPLEMENTED;
    }
}

} // namespace runtime
