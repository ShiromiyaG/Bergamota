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

#include "runtime/fs.h"
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
    ExHeader exheader;
    if (!parse_exheader(read_file(settings.game.exheader), exheader, error) || !library.open(settings.game.library, error)) {
        return fail(error);
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
    handlers.on_module = [](const std::string &name, uint32_t base) { library.place(name, base); };
    System system(config, lookup, services);
    load(system, exheader, read_file(settings.game.code));
    ZakuroGpu gpu(system.memory, true, exheader.program_id);
    std::printf("drawing %s\n", gpu.hardware ? "on the GPU (Vulkan)" : "in software");
    handlers.gpu = &gpu;

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
    double busy_before = 0, idle_before = 0;
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
        for (uint32_t s = 0; s < (both ? 2u : 1u); s++) {
            Picture p = gpu.screen(s);
            SDL_UpdateTexture(screens[s], nullptr, p.rgba.data(), int(p.width * 4));
            SDL_FRect at{s ? 40.0f : 0.0f, s ? 240.0f : 0.0f, float(p.width), 240.0f};
            SDL_RenderTexture(renderer, screens[s], nullptr, &at);
        }
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
            counting.gpu_ms = (gpu.busy - busy_before) * 1000;
            counting.idle_ms = (system.idle - idle_before) * 1000;
            stats = counting;
            if (print_stats) {
                std::printf("shown %d, worst gap %.0f ms, gpu %.0f ms, idle %.0f ms, drawing %.1f ms\n", stats.shown,
                            stats.worst_ms, stats.gpu_ms, stats.idle_ms,
                            std::chrono::duration<double, std::milli>(done - drawing).count());
                std::fflush(stdout);
            }
            counting = {};
            second = done;
            busy_before = gpu.busy;
            idle_before = system.idle;
        }
        return quit;
    });
    if (!system.error().empty()) std::fprintf(stderr, "stopped: %s\n", system.error().c_str());
    frontend::save(config_file, settings, error);
    frontend::ui_shutdown();
    SDL_Quit();
    return 0;
}
