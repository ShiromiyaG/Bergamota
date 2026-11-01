// The syscalls the games make (32 of the 34 in USUM's code.bin; the two
// debug ones are left out, and the CROs make none). Translated from
// Zakuro's kernel/svc.rs (Rust, MIT, Copyright (c) 2026 fearkov; see
// LICENSE-zakuro). Arguments and results are in the thread's registers, as
// on the 3DS.

#include "runtime/kernel.h"
#include "runtime/runtime.h"

#include <algorithm>
#include <array>

namespace runtime {

namespace {

// a zero sleep steps aside this long when other threads are ready, about a
// time slice
constexpr uint64_t YIELD_TICKS = 20000;

uint64_t pair(uint32_t low, uint32_t high) { return static_cast<uint64_t>(low) | static_cast<uint64_t>(high) << 32; }

} // namespace

void Kernel::syscall(uint32_t number) {
    Thread &self = *current();
    Context &c = self.context;
    switch (number) {
    case 0x01: // ControlMemory
    case 0x02: // QueryMemory
    case 0x1E: // CreateMemoryBlock
    case 0x1F: // MapMemoryBlock
    case 0x20: // UnmapMemoryBlock
    case 0x2B: // GetProcessInfo
        hooks_.memory_syscall(*this, self, number);
        break;
    case 0x03: // ExitProcess
        for (auto &t : threads_) {
            t->status = ThreadStatus::Dead;
        }
        live_.clear();
        exited_ = true;
        break;
    case 0x08:
        create_thread(self);
        break;
    case 0x09: // ExitThread
        end_thread(self);
        break;
    case 0x0A:
        sleep_thread(self);
        break;
    case 0x0B: { // GetThreadPriority
        auto id = resolve_thread(c.r[1]);
        c.r[0] = id ? result::SUCCESS : result::INVALID_HANDLE;
        if (id) {
            c.r[1] = threads_[*id]->priority;
        }
        break;
    }
    case 0x0C: { // SetThreadPriority
        auto id = resolve_thread(c.r[0]);
        if (id) {
            threads_[*id]->priority = std::min<uint32_t>(c.r[1], 0x3F);
        }
        c.r[0] = id ? result::SUCCESS : result::INVALID_HANDLE;
        break;
    }
    case 0x13:
        create_mutex(self);
        break;
    case 0x14:
        release_mutex(self);
        break;
    case 0x17:
        create_event(self);
        break;
    case 0x18:
        signal_or_clear(self, true);
        break;
    case 0x19:
        signal_or_clear(self, false);
        break;
    case 0x21:
        create_address_arbiter(self);
        break;
    case 0x22:
        arbitrate_address(self);
        break;
    case 0x23: { // CloseHandle
        auto it = handles_.find(c.r[0]);
        if (it != handles_.end()) {
            ObjectId id = it->second;
            handles_.erase(it);
            release(id);
        }
        c.r[0] = result::SUCCESS;
        break;
    }
    case 0x24:
        wait_synchronization1(self);
        break;
    case 0x25:
        wait_synchronization_n(self);
        break;
    case 0x27:
        duplicate_handle(self);
        break;
    case 0x28: { // GetSystemTick
        uint64_t now = clock_.ticks();
        c.r[0] = static_cast<uint32_t>(now);
        c.r[1] = static_cast<uint32_t>(now >> 32);
        break;
    }
    case 0x29: // GetHandleInfo
        c.r[0] = result::SUCCESS;
        c.r[1] = 0;
        c.r[2] = 0;
        break;
    case 0x2D:
        connect_to_port(self);
        break;
    case 0x32:
        send_sync_request(self);
        break;
    case 0x35: // GetProcessId
        c.r[0] = result::SUCCESS;
        c.r[1] = 0x28; // the first application's id
        break;
    case 0x37: { // GetThreadId
        auto id = resolve_thread(c.r[1]);
        c.r[0] = id ? result::SUCCESS : result::INVALID_HANDLE;
        if (id) {
            c.r[1] = *id;
        }
        break;
    }
    case 0x38: // GetResourceLimit
        c.r[0] = result::SUCCESS;
        c.r[1] = create_handle(insert(ResourceLimit{}));
        break;
    case 0x3A: { // GetResourceLimitCurrentValues: r0 values out, r2 names, r3 count
        for (uint32_t i = 0; i < c.r[3]; i++) {
            memory_.write32(c.r[0] + i * 8, 0);
            memory_.write32(c.r[0] + i * 8 + 4, 0);
        }
        c.r[0] = result::SUCCESS;
        break;
    }
    case 0x3C: // Break
        exited_ = true;
        break;
    default:
        c.r[0] = result::NOT_IMPLEMENTED;
        break;
    }
}

// CreateThread: r0 priority, r1 entry, r2 argument, r3 stack top, r4 core
void Kernel::create_thread(Thread &self) {
    Context &c = self.context;
    uint32_t priority = c.r[0], entry = c.r[1], argument = c.r[2], stack = c.r[3];
    ThreadId id = start(entry, stack, priority);
    Thread &t = *threads_[id];
    t.context.r[0] = argument;
    t.core = int32_t(c.r[4]);
    c.r[0] = result::SUCCESS;
    c.r[1] = create_handle(insert(ThreadObject{id}));
}

// SleepThread: r0:r1 nanoseconds, signed
void Kernel::sleep_thread(Thread &self) {
    Context &c = self.context;
    uint64_t nanos = pair(c.r[0], c.r[1]);
    c.r[0] = result::SUCCESS;
    uint64_t ticks;
    if (nanos != 0) {
        ticks = nanos_to_ticks(nanos);
    } else {
        // a zero sleep is a yield: it lets the others ready run a while, even
        // lower ones, or titles polling a worker this way would spin forever
        bool others = std::any_of(live_.begin(), live_.end(), [&](ThreadId id) {
            const Thread &t = *threads_[id];
            return id != self.id && (t.status == ThreadStatus::Ready || t.status == ThreadStatus::Running);
        });
        if (!others) {
            return;
        }
        ticks = YIELD_TICKS;
    }
    end_wait(self);
    self.wakeup_at = clock_.ticks() + ticks;
    self.status = ThreadStatus::Sleeping;
    self.wait_syscall = WaitSyscall::SleepThread;
}

// CreateMutex: r1 initially locked
void Kernel::create_mutex(Thread &self) {
    Context &c = self.context;
    Mutex m;
    if (c.r[1]) {
        m.owner = self.id;
        m.lock_count = 1;
    }
    c.r[0] = result::SUCCESS;
    c.r[1] = create_handle(insert(m));
}

void Kernel::release_mutex(Thread &self) {
    Context &c = self.context;
    auto id = resolve(c.r[0]);
    auto *m = id ? std::get_if<Mutex>(object(*id)) : nullptr;
    if (!m) {
        c.r[0] = result::INVALID_HANDLE;
        return;
    }
    if (m->lock_count > 0) {
        m->lock_count--;
    }
    if (m->lock_count == 0) {
        m->owner.reset();
    }
    c.r[0] = result::SUCCESS;
}

// CreateEvent: r1 reset type
void Kernel::create_event(Thread &self) {
    Context &c = self.context;
    ResetType reset = c.r[1] == 1 ? ResetType::Sticky : c.r[1] == 2 ? ResetType::Pulse : ResetType::OneShot;
    c.r[0] = result::SUCCESS;
    c.r[1] = create_event(reset).second;
}

void Kernel::signal_or_clear(Thread &self, bool signal) {
    Context &c = self.context;
    auto id = resolve(c.r[0]);
    if (!id) {
        c.r[0] = result::INVALID_HANDLE;
        return;
    }
    if (signal) {
        signal_event(*id);
    } else if (auto *e = std::get_if<Event>(object(*id))) {
        e->signaled = false;
    }
    c.r[0] = result::SUCCESS;
}

void Kernel::create_address_arbiter(Thread &self) {
    Context &c = self.context;
    c.r[0] = result::SUCCESS;
    c.r[1] = create_handle(insert(AddressArbiter{}));
}

// ArbitrateAddress: r0 arbiter, r1 address, r2 type, r3 value, r4:r5
// timeout in nanoseconds (negative: forever)
void Kernel::arbitrate_address(Thread &self) {
    Context &c = self.context;
    uint32_t address = c.r[1], type = c.r[2];
    int32_t value = static_cast<int32_t>(c.r[3]);
    int64_t timeout = static_cast<int64_t>(pair(c.r[4], c.r[5]));
    if (type > 4) {
        c.r[0] = result::INVALID_ENUM_VALUE;
        return;
    }
    auto id = resolve(c.r[0]);
    auto *arbiter = id ? std::get_if<AddressArbiter>(object(*id)) : nullptr;
    if (!arbiter) {
        c.r[0] = result::INVALID_HANDLE;
        return;
    }
    c.r[0] = result::SUCCESS;

    if (type == 0) { // signal: wake up to value threads parked there, all when negative
        int32_t woken = 0;
        std::vector<ThreadId> wake;
        auto &w = arbiter->waiters;
        for (auto it = w.begin(); it != w.end();) {
            if (it->second != address) {
                ++it;
                continue;
            }
            Thread &parked = *threads_[it->first];
            // one whose wait timed out or that ended left its entry: dropped
            // without counting
            if (parked.status != ThreadStatus::WaitArbiter || parked.wait_address != address ||
                std::find(wake.begin(), wake.end(), it->first) != wake.end()) {
                it = w.erase(it);
                continue;
            }
            if (value < 0 || woken < value) {
                woken++;
                wake.push_back(it->first);
                it = w.erase(it);
            } else {
                ++it;
            }
        }
        for (ThreadId t : wake) {
            Thread &parked = *threads_[t];
            end_wait(parked);
            parked.wait_result = std::optional<uint32_t>{0};
            parked.status = ThreadStatus::Ready;
        }
        return;
    }

    // the waits sleep only while the word there is below value
    int32_t now = static_cast<int32_t>(memory_.read32(address));
    if (now >= value) {
        return;
    }
    if (type == 2 || type == 4) { // decrement first
        memory_.write32(address, static_cast<uint32_t>(now - 1));
    }
    // a thread waits on one thing at a time: an entry a timed-out wait left
    // goes, or waits on addresses nobody signals pile up
    std::erase_if(arbiter->waiters, [&](const auto &w) { return w.first == self.id; });
    arbiter->waiters.emplace_back(self.id, address);
    end_wait(self);
    self.wait_address = address;
    bool timed = type == 3 || type == 4;
    if (timed && timeout >= 0) {
        self.wakeup_at = clock_.ticks() + nanos_to_ticks(static_cast<uint64_t>(timeout));
    }
    self.status = ThreadStatus::WaitArbiter;
    self.wait_syscall = WaitSyscall::ArbitrateAddress;
}

// WaitSynchronization1: r0 handle, r2:r3 timeout in nanoseconds
void Kernel::wait_synchronization1(Thread &self) {
    Context &c = self.context;
    int64_t timeout = static_cast<int64_t>(pair(c.r[2], c.r[3]));
    auto id = resolve(c.r[0]);
    if (!id) {
        c.r[0] = result::INVALID_HANDLE;
        return;
    }
    if (is_signaled(*id, self.id)) {
        acquire(*id, self.id);
        c.r[0] = result::SUCCESS;
        return;
    }
    if (timeout == 0) {
        c.r[0] = result::TIMEOUT;
        return;
    }
    begin_wait({*id}, false,
               timeout > 0 ? std::optional<uint64_t>(nanos_to_ticks(static_cast<uint64_t>(timeout))) : std::nullopt);
    self.wait_syscall = WaitSyscall::WaitSynchronization1;
}

// WaitSynchronizationN: r0 timeout low, r1 handles, r2 count, r3 wait all,
// r4 timeout high
void Kernel::wait_synchronization_n(Thread &self) {
    Context &c = self.context;
    int64_t timeout = static_cast<int64_t>(pair(c.r[0], c.r[4]));
    bool wait_all = c.r[3] != 0;
    std::vector<ObjectId> objects;
    for (uint32_t i = 0; i < c.r[2]; i++) {
        auto id = resolve(memory_.read32(c.r[1] + i * 4));
        if (!id) {
            c.r[0] = result::INVALID_HANDLE;
            return;
        }
        objects.push_back(*id);
    }
    std::optional<uint32_t> index;
    if (wait_all) {
        if (std::all_of(objects.begin(), objects.end(), [&](ObjectId o) { return is_signaled(o, self.id); })) {
            index = 0;
            for (ObjectId o : objects) {
                acquire(o, self.id);
            }
        }
    } else {
        for (uint32_t k = 0; k < objects.size(); k++) {
            if (is_signaled(objects[k], self.id)) {
                index = k;
                acquire(objects[k], self.id);
                break;
            }
        }
    }
    if (index) {
        c.r[0] = result::SUCCESS;
        c.r[1] = *index;
        return;
    }
    if (timeout == 0) {
        c.r[0] = result::TIMEOUT;
        return;
    }
    begin_wait(std::move(objects), wait_all,
               timeout > 0 ? std::optional<uint64_t>(nanos_to_ticks(static_cast<uint64_t>(timeout))) : std::nullopt);
    self.wait_syscall = WaitSyscall::WaitSynchronizationN;
}

// DuplicateHandle: r1 handle
void Kernel::duplicate_handle(Thread &self) {
    Context &c = self.context;
    std::optional<ObjectId> id;
    if (c.r[1] == CURRENT_THREAD) {
        id = insert(ThreadObject{self.id});
    } else if (c.r[1] == CURRENT_PROCESS) {
        id = insert(ProcessObject{});
    } else {
        id = resolve(c.r[1]);
    }
    if (!id) {
        c.r[0] = result::INVALID_HANDLE;
        return;
    }
    c.r[0] = result::SUCCESS;
    c.r[1] = create_handle(*id);
}

// ConnectToPort: r1 the port's name, a C string in guest memory
void Kernel::connect_to_port(Thread &self) {
    Context &c = self.context;
    std::string name;
    for (uint32_t i = 0; i < 12; i++) {
        uint32_t word = memory_.read32((c.r[1] + i) & ~3u);
        char ch = static_cast<char>(word >> (((c.r[1] + i) & 3) * 8));
        if (!ch) {
            break;
        }
        name.push_back(ch);
    }
    c.r[0] = result::SUCCESS;
    c.r[1] = create_handle(insert(ClientPort{name}));
}

// SendSyncRequest: r0 session; the command is in the thread's TLS
void Kernel::send_sync_request(Thread &self) {
    Context &c = self.context;
    auto id = resolve(c.r[0]);
    KObject *o = id ? object(*id) : nullptr;
    if (auto *port = o ? std::get_if<ClientPort>(o) : nullptr) {
        c.r[0] = result::SUCCESS;
        hooks_.request(*this, self, port->name, 0);
    } else if (auto *session = o ? std::get_if<ClientSession>(o) : nullptr) {
        c.r[0] = result::SUCCESS;
        hooks_.request(*this, self, session->service, session->subhandle);
    } else {
        c.r[0] = result::INVALID_HANDLE;
    }
}

} // namespace runtime

