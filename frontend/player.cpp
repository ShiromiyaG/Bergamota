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

#include "scenes.h"
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
#include "runtime/session.h"
#include "runtime/system.h"

#include <SDL3/SDL.h>
#include <imgui.h>
#include <imgui_impl_sdl3.h>
#include <imgui_impl_sdlrenderer3.h>
#include <imgui_impl_vulkan.h>
#include <SDL3/SDL_vulkan.h>

#include <algorithm>
#include <atomic>
#include <bit>
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
#include <set>
#include <thread>

using namespace runtime;
using frontend::Settings;


namespace {

// the top screen's width shown, at its height of 240
float top_width(const Settings &s) {
    return s.video.aspect == "16:9" ? 240.0f * 16 / 9 : s.video.aspect == "21:9" ? 240.0f * 21 / 9 : 400.0f;
}

// the pictures a vertical blank shows (VulkanRenderer::between): one, or
// with the display's rate as many as its refreshes in a blank, rounded
// (two at 120 and 144 Hz, three at 165 and 180)
int between_parts(const Settings &s, SDL_Window *window) {
    if (!s.video.between) return 0;
    if (!s.video.between_display) return 1;
    const SDL_DisplayMode *mode = SDL_GetCurrentDisplayMode(SDL_GetDisplayForWindow(window));
    float hz = mode && mode->refresh_rate > 0 ? mode->refresh_rate : 60.0f;
    return std::clamp(int(std::lround(hz / 60.0f)), 1, 4);
}

// the single screen (video.layout "single"): the top screen fills the
// canvas, the bottom one a panel at its lower right corner that slides in
// (shown, 0 to 1) on a button (Tab, the right stick's click), so the game's
// own bottom screen is there to see and touch when it is wanted
void place_single(VulkanPresenter::View &view, float shown) {
    using Placement = VulkanPresenter::Placement;
    view.canvas[0] = view.top, view.canvas[1] = 240;
    Placement top;
    top.to[2] = view.top, top.to[3] = 240;
    view.placements = {top};
    if (shown <= 0) return;
    const float margin = 6, w = 320 * 0.55f, h = 240 * 0.55f;
    // eased, in from the right edge
    float t = shown * shown * (3 - 2 * shown);
    Placement panel;
    panel.screen = 1;
    panel.to[0] = view.top - margin - w + (1 - t) * (w + margin);
    panel.to[1] = 240 - margin - h;
    panel.to[2] = panel.to[0] + w;
    panel.to[3] = panel.to[1] + h;
    view.placements.push_back(panel);
}

// a scene made for both screens (a movie, a cutscene) on one: both stacked,
// as the two-screen view has them, on a canvas as wide as the window's
// shape, its sides filled with the top screen blurred and dimmed
void place_stacked(VulkanPresenter::View &view) {
    using Placement = VulkanPresenter::Placement;
    float shape = view.height ? float(view.width) / float(view.height) : 1.0f;
    float w = std::max(view.top, 480 * shape), h = 480;
    view.canvas[0] = w, view.canvas[1] = h;
    // the fill covers the canvas, the top screen's shape kept
    float fw = std::max(w, h * view.top / 240), fh = fw * 240 / view.top;
    Placement fill;
    fill.to[0] = (w - fw) / 2, fill.to[1] = (h - fh) / 2, fill.to[2] = fill.to[0] + fw, fill.to[3] = fill.to[1] + fh;
    fill.brightness = 0.4f;
    fill.blur = 6;
    Placement top;
    top.to[0] = (w - view.top) / 2, top.to[2] = top.to[0] + view.top, top.to[3] = 240;
    Placement bottom;
    bottom.screen = 1;
    bottom.to[0] = (w - 320) / 2, bottom.to[1] = 240, bottom.to[2] = bottom.to[0] + 320, bottom.to[3] = 480;
    view.placements = {fill, top, bottom};
}

// how many of the host's pixels a 3DS pixel is drawn as, each way: as many
// as fill the resolution asked for (a size, or the window's) with the
// screens shown, both stacked (480 rows) or the top one alone (240)
float resolution_scale(const Settings &s, bool both, int window_width, int window_height) {
    float width = 0, height = 0;
    if (s.video.resolution == "window") width = float(window_width), height = float(window_height);
    else if (std::sscanf(s.video.resolution.c_str(), "%fx%f", &width, &height) != 2) return 1;
    return std::clamp(std::min(width / top_width(s), height / (both ? 480.0f : 240.0f)), 1.0f, 8.0f);
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

// the command line: --config, --setup, or the game's six files as before
// the settings; the settings read from the config file
void read_settings(int argc, char **argv, Settings &settings, std::filesystem::path &config_file, bool &setup) {
    config_file = frontend::default_path();
    std::vector<std::string> positional;
    for (int i = 1; i < argc; i++) {
        if (!std::strcmp(argv[i], "--config") && i + 1 < argc) config_file = frontend::path_of(argv[++i]);
        else if (!std::strcmp(argv[i], "--setup")) setup = true;
        else positional.push_back(argv[i]);
    }
    std::string error;
    if (!frontend::load(config_file, settings, error)) {
        std::fprintf(stderr, "%s: %s (the defaults instead)\n", frontend::utf8(config_file).c_str(), error.c_str());
    }
    if (positional.size() >= 6) {
        settings.game = {positional[0], positional[1], positional[2], positional[3], positional[4], positional[5]};
    }
}

// the overlay (Dear ImGui) on the presenter's device
void start_overlay(SDL_Window *window, VulkanPresenter &presenter) {
    ImGui_ImplSDL3_InitForVulkan(window);
    VkFormat format = presenter.format();
    ImGui_ImplVulkan_InitInfo info{};
    info.Instance = presenter.instance();
    info.PhysicalDevice = presenter.physical();
    info.Device = presenter.device();
    info.QueueFamily = presenter.family();
    info.Queue = presenter.queue();
    info.DescriptorPool = presenter.overlay_pool();
    info.MinImageCount = 2;
    info.ImageCount = 3;
    info.MSAASamples = VK_SAMPLE_COUNT_1_BIT;
    info.UseDynamicRendering = true;
    info.PipelineRenderingCreateInfo = {VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO_KHR};
    info.PipelineRenderingCreateInfo.colorAttachmentCount = 1;
    info.PipelineRenderingCreateInfo.pColorAttachmentFormats = &format;
    ImGui_ImplVulkan_Init(&info);
    // its font goes up through the queue the renderer shares
    std::lock_guard g(presenter.queue_lock());
    ImGui_ImplVulkan_CreateFontsTexture();
}

// the DSP's frames made since, to the sound card: the queue kept near 60 ms
// by playing a little faster or slower (half a percent at most, not heard),
// as the DSP's clock and the sound card's drift apart; a quarter of a
// second queued at most, the rest is late
void feed_sound(SDL_AudioStream *sound, std::vector<int16_t> &out, std::atomic<uint64_t> &starved) {
    if (!sound || out.empty()) return;
    constexpr int BYTES_PER_SECOND = int(AUDIO_RATE) * 4, TARGET = BYTES_PER_SECOND * 60 / 1000;
    int queued = SDL_GetAudioStreamQueued(sound);
    // nearly out before this came: heard as a gap
    if (queued < BYTES_PER_SECOND / 200) starved++;
    float off = std::clamp(float(queued - TARGET) / TARGET, -1.0f, 1.0f);
    SDL_SetAudioStreamFrequencyRatio(sound, 1.0f + 0.005f * off);
    if (queued < BYTES_PER_SECOND / 4) SDL_PutAudioStreamData(sound, out.data(), int(out.size() * 2));
    out.clear();
}

// the input, from the keyboard and the gamepad together, and the mouse on
// the bottom screen, wherever the view last shown places it, as the stylus
Input read_input(const Bindings &bindings, SDL_Gamepad *gamepad, SDL_Window *window, const VulkanPresenter::View &shown) {
    Input now;
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
    float mx = 0, my = 0, bx = 0, by = 0;
    SDL_MouseButtonFlags mouse = SDL_GetMouseState(&mx, &my);
    // the mouse in the window's points, the view in its pixels
    float density = SDL_GetWindowPixelDensity(window);
    if ((mouse & SDL_BUTTON_LMASK) && VulkanPresenter::on_bottom(shown, mx * density, my * density, bx, by)) {
        now.touch = std::make_pair(uint16_t(std::clamp(bx, 0.0f, 319.0f)), uint16_t(std::clamp(by, 0.0f, 239.0f)));
    }
    return now;
}

// RUNTIME_DUMP=folder: the first RUNTIME_DUMP_COUNT (600) top screens shown,
// as <frame>-<part>-<game|between>.ppm, to look at what the player showed
struct Dump {
    const char *folder = SDL_getenv("RUNTIME_DUMP");
    int left = SDL_getenv("RUNTIME_DUMP_COUNT") ? std::atoi(SDL_getenv("RUNTIME_DUMP_COUNT")) : 600;

    void take(const Frame &f) {
        if (!folder || left <= 0 || f.screens[0].rgba.empty()) return;
        left--;
        char name[512];
        std::snprintf(name, sizeof name, "%s/%06llu-%d-%s.ppm", folder, (unsigned long long)f.number, f.part, f.between ? "between" : "game");
        const Picture &p = f.screens[0];
        std::ofstream out(frontend::path_of(name), std::ios::binary);
        out << "P6\n" << p.width << " " << p.height << "\n255\n";
        for (size_t i = 0; i < p.rgba.size(); i += 4) out.write(reinterpret_cast<const char *>(&p.rgba[i]), 3);
    }
};

// how each second went: for the overlay's corner, RUNTIME_STATS's line, and
// the first draw of a session asking for what the renderer does not do yet,
// noted beside the config so any session tells
class Tally {
public:
    Tally(const std::filesystem::path &config_file) : lacks_(config_file.parent_path() / "renderer-lacks.txt") {}
    frontend::Stats stats; // the last second's
    std::atomic<uint64_t> starved{0}; // sound queued at under 5 ms when more came

    void shown(std::chrono::steady_clock::time_point at) {
        counting_.worst_ms = std::max(counting_.worst_ms, std::chrono::duration<double, std::milli>(at - last_shown_).count());
        last_shown_ = at;
        counting_.shown++;
    }

    void tick(std::chrono::steady_clock::time_point at, const Frame &showing, System &system, Services &services,
              VulkanRenderer &vulkan) {
        if (at - second_ < std::chrono::seconds(1)) return;
        counting_.gpu_ms = (showing.busy - busy_before_) * 1000;
        counting_.idle_ms = (system.idle - idle_before_) * 1000;
        stats = counting_;
        static constexpr const char *LACKING[2] = {"gas", "geometry shader modes"};
        for (int i = 0; i < 2; i++) {
            if (!vulkan.stats.lacking[i] || told_[i]) continue;
            told_[i] = true;
            std::time_t now = std::time(nullptr);
            char when[32];
            std::strftime(when, sizeof when, "%Y-%m-%d %H:%M:%S", std::localtime(&now));
            std::ofstream(lacks_, std::ios::app) << when << ": " << LACKING[i] << ", frame " << showing.number << "\n";
            if (print_) std::printf("the renderer lacks %s (frame %llu)\n", LACKING[i], (unsigned long long)showing.number);
        }
        if (print_) {
            std::printf("shown %d, the game's pictures %llu, worst gap %.0f ms, gpu %.0f ms, idle %.0f ms, the host's GPU %.0f ms; "
                        "sound: %llu DSP frames, %llu let go, starved %llu times\n",
                        stats.shown, (unsigned long long)(services.pictures - pictures_before_), stats.worst_ms, stats.gpu_ms,
                        stats.idle_ms, (vulkan.stats.gpu_seconds - host_gpu_before_) * 1000,
                        (unsigned long long)(services.dsp_frames - dsp_before_),
                        (unsigned long long)(services.dsp_dropped - dropped_before_), (unsigned long long)starved.exchange(0));
            std::fflush(stdout);
        }
        dsp_before_ = services.dsp_frames;
        dropped_before_ = services.dsp_dropped;
        host_gpu_before_ = vulkan.stats.gpu_seconds;
        pictures_before_ = services.pictures;
        counting_ = {};
        second_ = at;
        busy_before_ = showing.busy;
        idle_before_ = system.idle;
    }

private:
    std::filesystem::path lacks_;
    const bool print_ = SDL_getenv("RUNTIME_STATS") != nullptr;
    frontend::Stats counting_;
    std::chrono::steady_clock::time_point second_ = std::chrono::steady_clock::now(), last_shown_ = second_;
    bool told_[2]{};
    double busy_before_ = 0, idle_before_ = 0, host_gpu_before_ = 0;
    uint64_t pictures_before_ = 0, dsp_before_ = 0, dropped_before_ = 0;
};

} // namespace

int main(int argc, char **argv) {
    Settings settings;
    std::filesystem::path config_file;
    bool setup = false;
    read_settings(argc, argv, settings, config_file, setup);
    std::string error;

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

    // the game, opened as boot opens it (session.h)
    Session session;
    const auto &g = settings.game;
    // the modules (CROs) the game has loaded, by name, for the single
    // screen's scenes; told on the game's thread
    std::mutex modules_lock;
    std::set<std::string> modules;
    auto on_module = [&](const std::string &name, uint32_t base) {
        std::lock_guard g(modules_lock);
        if (base) modules.insert(name);
        else modules.erase(name);
    };
    if (!session.open({g.exheader, g.code, g.library, g.game, g.update, g.user, g.decomp}, Config{}, error, on_module)) return fail(error);
    // the scenes, from the port's file
    frontend::Scenes scenes;
    {
        std::filesystem::path file = g.layouts.empty() ? std::filesystem::path() : frontend::path_of(g.layouts);
        if (file.empty()) {
            if (const char *base = SDL_GetBasePath()) file = frontend::path_of(base) / "single-screen.toml";
        }
        if (!file.empty() && !scenes.read(file, error)) std::fprintf(stderr, "%s\n", error.c_str());
    }
    // the window named after the game, as its icon names it
    if (!session.title.empty()) SDL_SetWindowTitle(window, session.title.c_str());
    System &system = *session.system;
    Services &handlers = *session.services;
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
    start_overlay(window, presenter);
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
        both = settings.video.layout == "both";
        if (sound) SDL_SetAudioStreamGain(sound, settings.audio.mute ? 0.0f : settings.audio.volume / 100.0f);
        vulkan.outlines = settings.video.outlines == "thin" ? 1 : settings.video.outlines == "none" ? 2 : 0;
    };
    apply();
    frontend::Overlay overlay;
    Tally tally(config_file);

