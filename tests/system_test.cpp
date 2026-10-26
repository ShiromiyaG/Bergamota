// The system end to end, with guest code written by hand the way 3dsrecomp
// writes it: a C function per address, each one running until a syscall
// and leaving r15 where the next one starts. Two threads meet on the
// address arbiter: the main thread waits on a word in its heap, a second
// thread sets it and wakes the first, which ends the process.

#include "runtime/system.h"

#include <cstdio>
#include <cstdlib>

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

constexpr uint32_t WORD = 0x08000000; // the first word of the heap
constexpr uint32_t ARBITER = 0x08000004;

void svc(Context *ctx, uint32_t next, uint32_t number) {
    ctx->r[15] = next;
    ctx->svc = number;
    ctx->exit = EXIT_SVC;
}

// main: allocate a heap page, then the second thread
void main_0(Context *ctx) {
    ctx->r[0] = 3; // ControlMemory: allocate
    ctx->r[1] = WORD;
    ctx->r[2] = 0;
    ctx->r[3] = 0x1000;
    ctx->r[4] = 3;
    svc(ctx, 0x100010, 0x01);
}

void main_1(Context *ctx) {
    ctx->r[0] = 0;
    svc(ctx, 0x100020, 0x21); // CreateAddressArbiter
}

void main_2(Context *ctx) {
    mem_write32(ctx, ARBITER, ctx->r[1]);
    ctx->r[0] = 0x31; // priority, below the main thread's
    ctx->r[1] = 0x100100;
    ctx->r[2] = 0;
    ctx->r[3] = 0x08000800; // its stack, in the heap page
    ctx->r[4] = 0;
    svc(ctx, 0x100030, 0x08); // CreateThread
}

// waits while the word is below 1
void main_3(Context *ctx) {
    ctx->r[0] = mem_read32(ctx, ARBITER);
    ctx->r[1] = WORD;
    ctx->r[2] = 1; // WaitIfLessThan
    ctx->r[3] = 1;
    svc(ctx, 0x100040, 0x22);
}

void main_4(Context *ctx) {
    mem_write32(ctx, WORD + 8, 0xC0DE);
    svc(ctx, 0x100050, 0x03); // ExitProcess
}

void other_0(Context *ctx) {
    mem_write32(ctx, WORD, 1);
    ctx->r[0] = mem_read32(ctx, ARBITER);
    ctx->r[1] = WORD;
    ctx->r[2] = 0; // Signal
    ctx->r[3] = 1;
    svc(ctx, 0x100110, 0x22);
}

void other_1(Context *ctx) {
    ctx->r[15] = THREAD_EXIT; // returns from its entry: the thread ends
}

Code lookup(uint32_t address) {
    switch (address) {
    case 0x100000: return main_0;
    case 0x100010: return main_1;
    case 0x100020: return main_2;
    case 0x100030: return main_3;
    case 0x100040: return main_4;
    case 0x100100: return other_0;
    case 0x100110: return other_1;
    default: return nullptr;
    }
}

void two_threads_meet() {
    Config config;
    System system(config, lookup, nullptr);
    system.start(0x100000, 0x10000000, 0x30);
    int steps = 0;
    system.run([&] { return ++steps > 1000; });
    CHECK(system.kernel.exited());
    CHECK(system.memory.read32(WORD) == 1);
    CHECK(system.memory.read32(WORD + 8) == 0xC0DE);
    CHECK(system.memory.faults().empty());
    CHECK(steps < 20);
}

void memory_layout() {
    Config config;
    System system(config, lookup, nullptr);
    // the system pages are there, read-only
    CHECK(system.memory.read8(map::CONFIG_MEM_VADDR + 0x02) == 0x02);
    CHECK(system.memory.read8(map::SHARED_PAGE_VADDR + 0x04) == 1);
    // a linear allocation's address gives its physical one
    auto block = system.memory.allocate(Region::Application, 0x2000);
    CHECK(block && block->paddr == map::FCRAM_PADDR);
    Mapping free = system.memory.query(0x09000000);
    CHECK(free.state == MemoryState::Free);
}

} // namespace

int main() {
    two_threads_meet();
    memory_layout();
    std::printf(failures ? "%d failure(s)\n" : "system: all passed\n", failures);
    return failures ? EXIT_FAILURE : EXIT_SUCCESS;
}