namespace {

struct Syscall {
    uint8_t number;
    const char *name;
};

constexpr std::array SYSCALLS{
    Syscall{0x01, "ControlMemory"},     Syscall{0x02, "QueryMemory"},
    Syscall{0x03, "ExitProcess"},       Syscall{0x08, "CreateThread"},
    Syscall{0x09, "ExitThread"},        Syscall{0x0A, "SleepThread"},
    Syscall{0x0B, "GetThreadPriority"}, Syscall{0x0C, "SetThreadPriority"},
    Syscall{0x13, "CreateMutex"},       Syscall{0x14, "ReleaseMutex"},
    Syscall{0x17, "CreateEvent"},       Syscall{0x18, "SignalEvent"},
    Syscall{0x19, "ClearEvent"},        Syscall{0x1E, "CreateMemoryBlock"},
    Syscall{0x1F, "MapMemoryBlock"},    Syscall{0x20, "UnmapMemoryBlock"},
    Syscall{0x21, "CreateAddressArbiter"}, Syscall{0x22, "ArbitrateAddress"},
    Syscall{0x23, "CloseHandle"},       Syscall{0x24, "WaitSynchronization1"},
    Syscall{0x25, "WaitSynchronizationN"}, Syscall{0x27, "DuplicateHandle"},
    Syscall{0x28, "GetSystemTick"},     Syscall{0x29, "GetHandleInfo"},
    Syscall{0x2B, "GetProcessInfo"},    Syscall{0x2D, "ConnectToPort"},
    Syscall{0x32, "SendSyncRequest"},   Syscall{0x35, "GetProcessId"},
    Syscall{0x37, "GetThreadId"},       Syscall{0x38, "GetResourceLimit"},
    Syscall{0x3A, "GetResourceLimitCurrentValues"}, Syscall{0x3C, "Break"},
};

} // namespace

extern "C" const char *runtime_syscall_name(uint8_t number) {
    for (const auto &s : SYSCALLS) {
        if (s.number == number) {
            return s.name;
        }
    }
    return nullptr;
}
