// Boots a game on the runtime and reports how far it gets: the service
// commands nothing answers yet, and where its threads are at the end.
// A tool for building the services up one by one, not the port.
//
//   boot <exheader.bin> <code.bin> <recompiled library> [seconds] [game .3ds] [update .cia] [user folder]
//
// For comparing with Zakuro (tools/compare.py), on the virtual clock:
//   RUNTIME_FRAMES=n        run n frames instead of seconds (seconds, when given, still
//                           cap the run on the host's clock)
//   RUNTIME_REPLAY=file     the input a Zakuro recording holds, frame by frame
//   RUNTIME_SHOTS=folder    both screens every RUNTIME_SHOT_EVERY frames (100)
//   RUNTIME_USAGE=file      the commands asked for, as ZAKURO_USAGE writes them
//   RUNTIME_DECOMP=folder   the decomp (decomp.bin, decomp.txt) in place of the
//                           functions it replaces; the library built with it
//   RUNTIME_BETWEEN=n       frames drawn between the game's, n pictures a blank: 1 as
//                           "60 FPS" in the player, 2 as 120 Hz (shots <frame>-<part>-shown)
//   RUNTIME_SCALE=n         the buffers drawn n times as large each way (2.25: 1080p)
//   RUNTIME_RESCALE=f:n     and changed to n at frame f, as the player may
//   RUNTIME_CPU_PICTURES=0  no pictures read back, as the player draws (to time it; no shots)
//   RUNTIME_GENERIC_FRAG=1  the one fragment shader for every state, none made for a shape
//   RUNTIME_SAMPLE=n        where the game's thread spent its time: the n functions
//                           it was found in most, sampled every millisecond
//   RUNTIME_SAMPLE_FROM=s   sampled from s seconds in (on the host's clock)
//   RUNTIME_SAMPLE_GPU=1    the GPU's thread sampled as well
//   RUNTIME_TRAIL=n         after each shot, the next n frames shown; on the host's
//                           clock, the first n from frame RUNTIME_TRAIL_FROM
//                           (RUNTIME_NO_INPUT=1: no A presses; RUNTIME_REPLAY: its input)
//   RUNTIME_ASYNC=0         the GPU on the game's thread (on its own by default, as
//                           in the player)

#include "runtime/decomp.h"
#include "runtime/fs.h"
#include "runtime/pica.h"
#include "runtime/async_gpu.h"
#include "runtime/pica_sketch.h"
#ifdef RUNTIME_VULKAN
#include "runtime/vulkan_renderer.h"
#endif
#include "runtime/loader.h"
#include "runtime/sampler.h"
#include "runtime/services.h"
#include "runtime/session.h"
#include "runtime/system.h"

#include <chrono>
#include <functional>
#include <memory>
#include <cstdio>
#include <filesystem>
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
    // the game, opened as the player opens it (session.h); RUNTIME_DECOMP=folder:
    // the decomp's decomp.bin and decomp.txt, with a library recompiled with it
    // (3dsrecomp build --decomp)
    Session session;
    Session::Files files{argv[1], argv[2], argv[3], argc > 5 ? argv[5] : "", argc > 6 ? argv[6] : "",
                         argc > 7 ? argv[7] : "user", env("RUNTIME_DECOMP") ? env("RUNTIME_DECOMP") : ""};
    std::string error;
    auto told = [](const std::string &name, uint32_t base) {
        std::printf("module %s %s 0x%08X\n", name.c_str(), base ? "at" : "gone from", base);
    };
    if (!session.open(files, config, error, told)) {
        std::fprintf(stderr, "%s\n", error.c_str());
        return 1;
    }
    const ExHeader &exheader = session.exheader;
    Library &library = session.library;
    System &system = *session.system;
    Services &handlers = *session.services;
    std::printf("program %016llX, %zu functions, %zu modules, %u MiB\n", (unsigned long long)exheader.program_id,
                library.entry_count(), library.module_count(), exheader.app_bytes >> 20);
    // the runtime's own renderer (docs/vulkan-renderer.md); RUNTIME_RENDERER=sketch:
    // its draws filled flat on canvases saved with the shots instead
    const std::string renderer = env("RUNTIME_RENDERER") ? env("RUNTIME_RENDERER") : "";
    std::unique_ptr<pica::Sketch> sketch;
#ifdef RUNTIME_VULKAN
    std::unique_ptr<VulkanRenderer> vulkan;
