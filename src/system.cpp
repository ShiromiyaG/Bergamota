// The system and the loop that runs the games' code.

#include "runtime/system.h"

#include "runtime/interpreter.h"

#include <chrono>
#include <cstdio>
#include <thread>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <timeapi.h>
#endif

namespace runtime {

namespace {

System &system_of(Context *ctx) { return *static_cast<System *>(ctx->user); }

uint8_t host_read8(Context *c, uint32_t a) { return system_of(c).memory.cpu_read8(a); }
uint16_t host_read16(Context *c, uint32_t a) { return system_of(c).memory.cpu_read16(a); }
uint32_t host_read32(Context *c, uint32_t a) { return system_of(c).memory.cpu_read32(a); }
void host_write8(Context *c, uint32_t a, uint8_t v) { system_of(c).memory.cpu_write8(a, v); }
void host_write16(Context *c, uint32_t a, uint16_t v) { system_of(c).memory.cpu_write16(a, v); }
void host_write32(Context *c, uint32_t a, uint32_t v) { system_of(c).memory.cpu_write32(a, v); }

// one instruction the C left to the interpreter; a branch, a syscall or
// one it cannot run leaves the C for the main loop
void host_interpret(Context *c, uint32_t address, uint32_t) {
    System &s = system_of(c);
    Cpu cpu;
    load(*c, cpu);
    cpu.regs[15] = address;
    Exit exit = step(cpu, s.memory);
    store(cpu, *c);
    s.interpreted++;
    uint32_t next = address + (c->thumb ? 2 : 4);
    if (exit.kind == Exit::Supervisor) {
        c->svc = exit.value;
        c->exit = EXIT_SVC;
    } else if (exit.kind != Exit::None || c->r[15] != next) {
        c->exit = EXIT_UNWIND;
    }
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
            c->exit = EXIT_NONE;
            if (code) {
                code(c);
            } else if (!s.interpret(*c)) {
                c->exit = EXIT_UNWIND;
                return;
            }
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
#ifdef _WIN32
    // a millisecond's sleep is a millisecond, not the 15.6 ms the system
    // timer ticks at by default
    timeBeginPeriod(1);
#endif
    boot_ = static_cast<uint64_t>(std::chrono::steady_clock::now().time_since_epoch().count());
    memory.init_system_pages(config.boot_clock_ms);
}

uint64_t System::ticks() {
    if (config_.virtual_clock) return virtual_;
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
    c.read_pages = memory.cpu_read_pages();
    c.write_pages = memory.cpu_write_pages();
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

bool System::interpret(Context &c) {
    // the interpreter, from r15 to the first address the C has, a syscall,
    // or the end of a slice
    Cpu cpu;
    load(c, cpu);
    interpreted_at[c.r[15] | c.thumb]++;
    for (int32_t n = 0; n < config_.slice; n++) {
        Exit exit = step(cpu, memory);
        interpreted++;
        if (exit.kind == Exit::Supervisor) {
            store(cpu, c);
            c.svc = exit.value;
            c.exit = EXIT_SVC;
            return true;
        }
        if (exit.kind != Exit::None) {
            store(cpu, c);
            char text[96];
            std::snprintf(text, sizeof text, "the instruction 0x%08X at 0x%08X cannot run", exit.value,
                          cpu.regs[15] | cpu.cpsr.thumb);
            error_ = text;
            return false;
        }
        if (lookup_(cpu.regs[15] | cpu.cpsr.thumb)) {
            break;
        }
    }
    store(cpu, c);
    return true;
}

bool System::run(const std::function<bool()> &stop) {
    using clock = std::chrono::steady_clock;
    auto seconds = [](clock::time_point from) { return std::chrono::duration<double>(clock::now() - from).count(); };
    for (;;) {
        auto before_stop = clock::now();
        bool stopping = kernel.exited() || stop();
        profile.outside += seconds(before_stop);
        if (stopping) break;
        auto before_schedule = clock::now();
        bool scheduled = kernel.schedule();
        profile.schedule += seconds(before_schedule);
        if (!scheduled) {
            // every thread waits: on a time, or on an outside event the
            // services signal (the caller's stop() lets it raise them)
            auto wake = kernel.next_wakeup();
            if (config_.virtual_clock) {
                // a millisecond on at most, so the hardware's events (stop()'s
                // caller) come in order, or straight to the wakeup
                constexpr uint64_t MS = CPU_CLOCK_HZ / 1000;
                virtual_ = wake && *wake > virtual_ ? std::min(*wake, virtual_ + MS) : virtual_ + MS;
                continue;
            }
            if (!wake) {
                // nothing has a time to wake at: only an outside event can
                auto before = std::chrono::steady_clock::now();
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
                idle += std::chrono::duration<double>(std::chrono::steady_clock::now() - before).count();
                continue;
            }
            uint64_t now = ticks();
            if (*wake > now) {
                // a millisecond at most: the hardware's own events (audio
                // frames, vertical blank) come from stop()'s caller
                uint64_t nanos = std::min<uint64_t>((*wake - now) * 1000000000 / CPU_CLOCK_HZ, 1000000);
                auto before = std::chrono::steady_clock::now();
                std::this_thread::sleep_for(std::chrono::nanoseconds(nanos));
                idle += std::chrono::duration<double>(std::chrono::steady_clock::now() - before).count();
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
        c.exit = EXIT_NONE;
        c.budget = config_.slice;
        c.depth = 0;
        uint64_t interpreted_before = interpreted;
        auto before_code = clock::now();
        if (code) {
            // the code leaves at a jump it cannot follow: the same thread goes
            // on from there while its slice lasts, without the scheduler
            for (;;) {
                code(&c);
                profile.slices++;
                bool left = c.exit == EXIT_NONE || c.exit == EXIT_UNWIND;
                if (!left || c.budget <= 0 || (c.r[15] == THREAD_EXIT && !c.thumb)) break;
                code = lookup_(c.r[15] | c.thumb);
                if (!code) break;
                c.exit = EXIT_NONE;
                c.depth = 0;
            }
            profile.code += seconds(before_code);
        } else {
            bool went_on = interpret(c);
            profile.interpreter += seconds(before_code);
            if (!went_on) return false;
            profile.slices++;
        }
        if (config_.virtual_clock) {
            // what the slice used, a tick an instruction (one at least)
            int64_t used = code ? int64_t(config_.slice) - c.budget : int64_t(interpreted - interpreted_before);
            virtual_ += uint64_t(std::max<int64_t>(used, 1));
        }
        if (c.exit == EXIT_SVC) {
            auto before_svc = clock::now();
            kernel.syscall(c.svc);
            double spent = seconds(before_svc);
            profile.syscalls += spent;
            profile.by_svc[c.svc & 0xFF] += spent;
            profile.svcs++;
        }
        // a slice ran out or the code left at a jump: go on from r15, maybe
        // with another thread
    }
    return !kernel.exited();
}

} // namespace runtime
