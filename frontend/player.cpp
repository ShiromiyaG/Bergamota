// A game on the runtime, in a window: both screens for now (the ports put
// their own layout on top, Solstice its single screen), the keyboard, a
// gamepad, and the mouse on the bottom screen as the stylus. Sound through
// SDL3.
//
//   player [--config <file>] [--setup]
//   player <exheader.bin> <code.bin> <recompiled library> <game .3ds> <update .cia> <user folder>
//
// The settings live in config.toml (%APPDATA%\runtime by default): the
// launcher asks for the game's files when they are missing or with --setup,
// and F1 opens the overlay over the game for the rest (video, audio,
// controls). F11 switches fullscreen. RUNTIME_STATS prints a line a second
// on where the time went.
//
// The game runs on a thread of its own and the GPU on another (AsyncGpu);
// this one shows the frames the GPU hands over, one a vertical blank.

#include "settings.h"
#include "ui.h"

#include "runtime/decomp.h"
#include "runtime/async_gpu.h"
#include "runtime/fs.h"
#include "runtime/pica.h"
#include "runtime/vulkan_presenter.h"
#include "runtime/vulkan_renderer.h"
#include "runtime/loader.h"
#include "runtime/services.h"
#include "runtime/system.h"

#include <SDL3/SDL.h>
#include <imgui.h>
#include <imgui_impl_sdl3.h>
#include <imgui_impl_sdlrenderer3.h>
#include <imgui_impl_vulkan.h>
#include <SDL3/SDL_vulkan.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <deque>
#include <fstream>
#include <iterator>
#include <memory>
#include <mutex>
#include <thread>

using namespace runtime;
using frontend::Settings;

// the top screen's shape for a port's widescreen override, which finds it by
// name (Solstice's overrides/widescreen.c): width over height, 0 for the
// 3DS's own
extern "C" {
#ifdef _WIN32
__declspec(dllexport)
#else
__attribute__((visibility("default")))
#endif
volatile float runtime_top_aspect = 0;
}

namespace {

// the top screen's width shown, at its height of 240
float top_width(const Settings &s) {
    return s.video.aspect == "16:9" ? 240.0f * 16 / 9 : s.video.aspect == "21:9" ? 240.0f * 21 / 9 : 400.0f;
}

std::vector<uint8_t> read_file(const std::string &path) {
    std::ifstream in(frontend::path_of(path), std::ios::binary);
    return {std::istreambuf_iterator<char>(in), {}};
}

Library library;
Code lookup(uint32_t address) { return library.lookup(address); }
uint32_t redirect(uint32_t address) { return library.redirect(address); }
Services *services_ptr = nullptr;
void services(System &system, Thread &thread, const std::string &service, uint32_t subhandle) {
    services_ptr->request(system, thread, service, subhandle);
}

// the bindings, resolved from their names: a scancode and a gamepad button
// for each of the settings' buttons
struct Bindings {
    SDL_Scancode keys[std::size(frontend::BUTTONS)];
    SDL_GamepadButton pad[std::size(frontend::BUTTONS)];

    void resolve(const Settings &s) {
        for (size_t i = 0; i < std::size(frontend::BUTTONS); i++) {
            auto k = s.keys.find(frontend::BUTTONS[i].name);
            keys[i] = k == s.keys.end() ? SDL_SCANCODE_UNKNOWN : SDL_GetScancodeFromName(k->second.c_str());
            auto p = s.pad.find(frontend::BUTTONS[i].name);
            pad[i] = p == s.pad.end() ? SDL_GAMEPAD_BUTTON_INVALID : SDL_GetGamepadButtonFromString(p->second.c_str());
        }
    }
};

int fail(const std::string &message) {
    std::fprintf(stderr, "%s\n", message.c_str());
    SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "runtime", message.c_str(), nullptr);
    return 1;
}

} // namespace