    // the game on a thread of its own; this one keeps the window, reads the
    // input and shows the frames, so none of it stops the game
    std::mutex input_lock;
    Input input;
    std::atomic<bool> quit{false};
    // the top screen's shape for the port's widescreen (a game's code that
    // has port_top_aspect frames its 3D wider, Solstice's native/widescreen.c):
    // the shape the player shows it at, 0 for the 3DS's own, written into the
    // game's memory on its thread
    std::atomic<float> top_aspect{0};
    uint32_t top_aspect_at = 0;
    if (session.decomp) {
        if (auto it = session.decomp->symbols.find("port_top_aspect"); it != session.decomp->symbols.end()) top_aspect_at = it->second;
    }
    std::thread game([&] {
        system.run([&] {
            {
                std::lock_guard g(input_lock);
                handlers.input = input;
            }
            if (top_aspect_at) system.memory.write32(top_aspect_at, std::bit_cast<uint32_t>(top_aspect.load()));
            handlers.tick(system);
            feed_sound(sound, handlers.audio.output, tally.starved);
            return quit.load(std::memory_order_relaxed);
        });
        quit = true;
    });

    Dump dump;
    const frontend::Scene *scene_shown = nullptr;
    // the view last shown, for the mouse; the single screen's panel
    VulkanPresenter::View shown_view;
    // RUNTIME_PANEL=1: open from the start (to look at it without a key)
    bool panel_wanted = SDL_getenv("RUNTIME_PANEL") != nullptr;
    float panel = 0;
    auto panel_at = std::chrono::steady_clock::now();
    float scale_wanted = 1;
    auto scale_since = std::chrono::steady_clock::now();
    std::deque<Frame> waiting;
    Frame showing;
    auto shown_at = std::chrono::steady_clock::now();
    while (!quit) {
        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            if (event.type == SDL_EVENT_QUIT) quit = true;
            if (event.type == SDL_EVENT_GAMEPAD_ADDED && !gamepad) gamepad = SDL_OpenGamepad(event.gdevice.which);
            if (event.type == SDL_EVENT_KEY_DOWN && event.key.scancode == SDL_SCANCODE_F11 && !event.key.repeat) {
                settings.video.fullscreen = !settings.video.fullscreen;
                SDL_SetWindowFullscreen(window, settings.video.fullscreen);
            }
            if ((event.type == SDL_EVENT_KEY_DOWN && event.key.scancode == SDL_SCANCODE_TAB && !event.key.repeat) ||
                (event.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN && event.gbutton.button == SDL_GAMEPAD_BUTTON_RIGHT_STICK)) {
                panel_wanted = !panel_wanted;
            }
            overlay.event(event);
        }
        // none while the overlay is open
        Input now = overlay.open ? Input{} : read_input(bindings, gamepad, window, shown_view);
        {
            std::lock_guard g(input_lock);
            input = now;
        }

