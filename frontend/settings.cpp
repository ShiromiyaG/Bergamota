#include "settings.h"

#include <SDL3/SDL.h>
#include <toml++/toml.hpp>

#include <algorithm>
#include <fstream>

namespace frontend {

Settings::Settings() {
    keys = {
        {"a", "X"},       {"b", "Z"},           {"x", "S"},      {"y", "A"},          {"l", "Q"},
        {"r", "W"},       {"start", "Return"},  {"select", "Backspace"}, {"up", "Up"}, {"down", "Down"},
        {"left", "Left"}, {"right", "Right"},   {"circle_up", "I"}, {"circle_down", "K"}, {"circle_left", "J"},
        {"circle_right", "L"},
    };
    // by position: the 3DS's A is on the right, B at the bottom
    pad = {
        {"a", "b"},          {"b", "a"},           {"x", "y"},           {"y", "x"},
        {"l", "leftshoulder"}, {"r", "rightshoulder"}, {"start", "start"}, {"select", "back"},
        {"up", "dpup"},      {"down", "dpdown"},   {"left", "dpleft"},   {"right", "dpright"},
    };
}

bool Settings::game_ready() const {
    namespace fs = std::filesystem;
    // the game and a saves folder; the update, exheader, code and library
    // only when named (the program comes from the dumps, the library from
    // where 3dsrecomp installs it)
    if (game.game.empty() || !fs::exists(path_of(game.game)) || game.user.empty()) return false;
    for (const std::string *p : {&game.update, &game.exheader, &game.code, &game.library}) {
        if (!p->empty() && !fs::exists(path_of(*p))) return false;
    }
    return true;
}

std::filesystem::path default_path() {
    std::filesystem::path dir;
    if (char *pref = SDL_GetPrefPath(nullptr, "runtime")) {
        dir = path_of(pref);
        SDL_free(pref);
    }
    return dir / "config.toml";
}

namespace {

template <typename T> void get(const toml::table &t, const char *section, const char *key, T &into) {
    if (auto v = t[section][key].value<T>()) into = *v;
}

void get_map(const toml::table &t, const char *section, std::map<std::string, std::string> &into) {
    if (auto *table = t[section].as_table()) {
        for (auto &[key, value] : *table) {
            if (auto s = value.value<std::string>()) into[std::string(key.str())] = *s;
        }
    }
}

} // namespace

bool load(const std::filesystem::path &path, Settings &s, std::string &error) {
    if (!std::filesystem::exists(path)) return true;
    toml::table t;
    try {
        t = toml::parse_file(utf8(path));
    } catch (const toml::parse_error &e) {
        error = std::string(e.description());
        return false;
    }
    get(t, "game", "exheader", s.game.exheader);
    get(t, "game", "code", s.game.code);
    get(t, "game", "library", s.game.library);
    get(t, "game", "game", s.game.game);
    get(t, "game", "update", s.game.update);
    get(t, "game", "user", s.game.user);
    get(t, "game", "decomp", s.game.decomp);
    get(t, "game", "layouts", s.game.layouts);
    int64_t number;
    get(t, "video", "fullscreen", s.video.fullscreen);
    get(t, "video", "vsync", s.video.vsync);
    get(t, "video", "smooth", s.video.smooth);
    get(t, "video", "between", s.video.between);
    get(t, "video", "between_display", s.video.between_display);
    get(t, "video", "between_still", s.video.between_still);
    get(t, "video", "layout", s.video.layout);
    double panel_size = s.video.panel_size;
    get(t, "video", "panel_size", panel_size);
    s.video.panel_size = float(std::clamp(panel_size, 0.25, 1.0));
    get(t, "video", "panel_corner", s.video.panel_corner);
    get(t, "video", "no_ghosting", s.video.no_ghosting);
    get(t, "video", "aspect", s.video.aspect);
    get(t, "video", "resolution", s.video.resolution);
    get(t, "video", "antialias", s.video.antialias);
    get(t, "video", "outlines", s.video.outlines);
    double sharpen = s.video.sharpen;
    get(t, "video", "sharpen", sharpen);
    s.video.sharpen = float(std::clamp(sharpen, 0.0, 1.0));
    number = s.audio.volume;
    get(t, "audio", "volume", number);
    s.audio.volume = int(std::clamp<int64_t>(number, 0, 100));
    get(t, "audio", "mute", s.audio.mute);
    get_map(t, "keyboard", s.keys);
    get_map(t, "gamepad", s.pad);
    get(t, "overlay", "stats", s.stats);
    return true;
}

bool save(const std::filesystem::path &path, const Settings &s, std::string &error) {
    toml::table keys, pad;
    for (auto &[k, v] : s.keys) keys.insert(k, v);
    for (auto &[k, v] : s.pad) pad.insert(k, v);
    toml::table t{
        {"game", toml::table{{"exheader", s.game.exheader},
                             {"code", s.game.code},
                             {"library", s.game.library},
                             {"game", s.game.game},
                             {"update", s.game.update},
                             {"user", s.game.user},
                             {"decomp", s.game.decomp},
                             {"layouts", s.game.layouts}}},
        {"video", toml::table{{"fullscreen", s.video.fullscreen},
                              {"vsync", s.video.vsync},
                              {"smooth", s.video.smooth},
                              {"between", s.video.between},
                              {"between_display", s.video.between_display},
                              {"between_still", s.video.between_still},
                              {"layout", s.video.layout},
                              {"panel_size", double(s.video.panel_size)},
                              {"panel_corner", s.video.panel_corner},
                              {"no_ghosting", s.video.no_ghosting},
                              {"aspect", s.video.aspect},
                              {"resolution", s.video.resolution},
                              {"antialias", s.video.antialias},
                              {"outlines", s.video.outlines},
                              {"sharpen", double(s.video.sharpen)}}},
        {"audio", toml::table{{"volume", s.audio.volume}, {"mute", s.audio.mute}}},
        {"keyboard", keys},
        {"gamepad", pad},
        {"overlay", toml::table{{"stats", s.stats}}},
    };
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    std::ofstream out(path);
    if (!out) {
        error = "cannot write " + utf8(path);
        return false;
    }
    out << t << "\n";
    return true;
}

} // namespace frontend
