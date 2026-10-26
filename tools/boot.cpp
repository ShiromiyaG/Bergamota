// Boots a game on the runtime and reports how far it gets: every service
// command it makes (the first time), with the services that are not there
// yet answered "success, nothing", and where its threads are at the end.
// A tool for building the services up one by one, not the port.
//
//   boot <exheader.bin> <code.bin> <recompiled library> [seconds]

#include "runtime/loader.h"
#include "runtime/system.h"

#include <chrono>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <set>

using namespace runtime;

namespace {

std::vector<uint8_t> read_file(const char *path) {
    std::ifstream in(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), {}};
}

Library library;

Code lookup(uint32_t address) { return library.lookup(address); }

std::set<std::pair<std::string, uint32_t>> seen;

// srv: hands out sessions by name; everything else: success and zeros
void services(System &system, Thread &thread, const std::string &service, uint32_t) {
    uint32_t buffer = thread.tls + map::TLS_COMMAND_BUFFER;
    Memory &m = system.memory;
    uint32_t header = m.read32(buffer);
    uint32_t command = header >> 16;
    if (seen.insert({service, command}).second) {
        std::printf("ipc %-12s 0x%04X\n", service.c_str(), command);
    }
    if (service == "srv:" && command == 0x0005) { // GetServiceHandle: name in words 1-2, length in 3
        char name[9] = {};
        m.read_bytes(buffer + 4, name, 8);
        name[std::min<uint32_t>(m.read32(buffer + 12), 8)] = 0;
        Handle handle = system.kernel.create_handle(system.kernel.insert(ClientSession{name, 0}));
        m.write32(buffer, 0x00050042);
        m.write32(buffer + 4, 0);
        m.write32(buffer + 8, 0);
        m.write32(buffer + 12, handle);
        return;
    }
    m.write32(buffer, command << 16 | 0x40 * 4);
    for (uint32_t i = 1; i <= 4; i++) m.write32(buffer + i * 4, 0);
}

} // namespace

int main(int argc, char **argv) {
    if (argc < 4) {
        std::fprintf(stderr, "boot <exheader.bin> <code.bin> <library> [seconds]\n");
        return 2;
    }
    double seconds = argc > 4 ? std::atof(argv[4]) : 10;
    ExHeader exheader;
    std::string error;
    if (!parse_exheader(read_file(argv[1]), exheader, error) || !library.open(argv[3], error)) {
        std::fprintf(stderr, "%s\n", error.c_str());
        return 1;
    }
    std::printf("program %016llX, %zu functions, %zu modules, %u MiB\n", (unsigned long long)exheader.program_id,
                library.entry_count(), library.module_count(), exheader.app_bytes >> 20);
    Config config;
    config.app_bytes = exheader.app_bytes;
    config.region = exheader.region;
    System system(config, lookup, services);
    load(system, exheader, read_file(argv[2]));

    auto until = std::chrono::steady_clock::now() + std::chrono::duration<double>(seconds);
    bool went_on = system.run([&] { return std::chrono::steady_clock::now() > until; });
    std::printf("%s after %.1f s\n", system.kernel.exited() ? "ended" : went_on ? "running" : "stopped", seconds);
    std::printf("interpreted %llu instructions where the C had nothing\n", (unsigned long long)system.interpreted);
    if (!system.error().empty()) std::printf("error: %s\n", system.error().c_str());
    for (size_t i = 0; i < system.kernel.thread_count(); i++) {
        Thread &t = system.kernel.thread(static_cast<ThreadId>(i));
        std::printf("thread %zu: status %d, pc 0x%08X, lr 0x%08X\n", i, static_cast<int>(t.status), t.context.r[15],
                    t.context.r[14]);
    }
    for (auto [page, count] : system.memory.faults()) {
        std::printf("fault at 0x%08X x%u\n", page, count);
        if (page > 0 && system.memory.faults().size() > 20) break;
    }
    return 0;
}
