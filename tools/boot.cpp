// Boots a game on the runtime and reports how far it gets: the service
// commands nothing answers yet, and where its threads are at the end.
// A tool for building the services up one by one, not the port.
//
//   boot <exheader.bin> <code.bin> <recompiled library> [seconds] [game .3ds] [update .cia] [user folder]

#include "runtime/fs.h"
#include "runtime/loader.h"
#include "runtime/services.h"
#include "runtime/system.h"

#include <chrono>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <algorithm>
#include <set>

using namespace runtime;

namespace {

std::vector<uint8_t> read_file(const char *path) {
    std::ifstream in(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), {}};
}

Library library;

Code lookup(uint32_t address) { return library.lookup(address); }

Services *services_ptr = nullptr;

void services(System &system, Thread &thread, const std::string &service, uint32_t subhandle) {
    services_ptr->request(system, thread, service, subhandle);
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
    Services handlers(exheader.program_id);
    services_ptr = &handlers;
    Fs fs(argc > 7 ? argv[7] : "user", exheader.program_id);
    if (argc > 5) {
        fs.romfs = find_romfs(argv[5], error);
        if (!fs.romfs) std::fprintf(stderr, "%s\n", error.c_str());
    }
    if (argc > 6) {
        fs.update_romfs = find_romfs(argv[6], error);
        if (!fs.update_romfs) std::fprintf(stderr, "%s\n", error.c_str());
    }
    handlers.fs = &fs;
    handlers.on_module = [](const std::string &name, uint32_t base) {
        library.place(name, base);
        std::printf("module %s %s 0x%08X\n", name.c_str(), base ? "at" : "gone from", base);
    };
    System system(config, lookup, services);
    load(system, exheader, read_file(argv[2]));
#ifdef RUNTIME_ZAKURO_GPU
    ZakuroGpu gpu(system.memory, true);
    std::printf("drawing %s\n", gpu.hardware ? "on the GPU (Vulkan)" : "in software");
#else
    NullGpu gpu(system.memory);
#endif
    handlers.gpu = &gpu;

    auto until = std::chrono::steady_clock::now() + std::chrono::duration<double>(seconds);
    auto next_shot = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    int shots = 0;
    bool went_on = system.run([&] {
        // A pressed for a tenth of each second, to get past the title as the bots do
        auto now = std::chrono::steady_clock::now();
        auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count();
        handlers.input.buttons = ms % 1000 < 100 ? 1 : 0;
        handlers.tick(system);
#ifdef RUNTIME_ZAKURO_GPU
        // both screens to a picture every five seconds
        if (now > next_shot) {
            next_shot = now + std::chrono::seconds(5);
            for (uint32_t screen = 0; screen < 2; screen++) {
                Picture p = gpu.screen(screen);
                char name[64];
                std::snprintf(name, sizeof name, "shot-%02d-%s.ppm", shots, screen ? "bottom" : "top");
                std::ofstream out(name, std::ios::binary);
                out << "P6\n" << p.width << " " << p.height << "\n255\n";
                for (size_t i = 0; i < p.rgba.size(); i += 4) out.write(reinterpret_cast<char *>(&p.rgba[i]), 3);
            }
            shots++;
        }
#endif
        return now > until;
    });
    std::printf("%s after %.1f s\n", system.kernel.exited() ? "ended" : went_on ? "running" : "stopped", seconds);
#ifdef RUNTIME_ZAKURO_GPU
    std::printf("%llu pictures in %llu vertical blanks\n", (unsigned long long)handlers.pictures, (unsigned long long)handlers.vblanks);
    std::printf("gpu busy %.1f s, %llu command lists; host idle %.1f s\n", gpu.busy, (unsigned long long)gpu.lists, system.idle);
#endif
    {
        int peak = 0;
        for (int16_t s : handlers.audio.output) peak = std::max(peak, std::abs(int(s)));
        std::printf("sound: peak %d over the last %zu samples\n", peak, handlers.audio.output.size() / 2);
    }
    std::printf("interpreted %llu instructions where the C had nothing\n", (unsigned long long)system.interpreted);
    {
        std::vector<std::pair<uint64_t, uint32_t>> top;
        for (auto [at, n] : system.interpreted_at) top.push_back({n, at});
        std::sort(top.rbegin(), top.rend());
        for (size_t i = 0; i < top.size() && i < 8; i++) std::printf("  interpreted from 0x%08X x%llu\n", top[i].second, (unsigned long long)top[i].first);
    }
    if (!system.error().empty()) std::printf("error: %s\n", system.error().c_str());
    for (size_t i = 0; i < system.kernel.thread_count(); i++) {
        Thread &t = system.kernel.thread(static_cast<ThreadId>(i));
        std::printf("thread %zu: status %d, pc 0x%08X, lr 0x%08X\n", i, static_cast<int>(t.status), t.context.r[15],
                    t.context.r[14]);
    }
    for (auto &[key, count] : handlers.unimplemented()) {
        std::printf("unanswered %-12s 0x%04X x%u\n", key.first.c_str(), key.second, count);
    }
    for (auto [page, count] : system.memory.faults()) {
        std::printf("fault at 0x%08X x%u\n", page, count);
        if (page > 0 && system.memory.faults().size() > 20) break;
    }
    return 0;
}
