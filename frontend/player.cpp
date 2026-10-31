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

#include "settings.h"
#include "ui.h"

#include "runtime/decomp.h"
#include "runtime/async_gpu.h"
#include "runtime/fs.h"
#include "runtime/pica.h"
#include "runtime/vulkan_renderer.h"
#include "runtime/loader.h"
#include "runtime/services.h"
#include "runtime/system.h"

#include <SDL3/SDL.h>
#include <imgui.h>
#include <imgui_impl_sdl3.h>
#include <imgui_impl_sdlrenderer3.h>

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <functional>
#include <memory>
#include <iterator>

using namespace runtime;
using frontend::Settings;

namespace {

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
    SDL_Window *window = SDL_CreateWindow("runtime", 400 * SCALE, 480 * SCALE, SDL_WINDOW_RESIZABLE);
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
    // the renderer: the runtime's own, or Zakuro's through its bridge
    struct {
        std::function<Picture(uint32_t)> screen;
        std::function<double()> busy;
        std::function<void()> frame_done = [] {};
    } gpu;
    std::unique_ptr<pica::Pica> pica_gpu;
    std::unique_ptr<VulkanRenderer> vulkan;
    std::unique_ptr<AsyncGpu> async; // declared after what it drives: it goes first
#ifdef RUNTIME_ZAKURO_GPU
    std::unique_ptr<ZakuroGpu> zakuro_gpu;
#endif
    if (settings.video.renderer != "zakuro") {
        pica_gpu = std::make_unique<pica::Pica>(system.memory);
        vulkan = std::make_unique<VulkanRenderer>(system.memory);
        if (vulkan->ready()) {
            pica_gpu->backend = vulkan.get();
            // the GPU on a thread of its own, beside the game's (async_gpu.h);
            // RUNTIME_SYNC_GPU=1 keeps it on the game's, to compare
            if (!SDL_getenv("RUNTIME_SYNC_GPU")) async = std::make_unique<AsyncGpu>(*pica_gpu, system.memory);
            handlers.gpu = async ? static_cast<Gpu *>(async.get()) : pica_gpu.get();
            gpu = {[&](uint32_t s) { return pica_gpu->screen(s); }, [&] { return pica_gpu->busy; }, [&] { pica_gpu->frame_done(); }};
            std::printf("drawing with the runtime's renderer\n");
        } else {
            std::fprintf(stderr, "no Vulkan for the runtime's renderer (%s)\n", vulkan->error.c_str());
            vulkan.reset();
            pica_gpu.reset();
        }
    }
#ifdef RUNTIME_ZAKURO_GPU
    // Zakuro's renderer, to compare, in a build that has it
    if (!handlers.gpu) {
        zakuro_gpu = std::make_unique<ZakuroGpu>(system.memory, true, exheader.program_id);
        handlers.gpu = zakuro_gpu.get();
        gpu = {[&](uint32_t s) { return zakuro_gpu->screen(s); }, [&] { return zakuro_gpu->busy; }};
        std::printf("drawing with Zakuro's renderer\n");
    }
#endif
    if (!handlers.gpu) return fail("the game needs Vulkan, which this computer's GPU or driver does not give");

