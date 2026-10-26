// A game on the runtime, in a window: both screens for now (the ports put
// their own layout on top, Solstice its single screen), the keyboard, a
// gamepad, and the mouse on the bottom screen as the stylus.
//
//   player <exheader.bin> <code.bin> <recompiled library> <game .3ds> <update .cia> <user folder>
//
// Keys: arrows the D-pad, X/Z A/B, S/A X/Y, Q/W L/R, Enter Start,
// Backspace Select, IJKL the circle pad. A gamepad by position (the
// bottom face button is B, as on the 3DS).

#include "runtime/fs.h"
#include "runtime/loader.h"
#include "runtime/services.h"
#include "runtime/system.h"

#include <SDL3/SDL.h>

#include <chrono>
#include <cstdio>
#include <fstream>
#include <iterator>

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

// the 3DS's button bits
enum : uint32_t {
    A = 1, B = 2, SELECT = 4, START = 8, RIGHT = 16, LEFT = 32, UP = 64, DOWN = 128, R = 256, L = 512, X = 1024,
    Y = 2048,
};

struct Key {
    SDL_Scancode key;
    uint32_t button;
};
constexpr Key KEYS[] = {
    {SDL_SCANCODE_X, A}, {SDL_SCANCODE_Z, B}, {SDL_SCANCODE_S, X}, {SDL_SCANCODE_A, Y},
    {SDL_SCANCODE_Q, L}, {SDL_SCANCODE_W, R}, {SDL_SCANCODE_RETURN, START}, {SDL_SCANCODE_BACKSPACE, SELECT},
    {SDL_SCANCODE_UP, UP}, {SDL_SCANCODE_DOWN, DOWN}, {SDL_SCANCODE_LEFT, LEFT}, {SDL_SCANCODE_RIGHT, RIGHT},
};

struct Pad {
    SDL_GamepadButton button;
    uint32_t bit;
};
// by position: the 3DS's A is on the right, B at the bottom
constexpr Pad PADS[] = {
    {SDL_GAMEPAD_BUTTON_EAST, A}, {SDL_GAMEPAD_BUTTON_SOUTH, B}, {SDL_GAMEPAD_BUTTON_NORTH, X},
    {SDL_GAMEPAD_BUTTON_WEST, Y}, {SDL_GAMEPAD_BUTTON_LEFT_SHOULDER, L}, {SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER, R},
    {SDL_GAMEPAD_BUTTON_START, START}, {SDL_GAMEPAD_BUTTON_BACK, SELECT}, {SDL_GAMEPAD_BUTTON_DPAD_UP, UP},
    {SDL_GAMEPAD_BUTTON_DPAD_DOWN, DOWN}, {SDL_GAMEPAD_BUTTON_DPAD_LEFT, LEFT}, {SDL_GAMEPAD_BUTTON_DPAD_RIGHT, RIGHT},
};

} // namespace

