// The launcher, before the game, and the overlay, over it (F1), both drawn
// with Dear ImGui on the player's SDL renderer.
#ifndef FRONTEND_UI_H
#define FRONTEND_UI_H

#include "settings.h"

#include <SDL3/SDL.h>

#include <filesystem>
#include <string>
#include <vector>

#include "runtime/decomp_options.h"

namespace frontend {

// Dear ImGui on the window, for both
void ui_init(SDL_Window *window, SDL_Renderer *renderer);
void ui_shutdown();

// an option's value as set, else the code's default, within its range
int32_t option_value(const Settings &settings, const runtime::DecompOption &option);
// a row for each option, as a checkbox or a slider; true when one moved
bool option_rows(Settings &settings, const std::vector<runtime::DecompOption> &options);

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
    // the single screen's scene and its layout, shown in the Video tab
    std::string scene;
    // the port's options (its code's PORT_OPTIONs), in a tab of their own
    const std::vector<runtime::DecompOption> *options = nullptr;
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
