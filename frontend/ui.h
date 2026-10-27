// The launcher, before the game, and the overlay, over it (F1), both drawn
// with Dear ImGui: on the player's SDL renderer, or the overlay on the
// runtime's Vulkan renderer when that shows the screens itself.
#ifndef FRONTEND_UI_H
#define FRONTEND_UI_H

#include "settings.h"

#include "runtime/vulkan_renderer.h"

#include <SDL3/SDL.h>

#include <filesystem>
#include <string>

namespace frontend {

// Dear ImGui on the window, for both, drawn with the SDL renderer
void ui_init(SDL_Window *window, SDL_Renderer *renderer);
void ui_shutdown();
// what it draws with from now on: the SDL renderer, or Vulkan (false when
// its backend could not start); the context and its settings stay
void ui_on_renderer(SDL_Window *window, SDL_Renderer *renderer);
bool ui_on_vulkan(SDL_Window *window, const runtime::VulkanHandles &vk);
void ui_drop_backend();
// a frame of it begins, on whichever; and drawn into Vulkan's commands
void ui_new_frame();
void ui_draw_vulkan(VkCommandBuffer cmd);

// asks for the game's files and what needs a restart; true to start the
// game, false when the window closed
bool launcher(SDL_Window *window, SDL_Renderer *renderer, Settings &settings, const std::filesystem::path &file);

// how the last second went, for the overlay's corner
struct Stats {
    int shown = 0;        // pictures shown
    double worst_ms = 0;  // the longest gap between two
    double gpu_ms = 0;    // drawing, of the second
    double idle_ms = 0;   // every thread waiting
};

class Overlay {
public:
    // true when the overlay took the event (the game should not see it)
    bool event(const SDL_Event &event);
    // draws it; changed is set when a setting the player applies live moved
    void draw(Settings &settings, const Stats &stats, const std::filesystem::path &file, bool &changed);
    bool open = false;
    // the overlay wants the keyboard (the game gets none)
    bool keyboard() const;

private:
    // a button waiting for its new key (or gamepad button), and which
    const char *binding_ = nullptr;
    bool binding_pad_ = false;
    std::string pending_; // what it got (empty: cancelled)
    bool done_ = false;
    bool dirty_ = false; // saved when it closes
    std::string message_;
};

} // namespace frontend

#endif