int main(int argc, char **argv) {
    if (argc < 7) {
        std::fprintf(stderr, "player <exheader.bin> <code.bin> <library> <game .3ds> <update .cia> <user folder>\n");
        return 2;
    }
    ExHeader exheader;
    std::string error;
    if (!parse_exheader(read_file(argv[1]), exheader, error) || !library.open(argv[3], error)) {
        std::fprintf(stderr, "%s\n", error.c_str());
        return 1;
    }
    Config config;
    config.app_bytes = exheader.app_bytes;
    config.region = exheader.region;
    Services handlers(exheader.program_id);
    services_ptr = &handlers;
    Fs fs(argv[6], exheader.program_id);
    fs.romfs = find_romfs(argv[4], error);
    fs.update_romfs = find_romfs(argv[5], error);
    if (!fs.romfs) {
        std::fprintf(stderr, "%s\n", error.c_str());
        return 1;
    }
    handlers.fs = &fs;
    handlers.on_module = [](const std::string &name, uint32_t base) { library.place(name, base); };
    System system(config, lookup, services);
    load(system, exheader, read_file(argv[2]));
    ZakuroGpu gpu(system.memory);
    handlers.gpu = &gpu;

    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD)) {
        std::fprintf(stderr, "SDL: %s\n", SDL_GetError());
        return 1;
    }
    constexpr int SCALE = 2;
    SDL_Window *window = SDL_CreateWindow("runtime", 400 * SCALE, 480 * SCALE, SDL_WINDOW_RESIZABLE);
    SDL_Renderer *renderer = SDL_CreateRenderer(window, nullptr);
    SDL_SetRenderVSync(renderer, 0);
    SDL_SetRenderLogicalPresentation(renderer, 400, 480, SDL_LOGICAL_PRESENTATION_LETTERBOX);
    SDL_Texture *screens[2] = {
        SDL_CreateTexture(renderer, SDL_PIXELFORMAT_ABGR8888, SDL_TEXTUREACCESS_STREAMING, 400, 240),
        SDL_CreateTexture(renderer, SDL_PIXELFORMAT_ABGR8888, SDL_TEXTUREACCESS_STREAMING, 320, 240),
    };
    SDL_Gamepad *gamepad = nullptr;

    bool quit = false;
    auto next_frame = std::chrono::steady_clock::now();
    system.run([&] {
        handlers.tick(system);
        auto now = std::chrono::steady_clock::now();
        if (now < next_frame) return quit;
        next_frame = now + std::chrono::microseconds(16667);

        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            if (event.type == SDL_EVENT_QUIT) quit = true;
            if (event.type == SDL_EVENT_GAMEPAD_ADDED && !gamepad) gamepad = SDL_OpenGamepad(event.gdevice.which);
        }
        // the input, from the keyboard and the gamepad together
        const bool *keys = SDL_GetKeyboardState(nullptr);
        uint32_t buttons = 0;
        for (const Key &k : KEYS) buttons |= keys[k.key] ? k.button : 0;
        float cx = float(keys[SDL_SCANCODE_L]) - float(keys[SDL_SCANCODE_J]);
        float cy = float(keys[SDL_SCANCODE_I]) - float(keys[SDL_SCANCODE_K]);
        if (gamepad) {
            for (const Pad &p : PADS) buttons |= SDL_GetGamepadButton(gamepad, p.button) ? p.bit : 0;
            float gx = SDL_GetGamepadAxis(gamepad, SDL_GAMEPAD_AXIS_LEFTX) / 32767.0f;
            float gy = -SDL_GetGamepadAxis(gamepad, SDL_GAMEPAD_AXIS_LEFTY) / 32767.0f;
            if (std::abs(gx) > std::abs(cx)) cx = gx;
            if (std::abs(gy) > std::abs(cy)) cy = gy;
        }
        handlers.input.buttons = buttons;
        handlers.input.circle_x = cx;
        handlers.input.circle_y = cy;
        // the mouse on the bottom screen is the stylus
        float mx, my;
        SDL_MouseButtonFlags mouse = SDL_GetMouseState(&mx, &my);
        SDL_RenderCoordinatesFromWindow(renderer, mx, my, &mx, &my);
        handlers.input.touch.reset();
        if ((mouse & SDL_BUTTON_LMASK) && my >= 240 && my < 480 && mx >= 40 && mx < 360) {
            handlers.input.touch = std::make_pair(uint16_t(mx - 40), uint16_t(my - 240));
        }

        // the screens
        SDL_SetRenderDrawColor(renderer, 0, 0, 0, 255);
        SDL_RenderClear(renderer);
        for (uint32_t s = 0; s < 2; s++) {
            Picture p = gpu.screen(s);
            SDL_UpdateTexture(screens[s], nullptr, p.rgba.data(), int(p.width * 4));
            SDL_FRect at{s ? 40.0f : 0.0f, s ? 240.0f : 0.0f, float(p.width), 240.0f};
            SDL_RenderTexture(renderer, screens[s], nullptr, &at);
        }
        SDL_RenderPresent(renderer);
        return quit;
    });
    if (!system.error().empty()) std::fprintf(stderr, "stopped: %s\n", system.error().c_str());
    SDL_Quit();
    return 0;
}
