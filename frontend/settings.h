// The player's settings, kept in config.toml. The launcher edits what takes
// a restart (the game's files), the overlay what changes live;
// both save the same file.
#ifndef FRONTEND_SETTINGS_H
#define FRONTEND_SETTINGS_H

#include <cstdint>
#include <filesystem>
#include <map>
#include <string>

namespace frontend {

// the 3DS's buttons, in the order the settings list them, and their bits
struct Button {
    const char *name;
    uint32_t bit; // 0 for the circle pad's directions
};
constexpr Button BUTTONS[] = {
    {"a", 1},           {"b", 2},           {"x", 1024},      {"y", 2048},        {"l", 512},
    {"r", 256},         {"start", 8},       {"select", 4},    {"up", 64},         {"down", 128},
    {"left", 32},       {"right", 16},      {"circle_up", 0}, {"circle_down", 0}, {"circle_left", 0},
    {"circle_right", 0},
};

struct Settings {
    // the game: what the recompiled port needs, and where saves go; decomp,
    // a folder with the decomp's decomp.bin and decomp.txt, or none
    struct {
        std::string exheader, code, library, game, update, user, decomp;
    } game;
    struct {
        bool fullscreen = false;
        bool vsync = false;      // waits for the display, which the game, on the same thread, pays for
        bool smooth = true;      // the screens scaled up smoothly, or in sharp pixels
        bool between = false;    // a frame drawn between each two of the game's (30 Hz scenes shown at 60)
        std::string layout = "both"; // both screens stacked, or "top" alone
    } video;
    struct {
        int volume = 100; // percent
        bool mute = false;
    } audio;
    // each button's key (an SDL key name) and gamepad button (an SDL gamepad
    // button name)
    std::map<std::string, std::string> keys, pad;
    bool stats = false; // the overlay's corner with the frame times

    Settings(); // the default bindings

    bool game_ready() const;
};

// a path as UTF-8, as SDL and the settings file take them
inline std::string utf8(const std::filesystem::path &path) {
    auto s = path.u8string();
    return {s.begin(), s.end()};
}
inline std::filesystem::path path_of(const std::string &utf8) {
    return std::filesystem::path(std::u8string(utf8.begin(), utf8.end()));
}

// %APPDATA%\runtime\config.toml and its kin elsewhere
std::filesystem::path default_path();

// a missing file gives the defaults; false and error for one that does not parse
bool load(const std::filesystem::path &path, Settings &settings, std::string &error);
bool save(const std::filesystem::path &path, const Settings &settings, std::string &error);

} // namespace frontend

#endif
