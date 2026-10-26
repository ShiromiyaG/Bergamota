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

    // the time, in CPU ticks: the host's clock since boot
    uint64_t ticks() override;

    // why run() could not go on, when it could not
    const std::string &error() const { return error_; }

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
    std::string error_;
};

} // namespace runtime

#endif
