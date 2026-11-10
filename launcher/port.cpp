#include "port.h"

#include <SDL3/SDL.h>
#include <toml++/toml.hpp>

#include <algorithm>
#include <cstdio>
#include <fstream>
#include <sstream>

namespace launcher {

namespace {
std::vector<std::string> arguments;
std::string program;
} // namespace

std::optional<uint64_t> GameInfo::program_id() const {
    std::string digits = title.rfind("0x", 0) == 0 ? title.substr(2) : title;
    if (digits.empty() || digits.size() > 16 || !std::all_of(digits.begin(), digits.end(), [](char c) { return std::isxdigit(uint8_t(c)); }))
        return std::nullopt;
    return std::stoull(digits, nullptr, 16);
}

const GameInfo *Port::game(uint64_t program_id) const {
    for (const GameInfo &g : games)
        if (g.program_id() == program_id) return &g;
    return nullptr;
}

const GameInfo *Port::game(const std::string &title) const {
    for (const GameInfo &g : games)
        if (upper(g.title) == upper(title)) return &g;
    return nullptr;
}

std::string Port::names() const {
    std::vector<std::string> names;
    for (const GameInfo &g : games) names.push_back(g.name + " (" + g.title + ")");
    std::string out;
    for (size_t i = 0; i < names.size(); i++) {
        if (i) out += i + 1 == names.size() ? " or " : ", ";
        out += names[i];
    }
    return out;
}

bool parse_port(const std::string &text, Port &port, std::string &error) {
    toml::table t;
    try {
        t = toml::parse(text);
    } catch (const toml::parse_error &e) {
        error = std::string(e.description());
        return false;
    }
    port = Port{};
    port.name = t["name"].value_or(std::string());
    port.player = t["player"].value_or(std::string());
    port.about = t["about"].value_or(std::string());
    if (port.name.empty() || port.player.empty()) {
        error = "it names no port (name) or no player (player)";
        return false;
    }
    if (const toml::array *games = t["game"].as_array()) {
        for (const toml::node &node : *games) {
            const toml::table *g = node.as_table();
            if (!g) continue;
            GameInfo info;
            info.title = (*g)["title"].value_or(std::string());
            info.name = (*g)["name"].value_or(std::string());
            if (auto update = (*g)["update"].value<int64_t>()) info.update = uint16_t(*update);
            info.update_name = (*g)["update_name"].value_or(std::string());
            info.decomp = (*g)["decomp"].value_or(std::string());
            info.color = (*g)["color"].value_or(std::string());
            if (!info.program_id()) {
                error = "the title id " + info.title + " is not hexadecimal";
                return false;
            }
            port.games.push_back(std::move(info));
        }
    }
    return true;
}

void set_args(int argc, char **argv) {
    program = argc > 0 ? argv[0] : "";
    arguments.assign(argv + std::min(argc, 1), argv + argc);
}

const std::vector<std::string> &args() { return arguments; }

bool has_arg(const std::string &name) { return std::find(arguments.begin(), arguments.end(), name) != arguments.end(); }

std::optional<std::string> arg_after(const std::string &name) {
    auto at = std::find(arguments.begin(), arguments.end(), name);
    if (at == arguments.end() || at + 1 == arguments.end() || (at + 1)->rfind("--", 0) == 0) return std::nullopt;
    return *(at + 1);
}

std::string utf8(const fs::path &path) {
    auto s = path.u8string();
    return {s.begin(), s.end()};
}

fs::path path_of(const std::string &utf8) { return fs::path(std::u8string(utf8.begin(), utf8.end())); }

namespace {
fs::path given_base;
} // namespace

void set_base(const fs::path &dir) { given_base = dir; }

fs::path exe() {
    if (!given_base.empty()) return given_base / "launcher";
    const char *base = SDL_GetBasePath();
    fs::path name = path_of(program).filename();
    if (name.empty()) name = "launcher";
    if (!name.has_extension() && SDL_strcmp(SDL_GetPlatform(), "Windows") == 0) name += ".exe";
    return base ? path_of(base) / name : name;
}

fs::path beside(const std::string &name) {
    if (!given_base.empty()) return given_base / path_of(name);
    const char *base = SDL_GetBasePath();
    return base ? path_of(base) / path_of(name) : path_of(name);
}

namespace {
fs::path port_file() {
    if (auto named = arg_after("--port")) return path_of(*named);
    return beside("port.toml");
}
} // namespace

fs::path folder(const std::string &name) {
    fs::path dir = port_file().parent_path();
    return (dir.empty() ? beside("") : dir) / path_of(name);
}

const Port *get_port(std::string *error) {
    static Port port;
    static std::string why;
    static bool read = false, ok = false;
    if (!read) {
        read = true;
        fs::path file = port_file();
        std::ifstream in(file, std::ios::binary);
        if (!in) {
            why = utf8(file) + " could not be read";
        } else {
            std::stringstream text;
            text << in.rdbuf();
            ok = parse_port(text.str(), port, why);
            if (!ok) why = utf8(file) + ": " + why;
        }
    }
    if (!ok && error) *error = why;
    return ok ? &port : nullptr;
}

const Port &port() { return *get_port(); }

std::string title_of(uint64_t program_id) {
    char text[17];
    std::snprintf(text, sizeof text, "%016llX", static_cast<unsigned long long>(program_id));
    return text;
}

std::string upper(std::string text) {
    for (char &c : text) c = char(std::toupper(uint8_t(c)));
    return text;
}

} // namespace launcher
