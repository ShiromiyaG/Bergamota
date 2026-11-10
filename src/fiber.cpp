// The game's threads' stacks (fiber.h): Windows' fibers; on Linux and
// Android a switch of our own (x86-64, AArch64), ucontext elsewhere.

#include "runtime/fiber.h"

#include <cstdio>
#include <cstdlib>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#else
#include <sys/mman.h>
#include <cstdint>
// x86-64 (System V) and AArch64 switch with a few instructions of our own
// (Linux, Android: Android's libc has no swapcontext); others with ucontext
#if (defined(__x86_64__) || defined(__aarch64__)) && !defined(RUNTIME_FIBER_UCONTEXT)
#define FIBER_ASM 1
#else
#include <ucontext.h>
#endif
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

#elif defined(FIBER_ASM)

// runtime_fiber_switch(&from_sp, to_sp): the callee-saved registers and the
// floating point controls pushed on the running stack, its pointer kept in
// from_sp, then the same popped off to_sp's, returning where that one
// switched away. A new fiber's stack is laid out as if it had switched away
// at runtime_fiber_start, which calls fiber_entry with the fiber's Impl.
extern "C" void runtime_fiber_switch(void **from, void *to);
extern "C" void runtime_fiber_start();

#if defined(__x86_64__)
// rbp, rbx, r12-r15, then MXCSR and the x87 control word, 8 bytes
__asm__(".text\n"
        ".globl runtime_fiber_switch\n"
        ".type runtime_fiber_switch, @function\n"
        "runtime_fiber_switch:\n"
        "    pushq %rbp\n"
        "    pushq %rbx\n"
        "    pushq %r12\n"
        "    pushq %r13\n"
        "    pushq %r14\n"
        "    pushq %r15\n"
        "    subq $8, %rsp\n"
        "    stmxcsr (%rsp)\n"
        "    fnstcw 4(%rsp)\n"
        "    movq %rsp, (%rdi)\n"
        "    movq %rsi, %rsp\n"
        "    ldmxcsr (%rsp)\n"
        "    fldcw 4(%rsp)\n"
        "    addq $8, %rsp\n"
        "    popq %r15\n"
        "    popq %r14\n"
        "    popq %r13\n"
        "    popq %r12\n"
        "    popq %rbx\n"
        "    popq %rbp\n"
        "    ret\n"
        ".size runtime_fiber_switch, .-runtime_fiber_switch\n"
        ".globl runtime_fiber_start\n"
        ".type runtime_fiber_start, @function\n"
        "runtime_fiber_start:\n"
        "    movq %rbx, %rdi\n"
        "    call runtime_fiber_entry\n"
        "    ud2\n"
        ".size runtime_fiber_start, .-runtime_fiber_start\n");
#else
// x19-x30 (x29 the frame, x30 the return), d8-d15, then FPCR: 176 bytes
__asm__(".text\n"
        ".globl runtime_fiber_switch\n"
        ".type runtime_fiber_switch, %function\n"
        "runtime_fiber_switch:\n"
        "    sub sp, sp, #176\n"
        "    stp x19, x20, [sp, #0]\n"
        "    stp x21, x22, [sp, #16]\n"
        "    stp x23, x24, [sp, #32]\n"
        "    stp x25, x26, [sp, #48]\n"
        "    stp x27, x28, [sp, #64]\n"
        "    stp x29, x30, [sp, #80]\n"
        "    stp d8, d9, [sp, #96]\n"
        "    stp d10, d11, [sp, #112]\n"
        "    stp d12, d13, [sp, #128]\n"
        "    stp d14, d15, [sp, #144]\n"
        "    mrs x9, fpcr\n"
        "    str x9, [sp, #160]\n"
        "    mov x9, sp\n"
        "    str x9, [x0]\n"
        "    mov sp, x1\n"
        "    ldp x19, x20, [sp, #0]\n"
        "    ldp x21, x22, [sp, #16]\n"
        "    ldp x23, x24, [sp, #32]\n"
        "    ldp x25, x26, [sp, #48]\n"
        "    ldp x27, x28, [sp, #64]\n"
        "    ldp x29, x30, [sp, #80]\n"
        "    ldp d8, d9, [sp, #96]\n"
        "    ldp d10, d11, [sp, #112]\n"
        "    ldp d12, d13, [sp, #128]\n"
        "    ldp d14, d15, [sp, #144]\n"
        "    ldr x9, [sp, #160]\n"
        "    msr fpcr, x9\n"
        "    add sp, sp, #176\n"
        "    ret\n"
        ".size runtime_fiber_switch, .-runtime_fiber_switch\n"
        ".globl runtime_fiber_start\n"
        ".type runtime_fiber_start, %function\n"
        "runtime_fiber_start:\n"
        "    mov x0, x19\n"
        "    bl runtime_fiber_entry\n"
        "    brk #0\n"
        ".size runtime_fiber_start, .-runtime_fiber_start\n");
#endif

struct Fiber::Impl {
    void *sp = nullptr; // where it switched away, its registers under it
    void *stack = nullptr;
    size_t size = 0;
    std::function<void()> body;
};

extern "C" [[noreturn]] void runtime_fiber_entry(Fiber::Impl *impl) {
    impl->body();
    std::fprintf(stderr, "fiber: a body returned\n");
    std::abort();
}

Fiber::Fiber() : impl_(std::make_unique<Impl>()) {}

// the host thread's own: its sp kept on its first switch away
std::unique_ptr<Fiber> Fiber::main() { return std::unique_ptr<Fiber>(new Fiber()); }

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
    // the stack as runtime_fiber_switch leaves it, returning to
    // runtime_fiber_start with the Impl in a callee-saved register
    uintptr_t top = (reinterpret_cast<uintptr_t>(impl.stack) + size) & ~uintptr_t(15);
#if defined(__x86_64__)
    // the return slot at top - 8: past it, the stack is 16-aligned for the
    // call runtime_fiber_start makes
    auto *frame = reinterpret_cast<uint64_t *>(top - 64);
    uint32_t mxcsr;
    uint16_t fpcw;
    __asm__ volatile("stmxcsr %0" : "=m"(mxcsr));
    __asm__ volatile("fnstcw %0" : "=m"(fpcw));
    frame[0] = uint64_t(mxcsr) | uint64_t(fpcw) << 32;
    frame[1] = frame[2] = frame[3] = frame[4] = 0;                 // r15-r12
    frame[5] = reinterpret_cast<uint64_t>(&impl);                  // rbx
    frame[6] = 0;                                                  // rbp
    frame[7] = reinterpret_cast<uint64_t>(&runtime_fiber_start);   // the return
#else
    auto *frame = reinterpret_cast<uint64_t *>(top - 176);
    for (int i = 0; i < 22; i++) frame[i] = 0;
    frame[0] = reinterpret_cast<uint64_t>(&impl);                  // x19
    frame[11] = reinterpret_cast<uint64_t>(&runtime_fiber_start);  // x30
    uint64_t fpcr;
    __asm__ volatile("mrs %0, fpcr" : "=r"(fpcr));
    frame[20] = fpcr;
#endif
    impl.sp = frame;
    return fiber;
}

Fiber::~Fiber() {
    if (impl_ && impl_->stack) munmap(impl_->stack, impl_->size);
}

void Fiber::switch_from(Fiber &from) { runtime_fiber_switch(&from.impl_->sp, impl_->sp); }

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