        // the frames the GPU finished, a vertical blank's pictures each
        // blank, shown in order; two blanks' waiting at most (a screen
        // refreshing slower than the game's 59.83 Hz), the older ones skipped
        const int parts = between_parts(settings, window);
        vulkan.between = parts;
        for (Frame f; handlers.gpu->next_frame(f);) waiting.push_back(std::move(f));
        while (waiting.size() > 2 * size_t(std::max(parts, 1))) waiting.pop_front();
        // without the display's refreshes to wait for, a blank's later
        // pictures each a part of a blank after the one before
        if (!waiting.empty() && waiting.front().part > 0 && !settings.video.vsync) {
            auto due = shown_at + std::chrono::duration<double>(1.0 / (59.83 * std::max(parts, 1)));
            if (std::chrono::steady_clock::now() < due) {
                SDL_Delay(0);
                continue;
            }
        }
        bool fresh = !waiting.empty();
        if (fresh) {
            showing = std::move(waiting.front());
            waiting.pop_front();
            dump.take(showing);
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
        overlay.draw(settings, tally.stats, config_file, changed);
        ImGui::Render();
        int pw = 0, ph = 0;
        SDL_GetWindowSizeInPixels(window, &pw, &ph);
        VulkanPresenter::View view;
        view.width = uint32_t(pw), view.height = uint32_t(ph);
        view.both = both;
        view.smooth = settings.video.smooth;
        view.top = top_width(settings);
        top_aspect = view.top > 400 ? view.top / 240 : 0;
        vulkan.top_wide = view.top / 400;
        view.vsync = settings.video.vsync;
        view.antialias = settings.video.antialias;
        view.sharpen = settings.video.sharpen;
        // the single screen's panel, sliding in or out in a tenth of a second
        {
            auto at = std::chrono::steady_clock::now();
            float step = std::chrono::duration<float>(at - panel_at).count() / 0.12f;
            panel_at = at;
            panel = std::clamp(panel + (panel_wanted ? step : -step), 0.0f, 1.0f);
        }
        if (settings.video.layout == "single") {
            const frontend::Scene *scene = nullptr;
            {
                std::lock_guard g(modules_lock);
                scene = scenes.pick(modules);
            }
            const std::string layout = scene ? scene->layout : "panel";
            if (scene != scene_shown) {
                scene_shown = scene;
                std::fprintf(stderr, "scene: %s (%s)\n", scene ? scene->name.c_str() : "none", layout.c_str());
            }
            if (layout == "stacked") place_stacked(view);
            else place_single(view, layout == "top" ? 0.0f : panel);
        }
        VulkanPresenter::place_screens(view);
        shown_view = view;
        // the resolution, once it held a moment (a window being resized
        // would make the buffers again at every step)
        if (float want = resolution_scale(settings, both, pw, ph); std::abs(want - scale_wanted) > 0.01f) {
            scale_wanted = want;
            scale_since = std::chrono::steady_clock::now();
        } else if (std::chrono::steady_clock::now() - scale_since > std::chrono::milliseconds(500)) {
            vulkan.scale = scale_wanted;
        }
        presenter.present(fresh ? &showing : nullptr, view,
                          [](VkCommandBuffer cmd) { ImGui_ImplVulkan_RenderDrawData(ImGui::GetDrawData(), cmd); });
        if (changed) apply();

        auto done = std::chrono::steady_clock::now();
        if (fresh) tally.shown(done), shown_at = done;
        tally.tick(done, showing, system, handlers, vulkan);
    }
    game.join();
    if (!system.error().empty()) std::fprintf(stderr, "stopped: %s\n", system.error().c_str());
    // where the C had nothing, for the next recompile to take in
    if (save_hints(system.interpreted_at, session.exheader, error) < 0) std::fprintf(stderr, "hints: %s\n", error.c_str());
    frontend::save(config_file, settings, error);
    presenter.idle();
    ImGui_ImplVulkan_Shutdown();
    ImGui_ImplSDL3_Shutdown();
    ImGui::DestroyContext();
    SDL_Quit();
    return 0;
}
