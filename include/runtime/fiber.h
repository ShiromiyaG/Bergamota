// A stack of its own for each of the game's threads, switched to and from
// on the one host thread they share (interface 6): the game's code calls
// into the system as plain C calls, and a call that has the thread wait
// switches away inside it and comes back where it was. Windows' fibers; on
// x86-64 and AArch64 elsewhere (Linux, Android) a switch of our own, else
// ucontext.

#ifndef RUNTIME_FIBER_H
#define RUNTIME_FIBER_H

#include <cstddef>
#include <functional>
#include <memory>

namespace runtime {

class Fiber {
public:
    // the host thread's own stack as a fiber, to switch back to (one per
    // host thread, made before any other)
    static std::unique_ptr<Fiber> main();
    // a new stack of size bytes (reserved; the pages are taken as they are
    // used) that runs body once switched to. body must never return: it
    // switches away for good instead (a fiber that is done is destroyed
    // from another one)
    static std::unique_ptr<Fiber> make(size_t size, std::function<void()> body);
    ~Fiber();
    Fiber(const Fiber &) = delete;
    Fiber &operator=(const Fiber &) = delete;

    // from the fiber running now (from) to this one
    void switch_from(Fiber &from);

    struct Impl;

private:
    Fiber();
    std::unique_ptr<Impl> impl_;
};

} // namespace runtime

#endif
