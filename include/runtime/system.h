// The system: the kernel, the memory and the services together, and the
// loop that runs the games' code. The code is C (3dsrecomp's, with the
// decomp's in place of what it has): a function per guest address, found
// through a lookup the port gives. Interface 5's runs until a syscall, the
// end of its time slice or a jump it cannot follow, and comes back with the
// reason in Context::exit. Interface 6's runs on a fiber of its thread's
// own and calls the system as it goes: a syscall, the end of its slice and
// code it has no C for are calls that switch back here and come back where
// they were (Config::fibers).

#ifndef RUNTIME_SYSTEM_H
#define RUNTIME_SYSTEM_H

#include <cstdint>
#include <functional>
#include <map>
#include <string>

#include <memory>
#include <unordered_map>
#include <vector>

#include "runtime/fiber.h"
#include "runtime/kernel.h"
#include "runtime/memory.h"

namespace runtime {

// the code for a guest address (bit 0 set for Thumb), or null
using Lookup = Code (*)(uint32_t address);
// where the code for an address really is (bit 0 set for Thumb): itself, or
// the decomp's function a module's hooked one jumps to
using Redirect = uint32_t (*)(uint32_t address);

// what the services answer: called for each SendSyncRequest, with the
// command in the thread's TLS (Memory, at tls + 0x80)
using ServiceHandler = std::function<void(class System &, Thread &, const std::string &service, uint32_t subhandle)>;

struct Config {
    bool new3ds = false;
    uint32_t app_bytes = 64u << 20; // from the exheader's system mode
    Region region = Region::Application;
    uint64_t boot_clock_ms = 0;     // local time at boot, ms since 1900
    int32_t slice = 200000;         // a thread's time slice, in instructions
    // time as the code runs, a tick an instruction, as Zakuro counts it: a
    // run plays out the same each time, and never sleeps (for comparing)
    bool virtual_clock = false;
    // interface 6's code: each thread on a fiber of its own (the session
    // sets it from the library)
    bool fibers = false;
};

class System : private Hooks, private Clock {
public:
    System(const Config &config, Lookup lookup, ServiceHandler services);

    Memory memory;
    Kernel kernel;

    // the main thread, at the code's entry with its stack
    void start(uint32_t entry, uint32_t stack_top, uint32_t priority);

    // runs threads until stop() says to, every thread blocks, or the
    // process ends; false when it ended or could not go on
    bool run(const std::function<bool()> &stop);
    // with the virtual clock and every thread waiting: work beside the game
    // that will wake one (a GPU on its own thread) is let finish before time
    // moves on; true when there was some
    std::function<bool()> settle;

    // the time, in CPU ticks: the host's clock since boot, or the virtual one
    uint64_t ticks() override;

    const Config &config() const { return config_; }

    // why run() could not go on, when it could not
    const std::string &error() const { return error_; }

    // every jump through the lookup goes where redirect says first, r15 with it
    void set_redirect(Redirect redirect) { redirect_ = redirect; }
    // the code for the context's r15, redirected
    Code find(Context &c);

    // runs a context's code from r15 in the interpreter, up to an address
    // the C has, a syscall or a slice's end; false when it cannot go on
    bool interpret(Context &c);

    // interface 6, from the code on its thread's fiber: a syscall (the
    // thread waiting in it when it waits), the end of a slice (other
    // threads run), code without C run until it returns to stop (an
    // address, odd for Thumb) with the stack at sp, and one instruction
    void guest_svc(Context &c, uint32_t number);
    void guest_yield(Context &c);
    void guest_call(Context &c, uint32_t stop, uint32_t sp);
    void guest_interpret(Context &c, uint32_t address);
    // instructions the interpreter ran, where the C had nothing
    uint64_t interpreted = 0;
    // interface 6: the times the code unwound to its thread's loop (a call
    // that came back elsewhere, calls too deep): the rest of the callers
    // then run in the interpreter
    uint64_t unwinds = 0;
    // time the host slept, every thread waiting, in seconds
    double idle = 0;
    // where the host's time went, in seconds: the game's code, the
    // interpreter, syscalls (services among them), the scheduler, and the
    // run's caller (the hardware's events, drawing); and how many slices and
    // syscalls
    struct Profile {
        double code = 0, interpreter = 0, syscalls = 0, schedule = 0, outside = 0;
        uint64_t slices = 0, svcs = 0;
        double by_svc[256]{};
        std::map<uint32_t, double> by_thread; // the code's time, by thread
    } profile;
    // where it ran, by the address it entered at
    std::map<uint32_t, uint64_t> interpreted_at;

private:
    void request(Kernel &, Thread &, const std::string &service, uint32_t subhandle) override;
    void memory_syscall(Kernel &, Thread &, uint32_t number) override;
    uint32_t allocate_tls() override;
    void free_tls(uint32_t address) override { free_tls_.push_back(address); }
    void prepare(Context &context) override;

    // a thread's fiber: the code from r15, again and again, until it ends
    [[noreturn]] void thread_body(ThreadId id);
    // the interpreter on a thread's fiber, from r15 to an address the C
    // has, or to stop with the stack at sp (stop ~0: none): syscalls made
    // there and then, the slice's end a yield; false when it cannot go on
    bool interpret_here(Context &c, uint32_t stop, uint32_t sp);
    // back to the loop from the fiber running now, why given
    enum class Back { Yield, Svc, Ended, Failed };
    void back(Back why);
    bool run_fibers(const std::function<bool()> &stop);

    std::unique_ptr<Fiber> main_fiber_;
    std::unordered_map<ThreadId, std::unique_ptr<Fiber>> fibers_;
    Fiber *running_ = nullptr; // the fiber switched to last
    Back back_ = Back::Yield;
    double svc_time_ = 0; // of the fiber's turn, taken off the code's

    Config config_;
    Lookup lookup_;
    Redirect redirect_ = nullptr;
    ServiceHandler services_;
    Host host_{};
    uint32_t tls_used_ = 0;
    std::vector<uint32_t> free_tls_; // ended threads' TLS, made again first
    uint64_t boot_ = 0;
    uint64_t virtual_ = 0; // the virtual clock
    std::string error_;
};

} // namespace runtime

#endif