    SDL_Texture *screens[2] = {
        SDL_CreateTexture(renderer, SDL_PIXELFORMAT_ABGR8888, SDL_TEXTUREACCESS_STREAMING, 400, 240),
        SDL_CreateTexture(renderer, SDL_PIXELFORMAT_ABGR8888, SDL_TEXTUREACCESS_STREAMING, 320, 240),
    };
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
        SDL_SetRenderVSync(renderer, settings.video.vsync ? 1 : 0);
        for (SDL_Texture *t : screens) {
            SDL_SetTextureScaleMode(t, settings.video.smooth ? SDL_SCALEMODE_LINEAR : SDL_SCALEMODE_NEAREST);
        }
        both = settings.video.layout != "top";
        SDL_SetRenderLogicalPresentation(renderer, 400, both ? 480 : 240, SDL_LOGICAL_PRESENTATION_LETTERBOX);
        if (sound) SDL_SetAudioStreamGain(sound, settings.audio.mute ? 0.0f : settings.audio.volume / 100.0f);
    };
    apply();
    frontend::Overlay overlay;

    bool quit = false;
    const bool print_stats = SDL_getenv("RUNTIME_STATS") != nullptr;
    auto next_frame = std::chrono::steady_clock::now();
    auto second = next_frame, last_shown = next_frame;
    frontend::Stats stats, counting;
    double busy_before = 0, idle_before = 0, screens_ms = 0, send_ms = 0;
    uint64_t frames_seen = 0;
    bool new_frame_before = false;
    system.run([&] {
        handlers.tick(system);
        auto &out = handlers.audio.output;
        if (sound && !out.empty()) {
            // a quarter of a second queued at most, the rest is late
            if (SDL_GetAudioStreamQueued(sound) < int(AUDIO_RATE / 4 * 4)) {
                SDL_PutAudioStreamData(sound, out.data(), int(out.size() * 2));
            }
            out.clear();
        }
        auto now = std::chrono::steady_clock::now();
        if (now < next_frame) return quit;
        next_frame = now + std::chrono::microseconds(16667);

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
        uint32_t buttons = 0;
        float cx = 0, cy = 0;
        bool touching = false;
        float mx = 0, my = 0;
        if (!overlay.open) {
            const bool *keys = SDL_GetKeyboardState(nullptr);
            float circle[4] = {}; // up, down, left, right
            for (size_t i = 0; i < std::size(frontend::BUTTONS); i++) {
                bool held = bindings.keys[i] != SDL_SCANCODE_UNKNOWN && keys[bindings.keys[i]];
                if (gamepad && bindings.pad[i] != SDL_GAMEPAD_BUTTON_INVALID) held |= SDL_GetGamepadButton(gamepad, bindings.pad[i]);
                if (!held) continue;
                if (frontend::BUTTONS[i].bit) buttons |= frontend::BUTTONS[i].bit;
                else circle[i - 12] = 1;
            }
            cx = circle[3] - circle[2];
            cy = circle[0] - circle[1];
            if (gamepad) {
                float gx = SDL_GetGamepadAxis(gamepad, SDL_GAMEPAD_AXIS_LEFTX) / 32767.0f;
                float gy = -SDL_GetGamepadAxis(gamepad, SDL_GAMEPAD_AXIS_LEFTY) / 32767.0f;
                if (std::abs(gx) > std::abs(cx)) cx = gx;
                if (std::abs(gy) > std::abs(cy)) cy = gy;
            }
            // the mouse on the bottom screen is the stylus
            SDL_MouseButtonFlags mouse = SDL_GetMouseState(&mx, &my);
            SDL_RenderCoordinatesFromWindow(renderer, mx, my, &mx, &my);
            touching = both && (mouse & SDL_BUTTON_LMASK) && my >= 240 && my < 480 && mx >= 40 && mx < 360;
        }
        handlers.input.buttons = buttons;
        handlers.input.circle_x = cx;
        handlers.input.circle_y = cy;
        handlers.input.touch.reset();
        if (touching) handlers.input.touch = std::make_pair(uint16_t(mx - 40), uint16_t(my - 240));

        // the screens
        auto drawing = std::chrono::steady_clock::now();
        SDL_SetRenderDrawColor(renderer, 0, 0, 0, 255);
        SDL_RenderClear(renderer);
        // frames between (settings.video.between): the game frame just done is
        // drawn between first, then the screens are taken: a 30 Hz scene goes
        // ..., N-1, between, N, between, ... at 60, a game frame behind
        // the frame between on the tick a new game frame comes in (it is the
        // one from the frame before to that one), the game's own the tick
        // after; two new ones in a row (a scene at 60) show the game's
        // all of it with the GPU's thread idle and the renderer to this one
        Picture pictures[2];
        auto take = [&] {
            bool show_between = false;
            if (vulkan) {
                vulkan->between = settings.video.between ? 1 : 0;
                vulkan->prepare_between();
                bool new_frame = vulkan->game_frames != frames_seen;
                frames_seen = vulkan->game_frames;
                show_between = new_frame && !new_frame_before;
                new_frame_before = new_frame;
            }
            for (uint32_t s = 0; s < (both ? 2u : 1u); s++) {
                pictures[s] = gpu.screen(s);
                // the top screen only: the bottom one is drawn at 60 as it is
                if (vulkan && vulkan->between && s == 0) {
                    const auto &shown = pica_gpu->screens[s];
                    uint32_t stride = shown.stride ? shown.stride : 240 * pica::bytes_per_pixel(shown.format);
                    Picture b;
                    bool got = vulkan->picture_between(s, shown.address, stride, shown.format, b);
                    if (got && show_between && b.width == pictures[s].width) pictures[s] = std::move(b);
                }
            }
            gpu.frame_done();
        };
        if (async) async->locked(take);
        else take();
        for (uint32_t s = 0; s < (both ? 2u : 1u); s++) {
            Picture &p = pictures[s];
            SDL_UpdateTexture(screens[s], nullptr, p.rgba.data(), int(p.width * 4));
            SDL_FRect at{s ? 40.0f : 0.0f, s ? 240.0f : 0.0f, float(p.width), 240.0f};
            SDL_RenderTexture(renderer, screens[s], nullptr, &at);
        }
        auto taken = std::chrono::steady_clock::now();
        auto sent = taken;
        screens_ms += std::chrono::duration<double, std::milli>(taken - drawing).count();
        send_ms += std::chrono::duration<double, std::milli>(sent - taken).count();
        // the overlay over them, at the window's own size
        ImGui_ImplSDLRenderer3_NewFrame();
        ImGui_ImplSDL3_NewFrame();
        ImGui::NewFrame();
        bool changed = false;
        overlay.draw(settings, stats, config_file, changed);
        ImGui::Render();
        SDL_SetRenderLogicalPresentation(renderer, 0, 0, SDL_LOGICAL_PRESENTATION_DISABLED);
        ImGui_ImplSDLRenderer3_RenderDrawData(ImGui::GetDrawData(), renderer);
        SDL_SetRenderLogicalPresentation(renderer, 400, both ? 480 : 240, SDL_LOGICAL_PRESENTATION_LETTERBOX);
        SDL_RenderPresent(renderer);
        if (changed) apply();

        // how the second went
        auto done = std::chrono::steady_clock::now();
        counting.worst_ms = std::max(counting.worst_ms, std::chrono::duration<double, std::milli>(done - last_shown).count());
        last_shown = done;
        counting.shown++;
        if (done - second >= std::chrono::seconds(1)) {
            counting.gpu_ms = (gpu.busy() - busy_before) * 1000;
            counting.idle_ms = (system.idle - idle_before) * 1000;
            stats = counting;
            if (print_stats) {
                std::printf("shown %d, worst gap %.0f ms, gpu %.0f ms, idle %.0f ms, screens %.0f ms, sending %.0f ms\n",
                            stats.shown, stats.worst_ms, stats.gpu_ms, stats.idle_ms, screens_ms, send_ms);
                screens_ms = send_ms = 0;
                std::fflush(stdout);
            }
            counting = {};
            second = done;
            busy_before = gpu.busy();
            idle_before = system.idle;
        }
        return quit;
    });
    if (!system.error().empty()) std::fprintf(stderr, "stopped: %s\n", system.error().c_str());
    // where the C had nothing, for the next recompile to take in
    if (save_hints(system.interpreted_at, exheader, error) < 0) std::fprintf(stderr, "hints: %s\n", error.c_str());
    frontend::save(config_file, settings, error);
    frontend::ui_shutdown();
    SDL_Quit();
    return 0;
}
