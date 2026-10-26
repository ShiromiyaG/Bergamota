// What gsp asks of the GPU. A renderer plugs in here: for now Zakuro's,
// through a C interface, later the native Vulkan one. NullGpu draws
// nothing: it does the memory work (fills, copies) so a game runs without
// a screen.

#ifndef RUNTIME_GPU_H
#define RUNTIME_GPU_H

#include <cstdint>

#include "runtime/memory.h"

namespace runtime {

struct Gpu {
    virtual ~Gpu() = default;
    // the GPU's registers, from 0x1EF00000 (offsets as gsp gets them)
    virtual void write_register(uint32_t offset, uint32_t value) = 0;
    virtual uint32_t read_register(uint32_t offset) = 0;
    // a list of PICA200 commands, at a physical address
    virtual void command_list(uint32_t paddr, uint32_t size) = 0;
    virtual void memory_fill(uint32_t start, uint32_t end, uint32_t value, uint32_t width) = 0;
    virtual void display_transfer(uint32_t input, uint32_t output, uint32_t input_size, uint32_t output_size,
                                  uint32_t flags) = 0;
    virtual void texture_copy(uint32_t input, uint32_t output, uint32_t size, uint32_t input_gap,
                              uint32_t output_gap) = 0;
    // a screen's picture from now on: physical addresses
    virtual void framebuffer(uint32_t screen, uint32_t index, uint32_t left, uint32_t right, uint32_t stride,
                             uint32_t format) = 0;
};

class NullGpu : public Gpu {
public:
    explicit NullGpu(Memory &memory) : memory_(memory) {}
    void write_register(uint32_t offset, uint32_t value) override;
    uint32_t read_register(uint32_t offset) override;
    void command_list(uint32_t, uint32_t) override {}
    void memory_fill(uint32_t start, uint32_t end, uint32_t value, uint32_t width) override;
    void display_transfer(uint32_t, uint32_t, uint32_t, uint32_t, uint32_t) override {}
    void texture_copy(uint32_t input, uint32_t output, uint32_t size, uint32_t input_gap,
                      uint32_t output_gap) override;
    void framebuffer(uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t) override {}

private:
    Memory &memory_;
    uint32_t registers_[0x1000]{}; // the registers games read back
};

} // namespace runtime

#endif