#endif
    std::unique_ptr<pica::Pica> pica_gpu;
    std::unique_ptr<AsyncGpu> async;
    {
        pica_gpu = std::make_unique<pica::Pica>(system.memory);
        if (renderer == "sketch") {
            sketch = std::make_unique<pica::Sketch>();
            pica_gpu->backend = sketch.get();
        }
#ifdef RUNTIME_VULKAN
        if (renderer != "sketch") {
            vulkan = std::make_unique<VulkanRenderer>(system.memory);
            if (vulkan->ready()) pica_gpu->backend = vulkan.get();
            else std::printf("no Vulkan: %s\n", vulkan->error.c_str());
            // RUNTIME_BETWEEN=n: frames drawn between the game's, n pictures
            // a blank; the shots' trail says which of the frames shown were
            vulkan->between = env("RUNTIME_BETWEEN") ? std::atoi(env("RUNTIME_BETWEEN")) : 0;
            // RUNTIME_CPU_PICTURES=0: no pictures read back (the player's way, to time it; no shots)
            if (env("RUNTIME_CPU_PICTURES") && *env("RUNTIME_CPU_PICTURES") == '0') vulkan->cpu_pictures = false;
            // RUNTIME_SCALE=n: the buffers drawn n times as large each way (the shots stay the console's size)
            if (env("RUNTIME_SCALE")) vulkan->scale = float(std::atof(env("RUNTIME_SCALE")));
            // RUNTIME_SHADER_CACHE=folder: the translated vertex programs kept across runs
            if (env("RUNTIME_SHADER_CACHE")) vulkan->cache_in(env("RUNTIME_SHADER_CACHE"));
        }
#endif
        std::printf("drawing with the runtime's renderer\n");
        handlers.gpu = pica_gpu.get();
        // the GPU on a thread of its own, as the player runs it; RUNTIME_ASYNC=0
        // keeps it on the game's
        if (!(env("RUNTIME_ASYNC") && *env("RUNTIME_ASYNC") == '0')) {
            async = std::make_unique<AsyncGpu>(*pica_gpu, system.memory);
            handlers.gpu = async.get();
            // on the virtual clock, time waits for it as it waits for the game
            system.settle = [&] {
                if (async->settled()) return false;
                async->locked([] {});
                return true;
            };
        }
    }
    // what a screen shows now, the GPU's thread held meanwhile
    auto screen_now = [&](uint32_t s) {
        Picture p;
        if (async) async->locked([&] { p = pica_gpu->screen(s); });
        else p = pica_gpu->screen(s);
        return p;
    };

    auto save = [](const std::string &name, const Picture &p) {
        std::ofstream out(name, std::ios::binary);
        out << "P6\n" << p.width << " " << p.height << "\n255\n";
        for (size_t i = 0; i < p.rgba.size(); i += 4) out.write(reinterpret_cast<const char *>(&p.rgba[i]), 3);
    };
    // the frames as the player gets them, a vertical blank each: the last
    // one, and with RUNTIME_SHOTS those due
    Frame latest;
    uint64_t next_shot_frame = shot_every;
    // on the host's clock with RUNTIME_SHOTS: the first RUNTIME_TRAIL frames
    // shown, every one (the intro as the player plays it)
    int trail = !frames && shots_dir && env("RUNTIME_TRAIL") ? std::atoi(env("RUNTIME_TRAIL")) : 0;
    const uint64_t trail_from = env("RUNTIME_TRAIL_FROM") ? std::stoull(env("RUNTIME_TRAIL_FROM")) : 0;
    std::string trail_line;
    auto take_frames = [&] {
        Frame f;
        while (handlers.gpu->next_frame(f)) {
            if (frames && shots_dir && f.number >= next_shot_frame) {
                char prefix[512];
                std::snprintf(prefix, sizeof prefix, "%s/%06llu", shots_dir, (unsigned long long)next_shot_frame);
                save(std::string(prefix) + "-top.ppm", f.screens[0]);
                save(std::string(prefix) + "-bottom.ppm", f.screens[1]);
                next_shot_frame = f.number / shot_every * shot_every + shot_every;
                // with frames between: the next few frames shown, to see one fall between two
#ifdef RUNTIME_VULKAN
                if (vulkan && vulkan->between) trail = env("RUNTIME_TRAIL") ? std::atoi(env("RUNTIME_TRAIL")) : 8;
#endif
                trail_line.clear();
            } else if (trail > 0 && f.number >= trail_from) {
                trail--;
                char name[512];
                std::snprintf(name, sizeof name, "%s/%06llu-%d-shown-top.ppm", shots_dir, (unsigned long long)f.number, f.part);
                save(name, f.screens[0]);
                trail_line += f.between ? " between" : " game";
                if (!trail) std::printf("frames shown after %llu:%s\n", (unsigned long long)(next_shot_frame - shot_every), trail_line.c_str());
            }
            latest = std::move(f);
        }
    };

    auto until = std::chrono::steady_clock::now() + std::chrono::duration<double>(seconds);
    Sampler sampler, gpu_sampler;
    const size_t sample_count = env("RUNTIME_SAMPLE") ? std::stoul(env("RUNTIME_SAMPLE")) : 0;
    auto sample_from = std::chrono::steady_clock::now() +
                       std::chrono::duration<double>(env("RUNTIME_SAMPLE_FROM") ? std::atof(env("RUNTIME_SAMPLE_FROM")) : 0);
    bool sampling = false;
    const bool no_input = env("RUNTIME_NO_INPUT") != nullptr;
    auto next_shot = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    int shots = 0;
    uint64_t sketch_frame = 0;
    bool went_on = system.run([&] {
        auto now = std::chrono::steady_clock::now();
        if (sample_count && !sampling && now >= sample_from) {
            sampling = true;
            sampler.start_here();
            if (async && env("RUNTIME_SAMPLE_GPU")) gpu_sampler.start(async->native_thread());
        }
        if (frames) {
            // a frame at a time: the recording's input for it, the screens
            auto &in = replay.at(handlers.vblanks);
            handlers.input.buttons = in.buttons;
            handlers.input.circle_x = in.x;
            handlers.input.circle_y = in.y;
            handlers.input.touch.reset();
            if (in.tx >= 0) handlers.input.touch = std::make_pair(uint16_t(in.tx), uint16_t(in.ty));
            handlers.tick(system);
            take_frames();
#ifdef RUNTIME_VULKAN
            // RUNTIME_RESCALE=frame:n: the resolution changed to n at that frame, as the player may
            if (vulkan && env("RUNTIME_RESCALE")) {
                unsigned long long at = 0;
                float to = 1;
                if (std::sscanf(env("RUNTIME_RESCALE"), "%llu:%f", &at, &to) == 2 && handlers.vblanks == at) vulkan->scale = to;
            }
#endif
            if (sketch && shots_dir && handlers.vblanks >= sketch_frame + shot_every) {
                sketch_frame = handlers.vblanks / shot_every * shot_every;
                char prefix[512];
                std::snprintf(prefix, sizeof prefix, "%s/%06llu-sketch", shots_dir, (unsigned long long)sketch_frame);
                sketch->save(prefix);
            }
            // seconds given with RUNTIME_FRAMES: a hung run still stops and says where it is
            return handlers.vblanks >= frames || (argc > 4 && seconds > 0 && now >= until);
        }
        // A pressed for a tenth of each second, to get past the title as the bots
        // do; RUNTIME_NO_INPUT=1 leaves the game alone
        auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count();
        handlers.input.buttons = ms % 1000 < 100 && !no_input ? 1 : 0;
        if (replay_path) {
            // a recording on the host's clock: as the player would get it
            auto &in = replay.at(handlers.vblanks);
            handlers.input.buttons = in.buttons;
            handlers.input.circle_x = in.x;
            handlers.input.circle_y = in.y;
            handlers.input.touch.reset();
            if (in.tx >= 0) handlers.input.touch = std::make_pair(uint16_t(in.tx), uint16_t(in.ty));
        }
        handlers.tick(system);
        take_frames();
        // both screens to a picture every five seconds, in build/shots
        if (now > next_shot) {
            next_shot = now + std::chrono::seconds(5);
            std::error_code ec;
            std::filesystem::create_directories("build/shots", ec);
            for (uint32_t screen = 0; screen < 2; screen++) {
                char name[64];
                std::snprintf(name, sizeof name, "build/shots/shot-%02d-%s.ppm", shots, screen ? "bottom" : "top");
                save(name, latest.number ? latest.screens[screen] : screen_now(screen));
            }
            shots++;
        }
        return now > until;
    });
    // the frames still on their way
    if (async) async->locked([] {});
    take_frames();
    if (sampling) {
        sampler.stop();
        std::printf("the game's thread, %llu samples:\n", (unsigned long long)sampler.samples());
        for (const std::string &line : sampler.report(library, sample_count)) std::printf("  %s\n", line.c_str());
        gpu_sampler.stop();
        if (gpu_sampler.samples()) {
            std::printf("the GPU's thread, %llu samples:\n", (unsigned long long)gpu_sampler.samples());
            for (const std::string &line : gpu_sampler.report(library, sample_count)) std::printf("  %s\n", line.c_str());
        }
    }
    std::printf("%s after %.1f s\n", system.kernel.exited() ? "ended" : went_on ? "running" : "stopped", seconds);
    std::printf("%llu pictures in %llu vertical blanks\n", (unsigned long long)handlers.pictures, (unsigned long long)handlers.vblanks);
    if (async) std::printf("the game waited %.1f s for the GPU's thread\n", async->waited);
    std::printf("gpu busy %.1f s, %llu command lists; host idle %.1f s\n", pica_gpu->busy, (unsigned long long)pica_gpu->lists, system.idle);
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
    {
        // the threads whose code took the most
        std::vector<std::pair<double, uint32_t>> threads;
        for (auto [id, spent] : system.profile.by_thread) threads.push_back({spent, id});
        std::sort(threads.rbegin(), threads.rend());
        for (size_t i = 0; i < threads.size() && i < 8 && threads[i].first > 0.1; i++) {
            Thread &t = system.kernel.thread(threads[i].second);
            std::printf("  thread %u: code %.1f s, priority 0x%02X, core %d, entry 0x%08X\n", t.id, threads[i].first, t.priority,
                        t.core, t.entry);
        }
    }
    for (auto &[key, spent] : handlers.time_spent) {
        if (spent > 0.2) std::printf("  %s 0x%04X %.1f s\n", key.first.c_str(), key.second, spent);
    }
