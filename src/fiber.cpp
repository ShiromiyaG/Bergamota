// The game's threads' stacks (fiber.h).

#include "runtime/fiber.h"

#include <cstdio>
#include <cstdlib>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#else
#include <sys/mman.h>
#include <ucontext.h>
#endif

namespace runtime {

#ifdef _WIN32

struct Fiber::Impl {
    void *fiber = nullptr;
    bool converted = false; // the thread's own, made a fiber
    std::function<void()> body;
};

namespace {

void CALLBACK start(void *parameter) {
    auto *impl = static_cast<Fiber::Impl *>(parameter);
    impl->body();
    std::fprintf(stderr, "fiber: a body returned\n");
    std::abort();
}

} // namespace

Fiber::Fiber() : impl_(std::make_unique<Impl>()) {}

std::unique_ptr<Fiber> Fiber::main() {
    std::unique_ptr<Fiber> fiber(new Fiber());
    fiber->impl_->fiber = IsThreadAFiber() ? GetCurrentFiber() : ConvertThreadToFiber(nullptr);
    fiber->impl_->converted = true;
    return fiber;
}

std::unique_ptr<Fiber> Fiber::make(size_t size, std::function<void()> body) {
    std::unique_ptr<Fiber> fiber(new Fiber());
    fiber->impl_->body = std::move(body);
    // the stack reserved whole, committed a page at a time as it grows
    fiber->impl_->fiber = CreateFiberEx(64 * 1024, size, FIBER_FLAG_FLOAT_SWITCH, start, fiber->impl_.get());
    if (!fiber->impl_->fiber) {
        std::fprintf(stderr, "fiber: no stack of %zu bytes\n", size);
        std::abort();
    }
    return fiber;
}

Fiber::~Fiber() {
    if (impl_ && impl_->fiber && !impl_->converted) DeleteFiber(impl_->fiber);
}

void Fiber::switch_from(Fiber &) { SwitchToFiber(impl_->fiber); }

#else

struct Fiber::Impl {
    ucontext_t context{};
    void *stack = nullptr;
    size_t size = 0;
    std::function<void()> body;
};

namespace {

void start(unsigned high, unsigned low) {
    auto *impl = reinterpret_cast<Fiber::Impl *>((uintptr_t(high) << 32) | uintptr_t(low));
    impl->body();
    std::fprintf(stderr, "fiber: a body returned\n");
    std::abort();
}

} // namespace

Fiber::Fiber() : impl_(std::make_unique<Impl>()) {}

std::unique_ptr<Fiber> Fiber::main() {
    std::unique_ptr<Fiber> fiber(new Fiber());
    getcontext(&fiber->impl_->context);
    return fiber;
}

std::unique_ptr<Fiber> Fiber::make(size_t size, std::function<void()> body) {
    std::unique_ptr<Fiber> fiber(new Fiber());
    Impl &impl = *fiber->impl_;
    impl.body = std::move(body);
    impl.size = size;
    impl.stack = mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    if (impl.stack == MAP_FAILED) {
        std::fprintf(stderr, "fiber: no stack of %zu bytes\n", size);
        std::abort();
    }
    getcontext(&impl.context);
    impl.context.uc_stack.ss_sp = impl.stack;
    impl.context.uc_stack.ss_size = size;
    impl.context.uc_link = nullptr;
    uintptr_t self = reinterpret_cast<uintptr_t>(&impl);
    makecontext(&impl.context, reinterpret_cast<void (*)()>(start), 2, unsigned(self >> 32), unsigned(self & 0xFFFFFFFFu));
    return fiber;
}

Fiber::~Fiber() {
    if (impl_ && impl_->stack) munmap(impl_->stack, impl_->size);
}

void Fiber::switch_from(Fiber &from) { swapcontext(&from.impl_->context, &impl_->context); }

#endif

} // namespace runtime
