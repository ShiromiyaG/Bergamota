// What gsp asks of the GPU. A renderer plugs in here: for now Zakuro's,
// through a C interface, later the native Vulkan one. NullGpu draws
// nothing: it does the memory work (fills, copies) so a game runs without
// a screen.

#ifndef RUNTIME_GPU_H
#define RUNTIME_GPU_H

#include <cstdint>
#include <vector>

#include "runtime/memory.h"

namespace runtime {

// a screen's picture as it shows now, upright RGBA (400x240 or 320x240)
struct Picture {
    uint32_t width = 0, height = 0;
    std::vector<uint8_t> rgba;
};

// what both screens showed at a vertical blank, the game's frame or one
// drawn between two of them (VulkanRenderer::between)
struct Frame {
    uint64_t number = 0; // the vertical blank
    Picture screens[2];  // when asked for (VulkanRenderer::cpu_pictures)
    bool between = false;
    // which of the pictures its blank shows, in order (more than one at
    // 120 Hz and up: VulkanRenderer::between)
    int part = 0;
    double busy = 0; // the GPU's seconds of work so far, for the stats
    // the same on the host's GPU, for a presenter there: which of the
    // renderer's sets of screen images, the size of each screen in it (the
    // framebuffer's, turned on its side), and the submission that fills it
    int set = -1;
    uint32_t size[2][2]{};
    uint64_t ready = 0;
};

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
    // memory over a physical range made right before something other than
    // the GPU (a service, a DMA) writes or reads it: a GPU that keeps what it
    // draws, and the textures it read, elsewhere puts it back or drops it
    virtual void sync(uint32_t, uint32_t) {}
    // a GPU that runs beside the game (AsyncGpu): gsp queues each command's
    // interrupt with after(), and takes those done at its tick
    virtual bool deferred() const { return false; }
    virtual void after(uint8_t) {}
    virtual void take_interrupts(std::vector<uint8_t> &) {}
    // a vertical blank, in order with the rest: what the screens show now
    // becomes a frame
    virtual void vblank() {}
    // the frames since the last call, oldest first: false when none is
    // left. any thread may ask
    virtual bool next_frame(Frame &) { return false; }
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

// reads the picture the LCD shows from memory: the 3DS keeps it turned on
// its side, in one of five formats
Picture read_screen(Memory &memory, uint32_t screen, uint32_t address, uint32_t stride, uint32_t format);


} // namespace runtime

#endif