#ifdef RUNTIME_VULKAN
    if (vulkan) {
        std::printf("vulkan: %llu draws, %llu triangles, %llu uploads, %llu write-backs, %llu submits\n",
                    (unsigned long long)vulkan->stats.draws, (unsigned long long)vulkan->stats.triangles, (unsigned long long)vulkan->stats.uploads,
                    (unsigned long long)vulkan->stats.write_backs, (unsigned long long)vulkan->stats.submits);
        std::printf("vulkan time: vertices %.1f s, draws %.1f s, syncs %.1f s; the host's GPU %.1f s\n", vulkan->stats.time_vertices,
                    vulkan->stats.time_render, vulkan->stats.time_sync, vulkan->stats.gpu_seconds);
        std::printf("the host's GPU: passes %.1f s, frames between %.1f s (copies %.1f s), screens %.1f s\n", vulkan->stats.gpu_parts[0],
                    vulkan->stats.gpu_parts[2], vulkan->stats.gpu_parts[1], vulkan->stats.gpu_parts[3]);
        std::printf("passes %llu: ended for another target %llu, a texture that was the target %llu, the rest %llu\n",
                    (unsigned long long)vulkan->stats.passes, (unsigned long long)vulkan->stats.pass_ends[0],
                    (unsigned long long)vulkan->stats.pass_ends[1], (unsigned long long)vulkan->stats.pass_ends[2]);
        uint64_t compiled, loaded, failed;
        vulkan->translation_counts(compiled, loaded, failed);
        std::printf("vertex programs: %llu translated, %llu from the cache, %llu failed; %llu draws with them, %llu between reused\n",
                    (unsigned long long)compiled, (unsigned long long)loaded, (unsigned long long)failed,
                    (unsigned long long)vulkan->stats.translated_draws, (unsigned long long)vulkan->stats.reused_draws);
        std::printf("fragment shaders: %llu draws with one made for their state\n", (unsigned long long)vulkan->stats.specialized_draws);
        std::printf("draws the renderer lacks: gas %llu, geometry shader modes %llu\n",
                    (unsigned long long)vulkan->stats.lacking[0], (unsigned long long)vulkan->stats.lacking[1]);
        std::printf("write-backs: cpu %llu, front end %llu, fill %llu, texture %llu\n", (unsigned long long)vulkan->stats.write_back_causes[0],
                    (unsigned long long)vulkan->stats.write_back_causes[1], (unsigned long long)vulkan->stats.write_back_causes[2],
                    (unsigned long long)vulkan->stats.write_back_causes[3]);
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
    if (const char *hints = std::getenv("RUNTIME_HINTS"); hints && *hints == '1') {
        std::string why;
        int added = save_hints(system.interpreted_at, exheader, why);
        if (added < 0) std::printf("hints: %s\n", why.c_str());
        else std::printf("hints: %d new\n", added);
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
