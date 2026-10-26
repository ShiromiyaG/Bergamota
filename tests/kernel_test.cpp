// The kernel's threads and waits, driven by hand: each test plays the
// guest, setting a thread's registers and making its syscalls the way the
// games' code does, and checks what the scheduler does with it.

#include "runtime/kernel.h"

#include <cstdio>
#include <cstdlib>
#include <map>

using namespace runtime;

namespace {

int failures = 0;

#define CHECK(cond)                                                                                                    \
    do {                                                                                                               \
        if (!(cond)) {                                                                                                 \
            std::printf("%s:%d: %s\n", __FILE__, __LINE__, #cond);                                                     \
            failures++;                                                                                                \
        }                                                                                                              \
    } while (0)

struct Memory : GuestMemory {
    std::map<uint32_t, uint32_t> words;
    uint32_t read32(uint32_t a) override { return words[a]; }
    void write32(uint32_t a, uint32_t v) override { words[a] = v; }
};

struct ManualClock : Clock {
    uint64_t now = 0;
    uint64_t ticks() override { return now; }
};

struct NoHooks : Hooks {
    uint32_t next_tls = 0x1FF82000;
    void request(Kernel &, Thread &, const std::string &, uint32_t) override {}
    void memory_syscall(Kernel &, Thread &, uint32_t) override {}
    uint32_t allocate_tls() override { return next_tls += 0x200; }
};

struct World {
    Memory memory;
    ManualClock clock;
    NoHooks hooks;
    Kernel kernel{memory, clock, hooks};

    // the current thread makes a syscall with these registers
    Context &call(uint32_t number, std::initializer_list<uint32_t> regs) {
        Context &c = kernel.current()->context;
        uint32_t i = 0;
        for (uint32_t r : regs) {
            c.r[i++] = r;
        }
        kernel.syscall(number);
        return c;
    }
};

void arbiter_wait_and_signal() {
    World w;
    ThreadId a = w.kernel.start(0x100000, 0x10000000, 0x30);
    CHECK(w.kernel.schedule());
    CHECK(w.kernel.current()->id == a);
    // a makes thread b, lower priority
    Context &made = w.call(0x08, {0x31, 0x100100, 7, 0x0FF00000, 0});
    CHECK(made.r[0] == 0);
    ThreadId b = 1;
    CHECK(w.kernel.thread(b).context.r[0] == 7);
    uint32_t arbiter = w.call(0x21, {}).r[1];
    // a parks on a word below the value
    w.memory.words[0x2000] = 0;
    w.call(0x22, {arbiter, 0x2000, 1, 1, 0, 0});
    CHECK(w.kernel.thread(a).status == ThreadStatus::WaitArbiter);
    CHECK(w.kernel.schedule());
    CHECK(w.kernel.current()->id == b);
    // b signals one waiter
    w.call(0x22, {arbiter, 0x2000, 0, 1, 0, 0});
    CHECK(w.kernel.schedule());
    // a is more urgent and runs again, with success
    CHECK(w.kernel.current()->id == a);
    CHECK(w.kernel.thread(a).context.r[0] == 0);
}

void event_wait_timeout() {
    World w;
    w.kernel.start(0x100000, 0x10000000, 0x30);
    w.kernel.schedule();
    uint32_t event = w.call(0x17, {0, 0}).r[1];
    // wait 1 ms on an event nobody signals
    w.call(0x24, {event, 0, 1000000, 0});
    CHECK(!w.kernel.schedule());
    auto wake = w.kernel.next_wakeup();
    CHECK(wake && *wake == nanos_to_ticks(1000000));
    w.clock.now = *wake;
    CHECK(w.kernel.schedule());
    CHECK(w.kernel.current()->context.r[0] == result::TIMEOUT);
}

void event_signaled_one_shot() {
    World w;
    ThreadId a = w.kernel.start(0x100000, 0x10000000, 0x30);
    w.kernel.schedule();
    uint32_t event = w.call(0x17, {0, 0}).r[1];
    w.call(0x08, {0x31, 0x100100, 0, 0x0FF00000, 0});
    // a waits forever, b signals, a takes it and the event clears
    w.call(0x24, {event, 0, 0xFFFFFFFF, 0xFFFFFFFF});
    w.kernel.schedule();
    CHECK(w.kernel.current()->id == 1);
    w.call(0x18, {event});
    w.kernel.schedule();
    CHECK(w.kernel.current()->id == a);
    CHECK(w.kernel.current()->context.r[0] == 0);
    // one-shot: a second wait with no timeout fails at once
    CHECK(w.call(0x24, {event, 0, 0, 0}).r[0] == result::TIMEOUT);
}

void mutex_recursion_and_exit() {
    World w;
    ThreadId a = w.kernel.start(0x100000, 0x10000000, 0x30);
    w.kernel.schedule();
    uint32_t mutex = w.call(0x13, {0, 1}).r[1];
    // recursive: the owner takes it again at once
    CHECK(w.call(0x24, {mutex, 0, 0, 0}).r[0] == 0);
    w.call(0x08, {0x2F, 0x100100, 0, 0x0FF00000, 0}); // b, more urgent
    w.kernel.schedule();
    CHECK(w.kernel.current()->id == 1);
    // b blocks on the mutex a holds
    w.call(0x24, {mutex, 0, 0xFFFFFFFF, 0xFFFFFFFF});
    w.kernel.schedule();
    CHECK(w.kernel.current()->id == a);
    // a ends holding it: b gets it
    w.call(0x09, {});
    w.kernel.schedule();
    CHECK(w.kernel.current()->id == 1);
    CHECK(w.kernel.current()->context.r[0] == 0);
}

void sleep_and_yield() {
    World w;
    w.kernel.start(0x100000, 0x10000000, 0x30);
    w.kernel.schedule();
    // a yield with no one else ready keeps running
    w.call(0x0A, {0, 0});
    CHECK(w.kernel.current()->status == ThreadStatus::Running);
    // a 2 ms sleep
    w.call(0x0A, {2000000, 0});
    CHECK(!w.kernel.schedule());
    w.clock.now = nanos_to_ticks(2000000);
    CHECK(w.kernel.schedule());
    CHECK(w.kernel.current()->context.r[0] == 0);
}

} // namespace

int main() {
    arbiter_wait_and_signal();
    event_wait_timeout();
    event_signaled_one_shot();
    mutex_recursion_and_exit();
    sleep_and_yield();
    std::printf(failures ? "%d failure(s)\n" : "kernel: all passed\n", failures);
    return failures ? EXIT_FAILURE : EXIT_SUCCESS;
}
