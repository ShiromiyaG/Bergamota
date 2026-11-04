// The system and the loop that runs the games' code.

#include "runtime/system.h"

#include "runtime/interpreter.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
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
    if (s.config().fibers) {
        s.guest_interpret(*c, address);
        return;
    }
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
    // the code sets r15 to address before it asks; a redirect moves it on
    host_.lookup = [](Context *c, uint32_t address) {
        c->r[15] = address & ~1u;
        c->thumb = address & 1;
        return system_of(c).find(*c);
    };
    // C calling into the guest (recomp_call_guest, CALL_WHOLE): the code
    // runs on to the stop address on the same thread; a syscall that blocks
    // cannot be waited for there, and leaves it to the main loop
    // interface 6: on the thread's own fiber, calls that wait when they wait
    host_.svc = [](Context *c, uint32_t number) { system_of(c).guest_svc(*c, number); };
    host_.yield = [](Context *c) { system_of(c).guest_yield(*c); };
    host_.call = [](Context *c, uint32_t stop, uint32_t sp) { system_of(c).guest_call(*c, stop, sp); };
    host_.run = [](Context *c, uint32_t stop) {
        System &s = system_of(c);
        // interface 6: on the thread's fiber, where a syscall may wait
        if (s.config().fibers) {
            s.guest_call(*c, stop & ~1u, ~0u);
            if (!c->exit) c->exit = EXIT_NONE;
            return;
        }
        while (!(c->r[15] == stop && !c->thumb)) {
            Code code = s.find(*c);
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
    if (!free_tls_.empty()) {
        // an ended thread's, cleared as a new one's would be
        uint32_t address = free_tls_.back();
        free_tls_.pop_back();
        for (uint32_t offset = 0; offset < map::TLS_SIZE; offset += 4) memory.write32(address + offset, 0);
        return address;
    }
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

Code System::find(Context &c) {
    uint32_t address = c.r[15] | c.thumb;
    if (redirect_) {
        uint32_t to = redirect_(address);
        if (to != address) {
            c.r[15] = to & ~1u;
            c.thumb = to & 1;
            address = to;
        }
    }
    return lookup_(address);
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

void System::back(Back why) {
    back_ = why;
    main_fiber_->switch_from(*running_);
}

void System::guest_svc(Context &c, uint32_t number) {
    c.svc = number;
    auto before = std::chrono::steady_clock::now();
    kernel.syscall(number);
    double spent = std::chrono::duration<double>(std::chrono::steady_clock::now() - before).count();
    profile.syscalls += spent;
    profile.by_svc[number & 0xFF] += spent;
    profile.svcs++;
    svc_time_ += spent;
    // the scheduler looks again after every syscall, as with interface 5;
    // the thread comes back here when it runs again (an ended one never)
    Thread *self = kernel.current();
    back(!self || self->status == ThreadStatus::Dead ? Back::Ended : Back::Svc);
}

void System::guest_yield(Context &) { back(Back::Yield); }

void System::guest_interpret(Context &c, uint32_t address) {
    Cpu cpu;
    load(c, cpu);
    cpu.regs[15] = address;
    Exit exit = step(cpu, memory);
    store(cpu, c);
    interpreted++;
    uint32_t next = address + (c.thumb ? 2 : 4);
    if (exit.kind == Exit::Supervisor) {
        guest_svc(c, exit.value);
    } else if (exit.kind != Exit::None || c.r[15] != next) {
        // a branch: the code goes on from r15 (its dispatch, else the host)
        c.exit = EXIT_UNWIND;
    }
}

bool System::interpret_here(Context &c, uint32_t stop, uint32_t sp) {
    Cpu cpu;
    load(c, cpu);
    interpreted_at[c.r[15] | c.thumb]++;
    for (;;) {
        Exit exit = step(cpu, memory);
        interpreted++;
        c.budget--;
        if (exit.kind == Exit::Supervisor) {
            store(cpu, c);
            guest_svc(c, exit.value);
            load(c, cpu);
        } else if (exit.kind != Exit::None) {
            store(cpu, c);
            char text[96];
            std::snprintf(text, sizeof text, "the instruction 0x%08X at 0x%08X cannot run", exit.value,
                          cpu.regs[15] | cpu.cpsr.thumb);
            error_ = text;
            return false;
        }
        uint32_t at = cpu.regs[15] | cpu.cpsr.thumb;
        if ((at == stop && (sp == ~0u || cpu.regs[13] == sp)) || lookup_(at) || (cpu.regs[15] == THREAD_EXIT && !cpu.cpsr.thumb)) {
            store(cpu, c);
            return true;
        }
        if (c.budget <= 0) {
            store(cpu, c);
            guest_yield(c);
            load(c, cpu);
        }
    }
}

void System::guest_call(Context &c, uint32_t stop, uint32_t sp) {
    // as deep as the code's own calls go, then an unwind
    if (++c.depth > RECOMP_DEPTH_LIMIT) {
        c.depth--;
        c.exit = EXIT_UNWIND;
        unwinds++;
        return;
    }
    struct Leave {
        Context &c;
        ~Leave() { c.depth--; }
    } leave{c};
    for (;;) {
        if ((c.r[15] | c.thumb) == stop && (sp == ~0u || c.r[13] == sp)) return;
        static const bool trace = std::getenv("RUNTIME_TRACE_UNWIND") != nullptr;
        static int traced = 0;
        if (trace && (c.r[15] | c.thumb) == stop && traced++ < 20)
            std::fprintf(stderr, "call: back at 0x%08X with sp 0x%08X, not 0x%08X\n", stop, c.r[13], sp);
        Code code = find(c);
        c.exit = EXIT_NONE;
        if (code) {
            code(&c);
            // an unwind goes on up to the thread's own loop
            if (c.exit == EXIT_UNWIND) return;
        } else if (!interpret_here(c, stop, sp)) {
            back(Back::Failed);
        }
    }
}

void System::thread_body(ThreadId id) {
    Context &c = kernel.thread(id).context;
    for (;;) {
        if (c.r[15] == THREAD_EXIT && !c.thumb) guest_svc(c, 0x09); // the thread returned from its entry
        Code code = find(c);
        c.exit = EXIT_NONE;
        c.depth = 0;
        if (code) {
            code(&c);
            profile.slices++;
            if (c.exit == EXIT_UNWIND) unwinds++;
            static const bool trace = std::getenv("RUNTIME_TRACE_UNWIND") != nullptr;
            static int traced = 0;
            if (trace && c.exit == EXIT_UNWIND && traced++ < 40)
                std::fprintf(stderr, "unwind: thread %u at 0x%08X%s, lr 0x%08X, depth %u\n", id, c.r[15],
                             c.thumb ? " (thumb)" : "", c.r[14], c.depth);
        } else if (!interpret_here(c, ~0u, 0)) {
            back(Back::Failed);
        }
        // returned, or unwound: on from r15
    }
}

bool System::run_fibers(const std::function<bool()> &stop) {
    using clock = std::chrono::steady_clock;
    auto seconds = [](clock::time_point from) { return std::chrono::duration<double>(clock::now() - from).count(); };
    if (!main_fiber_) main_fiber_ = Fiber::main();
    // the fibers of threads that ended, let go here, off them
    std::vector<std::unique_ptr<Fiber>> ended;
    for (;;) {
        ended.clear();
        auto before_stop = clock::now();
        bool stopping = kernel.exited() || stop();
        profile.outside += seconds(before_stop);
        if (stopping) break;
        auto before_schedule = clock::now();
        bool scheduled = kernel.schedule();
        profile.schedule += seconds(before_schedule);
        if (!scheduled) {
            auto wake = kernel.next_wakeup();
            if (config_.virtual_clock) {
                if (settle && settle()) continue;
                constexpr uint64_t MS = CPU_CLOCK_HZ / 1000;
                virtual_ = wake && *wake > virtual_ ? std::min(*wake, virtual_ + MS) : virtual_ + MS;
                continue;
            }
            if (!wake) {
                auto before = clock::now();
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
                idle += seconds(before);
                continue;
            }
            uint64_t now = ticks();
            if (*wake > now) {
                uint64_t nanos = std::min<uint64_t>((*wake - now) * 1000000000 / CPU_CLOCK_HZ, 1000000);
                auto before = clock::now();
                std::this_thread::sleep_for(std::chrono::nanoseconds(nanos));
                idle += seconds(before);
            }
            continue;
        }
        Thread &t = *kernel.current();
        Context &c = t.context;
        auto &fiber = fibers_[t.id];
        // a deep stack, the pages taken as it grows: the code calls as deep
        // as the game does (RECOMP_DEPTH_LIMIT), each call a C frame
        if (!fiber) fiber = Fiber::make(16u << 20, [this, id = t.id] { thread_body(id); });
        c.budget = config_.slice;
        uint64_t interpreted_before = interpreted;
        svc_time_ = 0;
        running_ = fiber.get();
        auto before_code = clock::now();
        fiber->switch_from(*main_fiber_);
        double spent = seconds(before_code) - svc_time_;
        profile.code += spent;
        profile.by_thread[t.id] += spent;
        if (config_.virtual_clock) {
            int64_t used = int64_t(config_.slice) - c.budget;
            virtual_ += uint64_t(std::max({used, int64_t(interpreted - interpreted_before), int64_t(1)}));
        }
        if (back_ == Back::Failed) return false;
        if (back_ == Back::Ended) {
            ended.push_back(std::move(fiber));
            fibers_.erase(t.id);
        }
    }
    return !kernel.exited();
}

bool System::run(const std::function<bool()> &stop) {
    if (config_.fibers) return run_fibers(stop);
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
                if (settle && settle()) continue;
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
        Code code = find(c);
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
                code = find(c);
                if (!code) break;
                c.exit = EXIT_NONE;
                c.depth = 0;
            }
            double spent = seconds(before_code);
            profile.code += spent;
            profile.by_thread[t.id] += spent;
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
