// The GPU on a thread of its own, as the 3DS's runs beside its CPU: gsp's
// commands (command lists, fills, transfers, copies, register writes) go
// into a queue and the game goes on; a thread runs them on the renderer in
// order, and their interrupts (P3D, PSC0/1, PPF) come back to the game's
// thread, which gsp hands on at its next tick. What the CPU reads back (a
// register, memory the GPU draws into, a screen) waits for the queue first.

#ifndef RUNTIME_ASYNC_GPU_H
#define RUNTIME_ASYNC_GPU_H

#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

#include "runtime/gpu.h"

namespace runtime {

class AsyncGpu : public Gpu {
public:
    // inner: the renderer the thread drives (pica::Pica); memory: its page
    // guards, whose syncs wait for the queue
    AsyncGpu(Gpu &inner, Memory &memory);
    ~AsyncGpu() override;

    void write_register(uint32_t offset, uint32_t value) override;
    uint32_t read_register(uint32_t offset) override;
    void command_list(uint32_t paddr, uint32_t size) override;
    void memory_fill(uint32_t start, uint32_t end, uint32_t value, uint32_t width) override;
    void display_transfer(uint32_t input, uint32_t output, uint32_t input_size, uint32_t output_size,
                          uint32_t flags) override;
    void texture_copy(uint32_t input, uint32_t output, uint32_t size, uint32_t input_gap, uint32_t output_gap) override;
    void framebuffer(uint32_t screen, uint32_t index, uint32_t left, uint32_t right, uint32_t stride,
                     uint32_t format) override;
    void sync(uint32_t paddr, uint32_t size) override;
    bool deferred() const override { return true; }
    void take_interrupts(std::vector<uint8_t> &out) override;
    void after(uint8_t interrupt) override;
    void vblank() override;
    // the renderer's frames: they keep their own lock
    bool next_frame(Frame &out) override { return inner_.next_frame(out); }

    // runs f with the GPU idle and to itself
    void locked(const std::function<void()> &f);
    // nothing queued, running, or done and not yet taken
    bool settled();
    // seconds the game's thread waited for the GPU
    double waited = 0;
    // its thread, for the sampler
    void *native_thread() { return thread_.native_handle(); }

private:
    void push(uint8_t interrupt, std::function<void()> work);
    void drain(); // waits until the queue is empty and the thread idle
    void run();

    Gpu &inner_;
    Memory &memory_;
    std::function<void(uint32_t, uint32_t, bool)> guarded_; // the renderer's own page-guard sync
    std::mutex lock_;      // the queue, the interrupts done, busy_
    std::mutex gpu_;       // the renderer: one user at a time
    std::condition_variable wake_, idle_;
    struct Item {
        uint8_t interrupt; // 0xFF for none
        std::function<void()> work;
    };
    std::deque<Item> queue_;
    std::vector<uint8_t> done_;
    bool busy_ = false, stop_ = false;
    std::thread thread_;
};

} // namespace runtime

#endif
