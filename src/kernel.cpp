// the kernel calls the games make (34 in USUM's code.bin, two of them
// debug ones left out; the CROs make none): threads, synchronization, the
// address arbiter, memory, time, and SendSyncRequest into the services.
// Threads run cooperatively on one host thread for now, as on the 3DS's
// application core.

#include "runtime/runtime.h"

#include <array>
#include <cstdint>

namespace {

struct Syscall {
    uint8_t number;
    const char *name;
};

constexpr std::array SYSCALLS{
    Syscall{0x01, "ControlMemory"},
    Syscall{0x02, "QueryMemory"},
    Syscall{0x03, "ExitProcess"},
    Syscall{0x08, "CreateThread"},
    Syscall{0x09, "ExitThread"},
    Syscall{0x0A, "SleepThread"},
    Syscall{0x0B, "GetThreadPriority"},
    Syscall{0x0C, "SetThreadPriority"},
    Syscall{0x13, "CreateMutex"},
    Syscall{0x14, "ReleaseMutex"},
    Syscall{0x17, "CreateEvent"},
    Syscall{0x18, "SignalEvent"},
    Syscall{0x19, "ClearEvent"},
    Syscall{0x1E, "CreateMemoryBlock"},
    Syscall{0x1F, "MapMemoryBlock"},
    Syscall{0x20, "UnmapMemoryBlock"},
    Syscall{0x21, "CreateAddressArbiter"},
    Syscall{0x22, "ArbitrateAddress"},
    Syscall{0x23, "CloseHandle"},
    Syscall{0x24, "WaitSynchronization1"},
    Syscall{0x25, "WaitSynchronizationN"},
    Syscall{0x27, "DuplicateHandle"},
    Syscall{0x28, "GetSystemTick"},
    Syscall{0x29, "GetHandleInfo"},
    Syscall{0x2B, "GetProcessInfo"},
    Syscall{0x2D, "ConnectToPort"},
    Syscall{0x32, "SendSyncRequest"},
    Syscall{0x35, "GetProcessId"},
    Syscall{0x37, "GetThreadId"},
    Syscall{0x38, "GetResourceLimit"},
    Syscall{0x3A, "GetResourceLimitCurrentValues"},
    Syscall{0x3C, "Break"},
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
