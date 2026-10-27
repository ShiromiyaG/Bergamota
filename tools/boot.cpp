// Boots a game on the runtime and reports how far it gets: the service
// commands nothing answers yet, and where its threads are at the end.
// A tool for building the services up one by one, not the port.
//
//   boot <exheader.bin> <code.bin> <recompiled library> [seconds] [game .3ds] [update .cia] [user folder]
//
// For comparing with Zakuro (tools/compare.py), on the virtual clock:
//   RUNTIME_FRAMES=n        run n frames instead of seconds
//   RUNTIME_REPLAY=file     the input a Zakuro recording holds, frame by frame
//   RUNTIME_SHOTS=folder    both screens every RUNTIME_SHOT_EVERY frames (100)
//   RUNTIME_USAGE=file      the commands asked for, as ZAKURO_USAGE writes them
//   RUNTIME_DECOMP=folder   the decomp (decomp.bin, decomp.txt) in place of the
//                           functions it replaces; the library built with it

#include "runtime/decomp.h"
#include "runtime/fs.h"
#include "runtime/pica.h"
#include "runtime/pica_sketch.h"
#ifdef RUNTIME_VULKAN
#include "runtime/vulkan_renderer.h"
#endif
#include "runtime/loader.h"
#include "runtime/services.h"
#include "runtime/system.h"

#include <chrono>
#include <functional>
#include <memory>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <algorithm>
#include <set>
#include <sstream>
#include <string>

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

// a Zakuro recording: the clock it started at, and a line per change of
// input, from the frame it starts at
struct Replay {
    uint64_t clock = 0;
    struct Change {
        uint64_t frame;
        uint32_t buttons;
        float x, y;
        int tx, ty;
    };
    std::vector<Change> changes;
    size_t next = 0;
    Change current{0, 0, 0, 0, -1, -1};

    bool open(const char *path) {
        std::ifstream in(path);
        std::string line;
        if (!std::getline(in, line) || line.rfind("clock ", 0) != 0) return false;
        clock = std::stoull(line.substr(6));
        while (std::getline(in, line)) {
            std::istringstream fields(line);
            Change c;
            std::string buttons;
            if (fields >> c.frame >> buttons >> c.x >> c.y >> c.tx >> c.ty) {
                c.buttons = uint32_t(std::stoul(buttons, nullptr, 16));
                changes.push_back(c);
            }
        }
        return true;
    }
    const Change &at(uint64_t frame) {
        while (next < changes.size() && changes[next].frame <= frame) current = changes[next++];
        return current;
    }
};

const char *env(const char *name) {
    const char *value = std::getenv(name);
    return value && *value ? value : nullptr;
}

} // namespace

