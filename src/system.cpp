// The system and the loop that runs the games' code.

#include "runtime/system.h"

#include <chrono>
#include <cstdio>
#include <thread>

namespace runtime {

namespace {

System &system_of(Context *ctx) { return *static_cast<System *>(ctx->user); }

uint8_t host_read8(Context *c, uint32_t a) { return system_of(c).memory.read8(a); }
uint16_t host_read16(Context *c, uint32_t a) { return system_of(c).memory.read16(a); }
uint32_t host_read32(Context *c, uint32_t a) { return system_of(c).memory.read32(a); }
void host_write8(Context *c, uint32_t a, uint8_t v) { system_of(c).memory.write8(a, v); }
void host_write16(Context *c, uint32_t a, uint16_t v) { system_of(c).memory.write16(a, v); }
void host_write32(Context *c, uint32_t a, uint32_t v) { system_of(c).memory.write32(a, v); }

// the port has no interpreter: everything has to be in C, recompiled or
// decompiled
void host_interpret(Context *c, uint32_t address, uint32_t opcode) {
    std::fprintf(stderr, "runtime: no C for the instruction 0x%08X at 0x%08X\n", opcode, address);
    c->r[15] = address;
    c->exit = EXIT_UNWIND;
}

constexpr uint32_t STACK_SIZE = 0x4000;

} // namespace

System::System(const Config &config, Lookup lookup, ServiceHandler services)
    : memory(config.new3ds, config.app_bytes), kernel(memory, *this, *this), config_(config), lookup_(lookup),
      services_(std::move(services)) {
    host_.read8 = host_read8;
    host_.read16 = host_read16;
    host_.read32 = host_read32;
    host_.write8 = host_write8;
    host_.write16 = host_write16;
    host_.write32 = host_write32;
    host_.interpret = host_interpret;
    host_.lookup = [](Context *c, uint32_t address) { return system_of(c).lookup_(address); };
    // C calling into the guest (recomp_call_guest, CALL_WHOLE): the code
    // runs on to the stop address on the same thread; a syscall that blocks
    // cannot be waited for there, and leaves it to the main loop
    host_.run = [](Context *c, uint32_t stop) {
        System &s = system_of(c);
        while (!(c->r[15] == stop && !c->thumb)) {
            Code code = s.lookup_(c->r[15] | c->thumb);
            if (!code) {
                c->exit = EXIT_UNWIND;
                return;
            }
            c->exit = EXIT_NONE;
            code(c);
            if (c->exit == EXIT_SVC) {
                s.kernel.syscall(c->svc);
                if (s.kernel.current() && s.kernel.current()->status != ThreadStatus::Running) {
                    c->exit = EXIT_SVC;
                    return;
                }
            }
        }
        c->exit = EXIT_NONE;
    };
    boot_ = static_cast<uint64_t>(std::chrono::steady_clock::now().time_since_epoch().count());
    memory.init_system_pages(config.boot_clock_ms);
}

uint64_t System::ticks() {
    auto now = std::chrono::steady_clock::now().time_since_epoch();
    auto since = std::chrono::nanoseconds(now).count() - static_cast<int64_t>(boot_);
    return nanos_to_ticks(static_cast<uint64_t>(since));
}

void System::start(uint32_t entry, uint32_t stack_top, uint32_t priority) {
    // the main thread's stack, mapped below its top
    auto block = memory.allocate(config_.region, STACK_SIZE, true);
    if (block) {
        memory.map(stack_top - STACK_SIZE, block->paddr, STACK_SIZE, RW, MemoryState::Private);
    }
    kernel.start(entry, stack_top, priority);
}

uint32_t System::allocate_tls() {
    // eight threads' TLS to a page, each page mapped when first needed
    uint32_t address = map::TLS_VADDR + tls_used_ * map::TLS_SIZE;
    if (tls_used_ % (map::PAGE_SIZE / map::TLS_SIZE) == 0) {
        if (auto block = memory.allocate(Region::Base, map::PAGE_SIZE)) {
            memory.map(address, block->paddr, map::PAGE_SIZE, RW, MemoryState::Locked);
        }
    }
    tls_used_++;
    return address;
}

void System::prepare(Context &c) {
    c.read_pages = memory.read_pages();
    c.write_pages = memory.write_pages();
    c.host = &host_;
    c.user = this;
}

void System::request(Kernel &, Thread &thread, const std::string &service, uint32_t subhandle) {
    if (services_) {
        services_(*this, thread, service, subhandle);
    } else {
        thread.context.r[0] = result::NOT_IMPLEMENTED;
    }
}

void System::memory_syscall(Kernel &k, Thread &thread, uint32_t number) {
    runtime::memory_syscall(memory, k, thread, number, config_.region);
}

bool System::run(const std::function<bool()> &stop) {
    while (!kernel.exited() && !stop()) {
        if (!kernel.schedule()) {
            // every thread waits: on a time, or on an outside event the
            // services signal (the caller's stop() lets it raise them)
            auto wake = kernel.next_wakeup();
            if (!wake) {
                return true;
            }
            uint64_t now = ticks();
            if (*wake > now) {
                std::this_thread::sleep_for(std::chrono::nanoseconds((*wake - now) * 1000000000 / CPU_CLOCK_HZ));
            }
            continue;
        }
        Thread &t = *kernel.current();
        Context &c = t.context;
        if (c.r[15] == THREAD_EXIT && !c.thumb) { // the thread returned from its entry
            kernel.syscall(0x09);
            continue;
        }
        Code code = lookup_(c.r[15] | c.thumb);
        if (!code) {
            char text[64];
            std::snprintf(text, sizeof text, "no C for 0x%08X (thread %u)", c.r[15] | c.thumb, t.id);
            error_ = text;
            return false;
        }
        c.exit = EXIT_NONE;
        c.budget = config_.slice;
        c.depth = 0;
        code(&c);
        if (c.exit == EXIT_SVC) {
            kernel.syscall(c.svc);
        }
        // a slice ran out or the code left at a jump: go on from r15, maybe
        // with another thread
    }
    return !kernel.exited();
}

} // namespace runtime
