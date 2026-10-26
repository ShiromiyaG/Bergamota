// The kernel: threads, kernel objects and handles, waits, and the syscalls
// the games make. Translated from Zakuro's kernel (Rust, MIT, Copyright (c)
// 2026 fearkov; see LICENSE-zakuro), cut to what USUM, XY and ORAS use.
//
// The games' code is C (recompiled or decompiled) that gives control back
// on every syscall, its whole state in a Context: a guest thread is a
// Context, and switching threads is picking another one. Threads run
// cooperatively, one at a time, as on the 3DS's application core.

#ifndef RUNTIME_KERNEL_H
#define RUNTIME_KERNEL_H

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <variant>
#include <vector>

#include "recomp.h"

namespace runtime {

using Handle = uint32_t;
using ObjectId = uint32_t;
using ThreadId = uint32_t;

constexpr Handle CURRENT_PROCESS = 0xFFFF8001;
constexpr Handle CURRENT_THREAD = 0xFFFF8000;

// the ARM11 runs at 268.111856 MHz
constexpr uint64_t CPU_CLOCK_HZ = 268111856;
// the return address a fresh thread gets: returning there ends it
constexpr uint32_t THREAD_EXIT = 0xFFFFFFF0;

uint64_t nanos_to_ticks(uint64_t nanos);

namespace result {
constexpr uint32_t SUCCESS = 0;
constexpr uint32_t INVALID_HANDLE = 0xD8E007F7;
constexpr uint32_t INVALID_ENUM_VALUE = 0xD8E00405;
constexpr uint32_t OUT_OF_MEMORY = 0xD86007F3;
constexpr uint32_t NOT_FOUND = 0xD88007FA;
constexpr uint32_t TIMEOUT = 0x09401BFE;
constexpr uint32_t NOT_IMPLEMENTED = 0xF8C007F4;
} // namespace result

// how an event wakes what waits on it
enum class ResetType { OneShot, Sticky, Pulse };

struct Event {
    ResetType reset;
    bool signaled = false;
};

struct Mutex {
    std::optional<ThreadId> owner;
    uint32_t lock_count = 0; // the 3DS's mutexes are recursive
};

struct Semaphore {
    int32_t count;
    int32_t max_count;
};

// the 3DS's futex: threads park on a guest address until someone signals it
struct AddressArbiter {
    std::vector<std::pair<ThreadId, uint32_t>> waiters;
};

struct SharedMemory {
    uint32_t address; // in the owning process, or 0 when the kernel picks
    uint32_t size;
    std::optional<uint32_t> mapped_at;
};

// the game's end of a connection to a service
struct ClientSession {
    std::string service;
    uint32_t subhandle;
};

// a named port the game connects to (srv:)
struct ClientPort {
    std::string name;
};

struct ThreadObject {
    ThreadId id;
};

struct ProcessObject {};
struct ResourceLimit {};

using KObject = std::variant<ThreadObject, Event, Mutex, Semaphore, AddressArbiter, SharedMemory, ClientSession,
                             ClientPort, ProcessObject, ResourceLimit>;

enum class ThreadStatus { Ready, Running, Sleeping, WaitSync, WaitArbiter, Dead };

// which syscall blocked a thread: its result registers are written when it
// wakes
enum class WaitSyscall { None, WaitSynchronization1, WaitSynchronizationN, ArbitrateAddress, SleepThread };

struct Thread {
    ThreadId id;
    uint32_t priority; // 0 is the highest, 0x3F the lowest
    ThreadStatus status = ThreadStatus::Ready;
    uint32_t tls;
    // the guest's registers; vfp and fpscr live here, the Context points at
    // them
    Context context{};
    uint32_t vfp[32]{};
    uint32_t fpscr = 0;