int main(int argc, char **argv) {
    // what it said so far stays in a log even when a hung run is stopped
    std::setvbuf(stdout, nullptr, _IONBF, 0);
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
    Replay replay;
    const char *replay_path = env("RUNTIME_REPLAY"), *shots_dir = env("RUNTIME_SHOTS");
    if (replay_path && !replay.open(replay_path)) {
        std::fprintf(stderr, "bad recording %s\n", replay_path);
        return 1;
    }
    uint64_t frames = env("RUNTIME_FRAMES") ? std::stoull(env("RUNTIME_FRAMES")) : 0;
    uint64_t shot_every = env("RUNTIME_SHOT_EVERY") ? std::stoull(env("RUNTIME_SHOT_EVERY")) : 100;
    Config config;
    config.virtual_clock = frames != 0;
    if (replay_path) config.boot_clock_ms = replay.clock;
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
    // RUNTIME_DECOMP=folder: the decomp's decomp.bin and decomp.txt, with a
    // library recompiled with it (3dsrecomp build --decomp)
    std::unique_ptr<Decomp> decomp;
    if (const char *dir = env("RUNTIME_DECOMP")) {
        decomp = std::make_unique<Decomp>();
        if (!read_decomp(dir, *decomp, error)) {
            std::fprintf(stderr, "%s\n", error.c_str());
            return 1;
        }
    }
    System system(config, lookup, services);
    handlers.on_module = [&](const std::string &name, uint32_t base) {
        module_changed(library, system.memory, decomp.get(), handlers.cro, name, base);
        std::printf("module %s %s 0x%08X\n", name.c_str(), base ? "at" : "gone from", base);
    };
    load(system, exheader, read_file(argv[2]), decomp.get());
    if (!link_code(library, system.memory, decomp.get(), error)) {
        std::fprintf(stderr, "%s\n", error.c_str());
        return 1;
    }
    // RUNTIME_RENDERER=native: the runtime's own renderer (docs/vulkan-renderer.md)
    struct {
        std::function<Picture(uint32_t)> screen;
        std::function<double()> busy;
        std::function<uint64_t()> lists;
    } gpu;
    // RUNTIME_RENDERER=sketch: the same, its draws filled flat on canvases saved with the shots
    const std::string renderer = env("RUNTIME_RENDERER") ? env("RUNTIME_RENDERER") : "";
    const bool native = renderer == "native" || renderer == "sketch";
    std::unique_ptr<pica::Sketch> sketch;
#ifdef RUNTIME_VULKAN
    std::unique_ptr<VulkanRenderer> vulkan;
#endif
    std::unique_ptr<pica::Pica> pica_gpu;
#ifdef RUNTIME_ZAKURO_GPU
    std::unique_ptr<ZakuroGpu> zakuro_gpu;
    if (!native) {
        zakuro_gpu = std::make_unique<ZakuroGpu>(system.memory, true);
        std::printf("drawing with Zakuro's renderer %s\n", zakuro_gpu->hardware ? "on the GPU (Vulkan)" : "in software");
        handlers.gpu = zakuro_gpu.get();
        gpu = {[&](uint32_t s) { return zakuro_gpu->screen(s); }, [&] { return zakuro_gpu->busy; },
               [&] { return zakuro_gpu->lists; }};
    }
#endif
    if (!handlers.gpu) {
        pica_gpu = std::make_unique<pica::Pica>(system.memory);
        if (renderer == "sketch") {
            sketch = std::make_unique<pica::Sketch>();
            pica_gpu->backend = sketch.get();
        }
#ifdef RUNTIME_VULKAN
        if (renderer == "native") {
            // RUNTIME_PIPELINE_CACHE=file: the compiled pipelines kept between runs
            vulkan = std::make_unique<VulkanRenderer>(system.memory, env("RUNTIME_PIPELINE_CACHE") ? env("RUNTIME_PIPELINE_CACHE") : "");
            if (vulkan->ready()) pica_gpu->backend = vulkan.get();
            else std::printf("no Vulkan: %s\n", vulkan->error.c_str());
        }
#endif
        std::printf("drawing with the runtime's renderer\n");
        handlers.gpu = pica_gpu.get();
        gpu = {[&](uint32_t s) { return pica_gpu->screen(s); }, [&] { return pica_gpu->busy; },
               [&] { return pica_gpu->lists; }};
    }

    auto until = std::chrono::steady_clock::now() + std::chrono::duration<double>(seconds);
    auto next_shot = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    int shots = 0;
    uint64_t shot_frame = 0;
    bool went_on = system.run([&] {
        auto now = std::chrono::steady_clock::now();
        if (frames) {
            // a frame at a time: the recording's input for it, the screens
            auto &in = replay.at(handlers.vblanks);
            handlers.input.buttons = in.buttons;
            handlers.input.circle_x = in.x;
            handlers.input.circle_y = in.y;
            handlers.input.touch.reset();
            if (in.tx >= 0) handlers.input.touch = std::make_pair(uint16_t(in.tx), uint16_t(in.ty));
            handlers.tick(system);
#ifdef RUNTIME_ZAKURO_GPU
            if (shots_dir && handlers.vblanks >= shot_frame + shot_every) {
                shot_frame = handlers.vblanks / shot_every * shot_every;
                if (pica_gpu) pica_gpu->frame_done();
                if (sketch) {
                    char prefix[512];
                    std::snprintf(prefix, sizeof prefix, "%s/%06llu-sketch", shots_dir, (unsigned long long)shot_frame);
                    sketch->save(prefix);
                }
                for (uint32_t screen = 0; screen < 2; screen++) {
                    Picture p = gpu.screen(screen);
                    char name[512];
                    std::snprintf(name, sizeof name, "%s/%06llu-%s.ppm", shots_dir, (unsigned long long)shot_frame,
                                  screen ? "bottom" : "top");
                    std::ofstream out(name, std::ios::binary);
                    out << "P6\n" << p.width << " " << p.height << "\n255\n";
                    for (size_t i = 0; i < p.rgba.size(); i += 4) out.write(reinterpret_cast<char *>(&p.rgba[i]), 3);
                }
            }
#endif
            return handlers.vblanks >= frames;
        }
        // A pressed for a tenth of each second, to get past the title as the bots do
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
    std::printf("gpu busy %.1f s, %llu command lists; host idle %.1f s\n", gpu.busy(), (unsigned long long)gpu.lists(), system.idle);
#endif
    if (pica_gpu) {
        const auto &f = pica_gpu->feature_draws;
        std::printf("draws using fog %llu, gas %llu, the shadow mode %llu, a procedural texture %llu, a logic op %llu "
                    "(of %llu)\n",
                    (unsigned long long)f[pica::Pica::USES_FOG], (unsigned long long)f[pica::Pica::USES_GAS],
                    (unsigned long long)f[pica::Pica::USES_SHADOW], (unsigned long long)f[pica::Pica::USES_PROCTEX],
                    (unsigned long long)f[pica::Pica::USES_LOGIC_OP], (unsigned long long)pica_gpu->draws);
    }
    {
        int peak = 0;
        for (int16_t s : handlers.audio.output) peak = std::max(peak, std::abs(int(s)));
        std::printf("sound: peak %d over the last %zu samples\n", peak, handlers.audio.output.size() / 2);
    }
    std::printf("time: code %.1f s, interpreter %.1f s, syscalls %.1f s, scheduler %.1f s, outside %.1f s; %llu slices, %llu syscalls\n",
                system.profile.code, system.profile.interpreter, system.profile.syscalls, system.profile.schedule,
                system.profile.outside, (unsigned long long)system.profile.slices, (unsigned long long)system.profile.svcs);
    for (int n = 0; n < 256; n++) {
        if (system.profile.by_svc[n] > 0.2) std::printf("  svc 0x%02X %.1f s\n", n, system.profile.by_svc[n]);
    }
    for (auto &[key, spent] : handlers.time_spent) {
        if (spent > 0.2) std::printf("  %s 0x%04X %.1f s\n", key.first.c_str(), key.second, spent);
    }
#ifdef RUNTIME_VULKAN
    if (vulkan) {
        std::printf("vulkan: %llu draws, %llu triangles, %llu uploads, %llu write-backs, %llu submits\n",
                    (unsigned long long)vulkan->draws, (unsigned long long)vulkan->triangles, (unsigned long long)vulkan->uploads,
                    (unsigned long long)vulkan->write_backs, (unsigned long long)vulkan->submits);
        std::printf("vulkan time: vertices %.1f s, draws %.1f s, syncs %.1f s\n", vulkan->time_vertices, vulkan->time_render, vulkan->time_sync);
        std::printf("vulkan pipelines: %llu made in %.3f s\n", (unsigned long long)vulkan->pipelines, vulkan->time_pipelines);
        std::printf("write-backs: cpu %llu, front end %llu, fill %llu, texture %llu\n", (unsigned long long)vulkan->write_back_causes[0],
                    (unsigned long long)vulkan->write_back_causes[1], (unsigned long long)vulkan->write_back_causes[2],
                    (unsigned long long)vulkan->write_back_causes[3]);
    }
#endif
    if (sketch) {
        std::printf("sketch: %llu draws, %llu vertices shaded, %llu draws skipped\n", (unsigned long long)pica_gpu->draws,
                    (unsigned long long)sketch->shaded(), (unsigned long long)sketch->skipped);
    }
    std::printf("interpreted %llu instructions where the C had nothing\n", (unsigned long long)system.interpreted);
    {
        std::vector<std::pair<uint64_t, uint32_t>> top;
        for (auto [at, n] : system.interpreted_at) top.push_back({n, at});
        std::sort(top.rbegin(), top.rend());
        for (size_t i = 0; i < top.size() && i < 8; i++) std::printf("  interpreted from 0x%08X x%llu\n", top[i].second, (unsigned long long)top[i].first);
    }
    if (!system.error().empty()) std::printf("error: %s\n", system.error().c_str());
    if (const char *usage = env("RUNTIME_USAGE")) {
        std::ofstream out(usage);
        for (auto &[key, count] : handlers.usage) {
            char line[128];
            std::snprintf(line, sizeof line, "%llu ipc %s 0x%04X\n", (unsigned long long)count, key.first.c_str(), key.second);
            out << line;
        }
    }
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
