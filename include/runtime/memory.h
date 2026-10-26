// The guest's memory: the 3DS's physical memories, the page tables the
// games' code reads and writes through (Context::read_pages/write_pages),
// the mappings svcQueryMemory reports, and the memory syscalls.
// Translated from Zakuro's memory/ (Rust, MIT, Copyright (c) 2026 fearkov;
// see LICENSE-zakuro).

#ifndef RUNTIME_MEMORY_H
#define RUNTIME_MEMORY_H

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "runtime/kernel.h"

namespace runtime {

namespace map {
constexpr uint32_t PAGE_SIZE = 0x1000;
constexpr uint32_t PAGE_BITS = 12;
constexpr uint32_t PAGE_MASK = PAGE_SIZE - 1;
constexpr size_t PAGE_COUNT = size_t(1) << (32 - PAGE_BITS);

constexpr uint32_t VRAM_PADDR = 0x18000000, VRAM_SIZE = 0x00600000;
constexpr uint32_t DSP_RAM_PADDR = 0x1FF00000, DSP_RAM_SIZE = 0x00080000;
constexpr uint32_t AXI_WRAM_PADDR = 0x1FF80000, AXI_WRAM_SIZE = 0x00080000;
constexpr uint32_t FCRAM_PADDR = 0x20000000;
constexpr uint32_t FCRAM_SIZE_OLD3DS = 0x08000000, FCRAM_SIZE_NEW3DS = 0x10000000;

constexpr uint32_t CODE_VADDR = 0x00100000;
constexpr uint32_t HEAP_VADDR = 0x08000000;
constexpr uint32_t SHARED_MEMORY_VADDR = 0x10000000;
constexpr uint32_t LINEAR_VADDR_OLD = 0x14000000; // kernels before 8.0
constexpr uint32_t LINEAR_VADDR = 0x30000000;
constexpr uint32_t VRAM_VADDR = 0x1F000000;
constexpr uint32_t DSP_RAM_VADDR = 0x1FF00000;
constexpr uint32_t CONFIG_MEM_VADDR = 0x1FF80000;
constexpr uint32_t SHARED_PAGE_VADDR = 0x1FF81000;
constexpr uint32_t TLS_VADDR = 0x1FF82000;
constexpr uint32_t TLS_SIZE = 0x200;
constexpr uint32_t TLS_COMMAND_BUFFER = 0x80;
} // namespace map

enum class Region { Application, System, Base };

enum Permission : uint32_t { READ = 1, WRITE = 2, EXECUTE = 4, RW = 3 };

// MemoryState as svcQueryMemory reports it
enum class MemoryState : uint32_t {
    Free = 0, Reserved = 1, Io = 2, Static = 3, Code = 4, Private = 5, Shared = 6,
    Continuous = 7, Aliased = 8, Alias = 9, AliasCode = 10, Locked = 11,
};

struct Mapping {
    uint32_t base, size, paddr;
    uint32_t permission;
    MemoryState state;
};

struct Block {
    uint32_t paddr, size;
};

class Memory : public GuestMemory {
public:
    // app_bytes: the application region's size, from the exheader's system
    // mode
    Memory(bool new3ds, uint32_t app_bytes);

    uint8_t *const *read_pages() const { return read_.data(); }
    uint8_t *const *write_pages() const { return write_.data(); }

    // physical pages, from the bottom (the linear heap) or the top (code,
    // heap) of a region
    std::optional<Block> allocate(Region region, uint32_t size, bool top = false);
    void free(Block block);
    uint32_t region_used(Region region) const;
    uint8_t *host(uint32_t paddr, uint32_t size);

    void map(uint32_t vaddr, uint32_t paddr, uint32_t size, uint32_t permission, MemoryState state);
    void unmap(uint32_t vaddr, uint32_t size);
    const Mapping *mapping_at(uint32_t vaddr) const;
    Mapping query(uint32_t vaddr) const;
    const std::map<uint32_t, Mapping> &mappings() const { return mappings_; }

    uint8_t read8(uint32_t a);
    uint16_t read16(uint32_t a);
    uint32_t read32(uint32_t a) override;
    void write8(uint32_t a, uint8_t v);
    void write16(uint32_t a, uint16_t v);
    void write32(uint32_t a, uint32_t v) override;
    void read_bytes(uint32_t a, void *out, uint32_t size);
    void write_bytes(uint32_t a, const void *in, uint32_t size);
    std::string read_cstring(uint32_t a, uint32_t max);

    // the configuration page and the shared page (time, battery, 3D slider)
    uint8_t *config_page() { return axi_.data(); }
    uint8_t *shared_page() { return axi_.data() + 0x1000; }
    void init_system_pages(uint64_t boot_clock_ms);
    void update_clock(uint64_t boot_clock_ms, uint64_t ticks);

    // accesses to unmapped addresses, counted per address
    const std::map<uint32_t, uint32_t> &faults() const { return faults_; }

    uint32_t linear_base = map::LINEAR_VADDR;
    uint32_t heap_top = map::HEAP_VADDR;

private:
    std::vector<uint8_t> fcram_, vram_, dsp_, axi_;
    std::vector<uint8_t *> read_, write_;
    std::vector<bool> allocated_;
    std::pair<uint32_t, uint32_t> regions_[3]; // first page, page count
    std::map<uint32_t, Mapping> mappings_;
    std::map<uint32_t, uint32_t> faults_;
};

// the memory syscalls: ControlMemory, QueryMemory, CreateMemoryBlock,
// MapMemoryBlock, UnmapMemoryBlock, GetProcessInfo
void memory_syscall(Memory &memory, Kernel &kernel, Thread &thread, uint32_t number, Region region);

} // namespace runtime

#endif