int main(int argc, char **argv) {
    std::filesystem::path config_file = frontend::default_path();
    bool setup = false;
    std::vector<std::string> positional;
    for (int i = 1; i < argc; i++) {
        if (!std::strcmp(argv[i], "--config") && i + 1 < argc) config_file = frontend::path_of(argv[++i]);
        else if (!std::strcmp(argv[i], "--setup")) setup = true;
        else positional.push_back(argv[i]);
    }
    Settings settings;
    std::string error;
    if (!frontend::load(config_file, settings, error)) {
        std::fprintf(stderr, "%s: %s (the defaults instead)\n", frontend::utf8(config_file).c_str(), error.c_str());
    }
    if (positional.size() >= 6) {
        // the game's files on the command line, as before the settings
        settings.game = {positional[0], positional[1], positional[2], positional[3], positional[4], positional[5]};
    }

    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD | SDL_INIT_AUDIO)) return fail(std::string("SDL: ") + SDL_GetError());
    constexpr int SCALE = 2;
    SDL_Window *window = SDL_CreateWindow("runtime", 400 * SCALE, 480 * SCALE, SDL_WINDOW_RESIZABLE | SDL_WINDOW_VULKAN);
    SDL_Renderer *renderer = SDL_CreateRenderer(window, nullptr);
    if (!window || !renderer) return fail(std::string("SDL: ") + SDL_GetError());
    frontend::ui_init(window, renderer);

    if ((setup || !settings.game_ready()) && !frontend::launcher(window, renderer, settings, config_file)) {
        frontend::ui_shutdown();
        SDL_Quit();
        return 0;
    }

    // the game
    // the program: from the files named, else straight from the dumps (the
    // update's when there is one, whose program takes the game's place)
    ExHeader exheader;
    std::vector<uint8_t> exheader_bytes, code;
    if (!settings.game.exheader.empty() && !settings.game.code.empty()) {
        exheader_bytes = read_file(settings.game.exheader);
        code = read_file(settings.game.code);
    } else if (!read_program(settings.game.update.empty() ? settings.game.game : settings.game.update, exheader_bytes,
                             code, error)) {
        return fail(error);
    }
    if (!parse_exheader(exheader_bytes, exheader, error)) return fail(error);
    // the window named after the game, as its icon names it
    std::string title = read_title(settings.game.game);
    if (!title.empty()) SDL_SetWindowTitle(window, title.c_str());
    // the library: the one named, else where 3dsrecomp build installs it
    std::string library_path = settings.game.library;
    if (library_path.empty()) library_path = frontend::utf8(installed_library(exheader.program_id));
    if (!library.open(library_path, error)) {
        return fail(error + "\n(recompile the game first: 3dsrecomp build, or Solstice's launcher)");
    }
    Config config;
    config.app_bytes = exheader.app_bytes;
    config.region = exheader.region;
    Services handlers(exheader.program_id);
    services_ptr = &handlers;
    Fs fs(settings.game.user, exheader.program_id);
    fs.romfs = find_romfs(settings.game.game, error);
    if (!settings.game.update.empty()) fs.update_romfs = find_romfs(settings.game.update, error);
    if (!fs.romfs) return fail(error);
    handlers.fs = &fs;
    // the decomp in place of the functions it replaces, with a library
    // recompiled with it
    std::unique_ptr<Decomp> decomp;
    if (!settings.game.decomp.empty()) {
        decomp = std::make_unique<Decomp>();
        if (!read_decomp(settings.game.decomp, *decomp, error)) return fail(error);
    }
    System system(config, lookup, services);
    system.set_redirect(redirect);
    handlers.on_module = [&](const std::string &name, uint32_t base) {
        module_changed(library, system.memory, decomp.get(), handlers.cro, name, base);
    };
    load(system, exheader, code, decomp.get());
    if (!link_code(library, system.memory, decomp.get(), error)) return fail(error);
    // the renderer, on a device that presents to the window as well: the
    // launcher's SDL renderer and its overlay go
    ImGui_ImplSDLRenderer3_Shutdown();
    ImGui_ImplSDL3_Shutdown();
    SDL_DestroyRenderer(renderer);
    renderer = nullptr;
    Uint32 extension_count = 0;
    const char *const *extension_names = SDL_Vulkan_GetInstanceExtensions(&extension_count);
    std::vector<std::string> extensions(extension_names, extension_names + extension_count);
    pica::Pica pica_gpu(system.memory);
    VulkanRenderer vulkan(system.memory, extensions);
    if (!vulkan.ready()) {
        return fail("the game needs Vulkan, which this computer's GPU or driver does not give (" + vulkan.error + ")");
    }
    pica_gpu.backend = &vulkan;
    // the frames read back to the CPU only for RUNTIME_DUMP; the window
    // shows them straight from the GPU
    vulkan.cpu_pictures = SDL_getenv("RUNTIME_DUMP") != nullptr;
    VulkanPresenter presenter(vulkan, [&](VkInstance instance, VkSurfaceKHR *surface) {
        return SDL_Vulkan_CreateSurface(window, instance, nullptr, surface);
    });
    if (!presenter.ready()) return fail("the window cannot show Vulkan's pictures (" + presenter.error + ")");
    // the overlay on the same device
    ImGui_ImplSDL3_InitForVulkan(window);
    VkFormat overlay_format = presenter.format();
    ImGui_ImplVulkan_InitInfo overlay_info{};
    overlay_info.Instance = presenter.instance();
    overlay_info.PhysicalDevice = presenter.physical();
    overlay_info.Device = presenter.device();
    overlay_info.QueueFamily = presenter.family();
    overlay_info.Queue = presenter.queue();
    overlay_info.DescriptorPool = presenter.overlay_pool();
    overlay_info.MinImageCount = 2;
    overlay_info.ImageCount = 3;
    overlay_info.MSAASamples = VK_SAMPLE_COUNT_1_BIT;
    overlay_info.UseDynamicRendering = true;
    overlay_info.PipelineRenderingCreateInfo = {VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO_KHR};
    overlay_info.PipelineRenderingCreateInfo.colorAttachmentCount = 1;
    overlay_info.PipelineRenderingCreateInfo.pColorAttachmentFormats = &overlay_format;
    ImGui_ImplVulkan_Init(&overlay_info);
    {
        // its font goes up through the queue the renderer shares
        std::lock_guard g(presenter.queue_lock());
        ImGui_ImplVulkan_CreateFontsTexture();
    }
    // the game's vertex programs as shaders, made once and kept beside the settings
    vulkan.cache_in(frontend::utf8(config_file.parent_path() / "shaders"));
    // the GPU on a thread of its own, beside the game's (async_gpu.h);
    // RUNTIME_SYNC_GPU=1 keeps it on the game's, to compare
    std::unique_ptr<AsyncGpu> async; // declared after what it drives: it goes first
    if (!SDL_getenv("RUNTIME_SYNC_GPU")) async = std::make_unique<AsyncGpu>(pica_gpu, system.memory);
    handlers.gpu = async ? static_cast<Gpu *>(async.get()) : &pica_gpu;

    SDL_Gamepad *gamepad = nullptr;
    // the DSP's frames as they come, at its own rate; SDL resamples
    SDL_AudioSpec spec{SDL_AUDIO_S16, 2, int(AUDIO_RATE)};
    SDL_AudioStream *sound = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, nullptr, nullptr);
    if (sound) SDL_ResumeAudioStreamDevice(sound);

    // what the settings say, applied now and whenever the overlay moves them
    Bindings bindings;
    bool both = true;
    auto apply = [&] {
        bindings.resolve(settings);
        SDL_SetWindowFullscreen(window, settings.video.fullscreen);
        both = settings.video.layout != "top";
        if (sound) SDL_SetAudioStreamGain(sound, settings.audio.mute ? 0.0f : settings.audio.volume / 100.0f);
        vulkan.between = settings.video.between ? 1 : 0;
    };
    apply();
    frontend::Overlay overlay;

    // the game on a thread of its own; this one keeps the window, reads the
    // input and shows the frames, so none of it stops the game
    std::mutex input_lock;
    Input input;
    std::atomic<bool> quit{false};
    std::thread game([&] {
        system.run([&] {
            {
                std::lock_guard g(input_lock);
                handlers.input = input;
            }
            handlers.tick(system);
            auto &out = handlers.audio.output;
            if (sound && !out.empty()) {
                // the queue kept near 60 ms by playing a little faster or slower
                // (half a percent at most, not heard): the DSP's clock and the
                // sound card's drift apart; a quarter of a second queued at
                // most, the rest is late
                constexpr int BYTES_PER_SECOND = int(AUDIO_RATE) * 4, TARGET = BYTES_PER_SECOND * 60 / 1000;
                int queued = SDL_GetAudioStreamQueued(sound);
                float off = std::clamp(float(queued - TARGET) / TARGET, -1.0f, 1.0f);
                SDL_SetAudioStreamFrequencyRatio(sound, 1.0f + 0.005f * off);
                if (queued < BYTES_PER_SECOND / 4) SDL_PutAudioStreamData(sound, out.data(), int(out.size() * 2));
                out.clear();
            }
            return quit.load(std::memory_order_relaxed);
        });
        quit = true;
    });

    const bool print_stats = SDL_getenv("RUNTIME_STATS") != nullptr;
    // RUNTIME_DUMP=folder: the first RUNTIME_DUMP_COUNT (600) top screens shown, as
    // <frame>-<game|between>.ppm, to look at what the player showed
    const char *dump = SDL_getenv("RUNTIME_DUMP");
    int dump_left = SDL_getenv("RUNTIME_DUMP_COUNT") ? std::atoi(SDL_getenv("RUNTIME_DUMP_COUNT")) : 600;
    auto second = std::chrono::steady_clock::now(), last_shown = second;
    frontend::Stats stats, counting;
    bool told[5]{};
    double busy_before = 0, idle_before = 0, host_gpu_before = 0;
    uint64_t pictures_before = 0;
    std::deque<Frame> waiting;
    Frame showing;
    while (!quit) {
        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            if (event.type == SDL_EVENT_QUIT) quit = true;
            if (event.type == SDL_EVENT_GAMEPAD_ADDED && !gamepad) gamepad = SDL_OpenGamepad(event.gdevice.which);
            if (event.type == SDL_EVENT_KEY_DOWN && event.key.scancode == SDL_SCANCODE_F11 && !event.key.repeat) {
                settings.video.fullscreen = !settings.video.fullscreen;
                SDL_SetWindowFullscreen(window, settings.video.fullscreen);
            }
            overlay.event(event);
        }

        // the input, from the keyboard and the gamepad together; none while
        // the overlay is open
        Input now;
        if (!overlay.open) {
            const bool *keys = SDL_GetKeyboardState(nullptr);
            float circle[4] = {}; // up, down, left, right
            for (size_t i = 0; i < std::size(frontend::BUTTONS); i++) {
                bool held = bindings.keys[i] != SDL_SCANCODE_UNKNOWN && keys[bindings.keys[i]];
                if (gamepad && bindings.pad[i] != SDL_GAMEPAD_BUTTON_INVALID) held |= SDL_GetGamepadButton(gamepad, bindings.pad[i]);
                if (!held) continue;
                if (frontend::BUTTONS[i].bit) now.buttons |= frontend::BUTTONS[i].bit;
                else circle[i - 12] = 1;
            }
            float cx = circle[3] - circle[2], cy = circle[0] - circle[1];
            if (gamepad) {
                float gx = SDL_GetGamepadAxis(gamepad, SDL_GAMEPAD_AXIS_LEFTX) / 32767.0f;
                float gy = -SDL_GetGamepadAxis(gamepad, SDL_GAMEPAD_AXIS_LEFTY) / 32767.0f;
                if (std::abs(gx) > std::abs(cx)) cx = gx;
                if (std::abs(gy) > std::abs(cy)) cy = gy;
            }
            now.circle_x = cx;
            now.circle_y = cy;
            // the mouse on the bottom screen is the stylus
            float mx = 0, my = 0;
            SDL_MouseButtonFlags mouse = SDL_GetMouseState(&mx, &my);
            int ww = 0, wh = 0;
            SDL_GetWindowSize(window, &ww, &wh);
            float scale = 1, x0 = 0, y0 = 0, top = top_width(settings);
            VulkanPresenter::letterbox(float(ww), float(wh), both, top, scale, x0, y0);
            mx = (mx - x0) / scale - (top - 320) / 2, my = (my - y0) / scale;
            if (both && (mouse & SDL_BUTTON_LMASK) && my >= 240 && my < 480 && mx >= 0 && mx < 320) {
                now.touch = std::make_pair(uint16_t(mx), uint16_t(my - 240));
            }
        }
        {
            std::lock_guard g(input_lock);
            input = now;
        }

        // the frames the GPU finished, one a vertical blank, shown in order;
        // two waiting at most (a screen refreshing slower than the game's 59.83
        // Hz), the older ones skipped
        for (Frame f; handlers.gpu->next_frame(f);) waiting.push_back(std::move(f));
        while (waiting.size() > 2) waiting.pop_front();
        bool fresh = !waiting.empty();
        if (fresh) {
            showing = std::move(waiting.front());
            waiting.pop_front();
            if (dump && dump_left > 0 && !showing.screens[0].rgba.empty()) {
                dump_left--;
                char name[512];
                std::snprintf(name, sizeof name, "%s/%06llu-%s.ppm", dump, (unsigned long long)showing.number,
                              showing.between ? "between" : "game");
                const Picture &p = showing.screens[0];
                std::ofstream out(frontend::path_of(name), std::ios::binary);
                out << "P6\n" << p.width << " " << p.height << "\n255\n";
                for (size_t i = 0; i < p.rgba.size(); i += 4) out.write(reinterpret_cast<const char *>(&p.rgba[i]), 3);
            }
        } else if (!settings.video.vsync && !overlay.open) {
            // nothing new, and no refresh to wait for
            SDL_Delay(1);
            continue;
        }

        // the screens straight from the GPU, the overlay over them at the
        // window's own size
        ImGui_ImplVulkan_NewFrame();
        ImGui_ImplSDL3_NewFrame();
        ImGui::NewFrame();
        bool changed = false;
        overlay.draw(settings, stats, config_file, changed);
        ImGui::Render();
        int pw = 0, ph = 0;
        SDL_GetWindowSizeInPixels(window, &pw, &ph);
        VulkanPresenter::View view;
        view.width = uint32_t(pw), view.height = uint32_t(ph);
        view.both = both;
        view.smooth = settings.video.smooth;
        view.top = top_width(settings);
        runtime_top_aspect = view.top > 400 ? view.top / 240 : 0;
        view.vsync = settings.video.vsync;
        presenter.present(fresh ? &showing : nullptr, view,
                          [](VkCommandBuffer cmd) { ImGui_ImplVulkan_RenderDrawData(ImGui::GetDrawData(), cmd); });
        if (changed) apply();

        // how the second went
        auto done = std::chrono::steady_clock::now();
        if (fresh) {
            counting.worst_ms = std::max(counting.worst_ms, std::chrono::duration<double, std::milli>(done - last_shown).count());
            last_shown = done;
            counting.shown++;
        }
        if (done - second >= std::chrono::seconds(1)) {
            counting.gpu_ms = (showing.busy - busy_before) * 1000;
            counting.idle_ms = (system.idle - idle_before) * 1000;
            stats = counting;
            // the first draw of a session asking for what the renderer does
            // not do yet, noted beside the config: any session tells
            static constexpr const char *LACKING[5] = {"w buffer", "gas", "procedural textures", "geometry shader", "shadow"};
            for (int i = 0; i < 5; i++) {
                if (!vulkan.lacking[i] || told[i]) continue;
                told[i] = true;
                std::time_t now = std::time(nullptr);
                char when[32];
                std::strftime(when, sizeof when, "%Y-%m-%d %H:%M:%S", std::localtime(&now));
                std::ofstream(config_file.parent_path() / "renderer-lacks.txt", std::ios::app)
                    << when << ": " << LACKING[i] << ", frame " << showing.number << "\n";
                if (print_stats) std::printf("the renderer lacks %s (frame %llu)\n", LACKING[i], (unsigned long long)showing.number);
            }
            if (print_stats) {
                std::printf("shown %d, the game's pictures %llu, worst gap %.0f ms, gpu %.0f ms, idle %.0f ms, the host's GPU %.0f ms\n",
                            stats.shown, (unsigned long long)(handlers.pictures - pictures_before), stats.worst_ms, stats.gpu_ms,
                            stats.idle_ms, (vulkan.gpu_seconds - host_gpu_before) * 1000);
                host_gpu_before = vulkan.gpu_seconds;
                pictures_before = handlers.pictures;
                std::fflush(stdout);
            }
            counting = {};
            second = done;
            busy_before = showing.busy;
            idle_before = system.idle;
        }
    }
    game.join();
    if (!system.error().empty()) std::fprintf(stderr, "stopped: %s\n", system.error().c_str());
    // where the C had nothing, for the next recompile to take in
    if (save_hints(system.interpreted_at, exheader, error) < 0) std::fprintf(stderr, "hints: %s\n", error.c_str());
    frontend::save(config_file, settings, error);
    presenter.idle();
    ImGui_ImplVulkan_Shutdown();
    ImGui_ImplSDL3_Shutdown();
    ImGui::DestroyContext();
    SDL_Quit();
    return 0;
}