    std::vector<ObjectId> wait_objects;
    bool wait_all = false;
    std::optional<uint64_t> wakeup_at;
    std::optional<uint32_t> wait_address;
    WaitSyscall wait_syscall = WaitSyscall::None;
    // set when the wait ended: the object's index, or nullopt for a timeout
    std::optional<std::optional<uint32_t>> wait_result;
};

// what the kernel needs from the rest of the runtime
struct GuestMemory {
    virtual ~GuestMemory() = default;
    virtual uint32_t read32(uint32_t address) = 0;
    virtual void write32(uint32_t address, uint32_t value) = 0;
};

struct Clock {
    virtual ~Clock() = default;
    // the time in CPU ticks
    virtual uint64_t ticks() = 0;
};

class Kernel;

// the services and the memory syscalls, answered elsewhere
struct Hooks {
    virtual ~Hooks() = default;
    // SendSyncRequest: the command buffer is in the thread's TLS
    virtual void request(Kernel &, Thread &, const std::string &service, uint32_t subhandle) = 0;
    // ControlMemory, QueryMemory, CreateMemoryBlock, MapMemoryBlock,
    // UnmapMemoryBlock, GetProcessInfo: they read and write the context's
    // registers as the syscall does
    virtual void memory_syscall(Kernel &, Thread &, uint32_t number) = 0;
    // a page for a new thread's TLS
    virtual uint32_t allocate_tls() = 0;
};

class Kernel {
public:
    Kernel(GuestMemory &memory, Clock &clock, Hooks &hooks);

    // the main thread, at entry with its stack
    ThreadId start(uint32_t entry, uint32_t stack_top, uint32_t priority);

    // runs one syscall for the current thread (ctx->svc), as the code left it
    void syscall(uint32_t number);

    // picks the thread to run next; false when every thread is blocked (the
    // caller sleeps until next_wakeup() or an outside event)
    bool schedule();
    std::optional<uint64_t> next_wakeup() const;

    Thread *current();
    Thread &thread(ThreadId id) { return *threads_[id]; }
    size_t thread_count() const { return threads_.size(); }

    // for the services: events they signal (vertical blank, GPU interrupts)
    std::pair<ObjectId, Handle> create_event(ResetType reset);
    void signal_event(ObjectId object);
    ObjectId insert(KObject object);
    Handle create_handle(ObjectId object);
    std::optional<ObjectId> resolve(Handle handle);
    KObject *object(ObjectId id);

    bool exited() const { return exited_; }

private:
    struct Slot {
        KObject object;
        uint32_t refcount = 0;
    };

    void add_ref(ObjectId id);
    void release(ObjectId id);
    bool is_signaled(ObjectId object, ThreadId waiter);
    void acquire(ObjectId object, ThreadId waiter);
    void begin_wait(std::vector<ObjectId> objects, bool wait_all, std::optional<uint64_t> timeout);
    void end_wait(Thread &thread);
    bool try_satisfy_wait(Thread &thread, uint64_t now);
    std::optional<ThreadId> pick_next() const;
    void apply_wait_result(Thread &thread);
    void end_thread(Thread &thread);
    std::optional<ThreadId> resolve_thread(Handle handle);

    // the syscalls
    void create_thread(Thread &self);
    void sleep_thread(Thread &self);
    void create_mutex(Thread &self);
    void release_mutex(Thread &self);
    void create_event(Thread &self);
    void signal_or_clear(Thread &self, bool signal);
    void create_address_arbiter(Thread &self);
    void arbitrate_address(Thread &self);
    void wait_synchronization1(Thread &self);
    void wait_synchronization_n(Thread &self);
    void duplicate_handle(Thread &self);
    void connect_to_port(Thread &self);
    void send_sync_request(Thread &self);

    GuestMemory &memory_;
    Clock &clock_;
    Hooks &hooks_;
    std::vector<std::unique_ptr<Thread>> threads_;
    std::optional<ThreadId> current_;
    std::vector<std::optional<Slot>> slots_;
    std::vector<ObjectId> free_;
    std::unordered_map<Handle, ObjectId> handles_;
    Handle next_handle_ = 0x00010000; // well above 0, so a zeroed handle fails loudly
    std::vector<ObjectId> pulsed_;
    bool exited_ = false;
};

} // namespace runtime

#endif
