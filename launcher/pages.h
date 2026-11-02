// The player's settings from the launcher: player.toml in the port's
// folder, read and written with the player's own code (frontend/settings),
// so its defaults are the launcher's; the same file its F1 menu changes.
// The Graphics and Controls pages save as soon as something changes.
#ifndef LAUNCHER_PAGES_H
#define LAUNCHER_PAGES_H

#include "../frontend/settings.h"

#include <imgui.h>

#include <string>

union SDL_Event;

namespace launcher {

class PlayerPages {
public:
    void load();
    // a short line of what the game will look like, for the home page
    std::string summary() const;
    bool record() const { return settings_.game.record; }
    void set_record(bool on);

    void graphics(ImU32 accent);
    void controls(ImU32 accent);
    // a key or gamepad button being chosen takes the event; true when it did
    bool event(const SDL_Event &event);

private:
    void save();
    frontend::Settings settings_;
    // the 3DS button whose key or gamepad button is being chosen
    std::string capture_;
    bool pad_ = false;
    std::string note_;
};

} // namespace launcher

#endif
