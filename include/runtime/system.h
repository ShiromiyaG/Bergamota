// The system: the kernel, the memory and the services together, and the
// loop that runs the games' code. The code is C (3dsrecomp's, with the
// decomp's in place of what it has): a function per guest address, found
// through a lookup the port gives, that runs until a syscall, the end of
// its time slice or a jump it cannot follow, and comes back with the
// reason in Context::exit.

#ifndef RUNTIME_SYSTEM_H
#define RUNTIME_SYSTEM_H

#include <cstdint>
#include <functional>
#include <map>
#include <string>

#include "runtime/kernel.h"
#include "runtime/memory.h"

namespace runtime {

// the code for a guest address (bit 0 set for Thumb), or null
using Lookup = Code (*)(uint32_t address);

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

    // the time, in CPU ticks: the host's clock since boot, or the virtual one
    uint64_t ticks() override;

    // why run() could not go on, when it could not
    const std::string &error() const { return error_; }

    // runs a context's code from r15 in the interpreter, up to an address
    // the C has, a syscall or a slice's end; false when it cannot go on
    bool interpret(Context &c);
    // instructions the interpreter ran, where the C had nothing
    uint64_t interpreted = 0;
    // time the host slept, every thread waiting, in seconds
    double idle = 0;
    // where it ran, by the address it entered at
    std::map<uint32_t, uint64_t> interpreted_at;

private:
    void request(Kernel &, Thread &, const std::string &service, uint32_t subhandle) override;
    void memory_syscall(Kernel &, Thread &, uint32_t number) override;
    uint32_t allocate_tls() override;
    void prepare(Context &context) override;

    Config config_;
    Lookup lookup_;
    ServiceHandler services_;
    Host host_{};
    uint32_t tls_used_ = 0;
    uint64_t boot_ = 0;
    uint64_t virtual_ = 0; // the virtual clock
    std::string error_;
};

} // namespace runtime

#endif
