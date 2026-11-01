// The GPU on a thread of its own (include/runtime/async_gpu.h).

#include "runtime/async_gpu.h"

#include <chrono>

namespace runtime {

namespace {
// no interrupt after an item
enum : uint8_t { NONE = 0xFF };
} // namespace

AsyncGpu::AsyncGpu(Gpu &inner, Memory &memory) : inner_(inner), memory_(memory) {
    // the CPU touching what the GPU drew: the queue runs out first, then the
    // renderer brings it down, alone
    guarded_ = memory_.gpu_sync;
    if (guarded_) {
        memory_.gpu_sync = [this](uint32_t vaddr, uint32_t size, bool write) {
            drain();
            std::lock_guard g(gpu_);
            guarded_(vaddr, size, write);
        };
    }
    thread_ = std::thread([this] { run(); });
}

AsyncGpu::~AsyncGpu() {
    {
        std::lock_guard g(lock_);
        stop_ = true;
    }
    wake_.notify_all();
    thread_.join();
    if (guarded_) memory_.gpu_sync = guarded_;
}

void AsyncGpu::run() {
    for (;;) {
        Item item;
        {
            std::unique_lock g(lock_);
            wake_.wait(g, [this] { return stop_ || !queue_.empty(); });
            if (stop_ && queue_.empty()) return;
            item = std::move(queue_.front());
            queue_.pop_front();
            busy_ = true;
        }
        {
            std::lock_guard g(gpu_);
            item.work();
        }
        {
            std::lock_guard g(lock_);
            if (item.interrupt != NONE) done_.push_back(item.interrupt);
            busy_ = false;
        }
        idle_.notify_all();
    }
}

void AsyncGpu::push(uint8_t interrupt, std::function<void()> work) {
    {
        std::lock_guard g(lock_);
        queue_.push_back({interrupt, std::move(work)});
    }
    wake_.notify_one();
}

void AsyncGpu::drain() {
    auto started = std::chrono::steady_clock::now();
    std::unique_lock g(lock_);
    idle_.wait(g, [this] { return queue_.empty() && !busy_; });
    waited += std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
}

void AsyncGpu::locked(const std::function<void()> &f) {
    drain();
    std::lock_guard g(gpu_);
    f();
}

bool AsyncGpu::settled() {
    std::lock_guard g(lock_);
    return queue_.empty() && !busy_ && done_.empty();
}

void AsyncGpu::take_interrupts(std::vector<uint8_t> &out) {
    std::lock_guard g(lock_);
    out.insert(out.end(), done_.begin(), done_.end());
    done_.clear();
}

// a register write can start a fill or a transfer: in order with the rest
void AsyncGpu::write_register(uint32_t offset, uint32_t value) {
    push(NONE, [this, offset, value] { inner_.write_register(offset, value); });
}

uint32_t AsyncGpu::read_register(uint32_t offset) {
    drain();
    std::lock_guard g(gpu_);
    return inner_.read_register(offset);
}

void AsyncGpu::command_list(uint32_t paddr, uint32_t size) {
    push(NONE, [this, paddr, size] { inner_.command_list(paddr, size); });
}

void AsyncGpu::memory_fill(uint32_t start, uint32_t end, uint32_t value, uint32_t width) {
    push(NONE, [this, start, end, value, width] { inner_.memory_fill(start, end, value, width); });
}

void AsyncGpu::display_transfer(uint32_t input, uint32_t output, uint32_t input_size, uint32_t output_size,
                                uint32_t flags) {
    push(NONE, [=, this] { inner_.display_transfer(input, output, input_size, output_size, flags); });
}

void AsyncGpu::texture_copy(uint32_t input, uint32_t output, uint32_t size, uint32_t input_gap, uint32_t output_gap) {
    push(NONE, [=, this] { inner_.texture_copy(input, output, size, input_gap, output_gap); });
}

// the interrupt for what was queued before it, once that is done
void AsyncGpu::after(uint8_t interrupt) { push(interrupt, [] {}); }

// the screens taken in order: after the frame's last transfer, before the next
void AsyncGpu::vblank() { push(NONE, [this] { inner_.vblank(); }); }

void AsyncGpu::framebuffer(uint32_t screen, uint32_t index, uint32_t left, uint32_t right, uint32_t stride,
                           uint32_t format) {
    push(NONE, [=, this] { inner_.framebuffer(screen, index, left, right, stride, format); });
}

// memory a service or a DMA is about to use: everything queued before it
// done, then brought down
void AsyncGpu::sync(uint32_t paddr, uint32_t size) {
    drain();
    std::lock_guard g(gpu_);
    inner_.sync(paddr, size);
}

} // namespace runtime
