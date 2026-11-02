#include "library.h"

#include "runtime/dump.h"

#include <SDL3/SDL.h>
#include <toml++/toml.hpp>

#include <algorithm>
#include <fstream>
#include <sstream>

namespace launcher {

namespace {

std::optional<toml::table> read_toml(const fs::path &path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return std::nullopt;
    std::stringstream text;
    text << in.rdbuf();
    try {
        return toml::parse(text.str());
    } catch (const toml::parse_error &) {
        return std::nullopt;
    }
}

bool write_toml(const fs::path &path, const toml::table &table, std::string &error) {
    std::error_code ec;
    fs::create_directories(path.parent_path(), ec);
    std::ofstream out(path, std::ios::binary);
    out << table << "\n";
    if (!out) error = "could not write " + utf8(path);
    return bool(out);
}

template <typename View> fs::path path_value(View node) { return path_of(node.value_or(std::string())); }

Config config_of(const toml::table &t) {
    Config c;
    c.title = t["title"].value_or(std::string());
    c.game = path_value(t["game"]);
    c.update = path_value(t["update"]);
    c.built = t["built"].value_or(false);
    c.full = t["full"].value_or(false);
    c.decomp = path_value(t["decomp"]);
    // kept as TOML's signed integer, its bits the hash's
    if (auto hash = t["decomp_built"].value<int64_t>()) c.decomp_built = uint64_t(*hash);
    return c;
}

toml::table table_of(const Config &c) {
    toml::table t{{"title", c.title}, {"built", c.built}, {"full", c.full}};
    if (!c.game.empty()) t.insert("game", utf8(c.game));
    if (!c.update.empty()) t.insert("update", utf8(c.update));
    if (!c.decomp.empty()) t.insert("decomp", utf8(c.decomp));
    if (c.decomp_built) t.insert("decomp_built", int64_t(*c.decomp_built));
    return t;
}

fs::path config_toml() {
    fs::path dir = data_dir();
    return dir.empty() ? fs::path() : dir / "config.toml";
}

} // namespace

bool Config::ready() const {
    std::error_code ec;
    fs::path dir = decomp_dir();
    return built && fs::is_regular_file(game, ec) && (update.empty() || fs::is_regular_file(update, ec)) &&
           decomp_built == (dir.empty() ? std::nullopt : fingerprint(dir));
}

fs::path Config::decomp_dir() const {
    fs::path dir = decomp;
    if (dir.empty()) {
        const Port *p = get_port();
        const GameInfo *info = p ? p->game(title) : nullptr;
        if (info && !info->decomp.empty()) dir = folder(info->decomp);
    }
    std::error_code ec;
    bool built = !dir.empty() && fs::is_regular_file(dir / "decomp.txt", ec) && fs::is_regular_file(dir / "decomp.bin", ec);
    return built ? dir : fs::path();
}

Library Library::load() {
    Library library;
    fs::path path = config_toml();
    auto t = path.empty() ? std::nullopt : read_toml(path);
    if (!t) return library;
    library.last = (*t)["last"].value_or(std::string());
    library.recomp = path_value((*t)["recomp"]);
    library.player = path_value((*t)["player"]);
    library.fast = (*t)["fast"].value_or(false);
    if (const toml::table *games = (*t)["games"].as_table()) {
        for (auto &&[key, node] : *games)
            if (const toml::table *g = node.as_table()) library.games[upper(std::string(key.str()))] = config_of(*g);
    }
    // a launcher from before the port's file: its one game at the top,
    // moved into the game's own folder
    if (library.games.empty() && library.last.empty() && (*t)["game"].is_string()) {
        Config c = config_of(*t);
        runtime::DumpInfo info;
        std::string detail;
        if (runtime::read_dump(c.game, info, detail) == runtime::DumpProblem::None) {
            c.title = title_of(info.title_id);
            for (const char *name : {"build", "update"}) {
                fs::path old = data_dir() / name, moved = game_dir(c.title) / name;
                std::error_code ec;
                if (fs::is_directory(old, ec) && !fs::exists(moved, ec)) {
                    fs::create_directories(moved.parent_path(), ec);
                    fs::rename(old, moved, ec);
                }
            }
            library.games[c.title] = c;
            library.last = c.title;
        }
    }
    return library;
}

bool Library::save(std::string &error) const {
    fs::path path = config_toml();
    if (path.empty()) {
        error = "there is no APPDATA folder to keep the settings in";
        return false;
    }
    toml::table t{{"fast", fast}};
    if (!last.empty()) t.insert("last", last);
    if (!recomp.empty()) t.insert("recomp", utf8(recomp));
    if (!player.empty()) t.insert("player", utf8(player));
    toml::table games_table;
    for (const auto &[title, config] : games) games_table.insert(title, table_of(config));
    t.insert("games", std::move(games_table));
    return write_toml(path, t, error);
}

Config Library::current() const {
    if (auto it = games.find(last); it != games.end()) return it->second;
    return games.empty() ? Config{} : games.begin()->second;
}

void Library::put(const Config &config) {
    if (config.title.empty()) return;
    last = config.title;
    games[config.title] = config;
}

std::vector<Config> Library::ready() const {
    std::vector<Config> out;
    for (const auto &[title, config] : games)
        if (config.ready()) out.push_back(config);
    const Port *p = get_port();
    auto order = [&](const Config &c) {
        if (!p) return p ? size_t(0) : SIZE_MAX;
        for (size_t i = 0; i < p->games.size(); i++)
            if (upper(p->games[i].title) == c.title) return i;
        return SIZE_MAX;
    };
    std::stable_sort(out.begin(), out.end(), [&](const Config &a, const Config &b) { return order(a) < order(b); });
    return out;
}

std::optional<uint64_t> fingerprint(const fs::path &dir) {
    uint64_t hash = 0xCBF29CE484222325ull;
    for (const char *name : {"decomp.txt", "decomp.bin"}) {
        std::ifstream in(dir / name, std::ios::binary);
        if (!in) return std::nullopt;
        std::vector<char> bytes((std::istreambuf_iterator<char>(in)), {});
        for (char byte : bytes) hash = (hash ^ uint8_t(byte)) * 0x00000100000001B3ull;
    }
    return hash;
}

fs::path data_dir() {
    const Port *p = get_port();
    if (!p) return {};
    const char *appdata = SDL_GetEnvironmentVariable(SDL_GetEnvironment(), "APPDATA");
    if (appdata && *appdata) return path_of(appdata) / path_of(p->name);
    // elsewhere than Windows: where SDL keeps a program's data
    char *pref = SDL_GetPrefPath(nullptr, p->name.c_str());
    if (!pref) return {};
    fs::path dir = path_of(pref);
    SDL_free(pref);
    return dir;
}

fs::path game_dir(const std::string &title) {
    fs::path dir = data_dir();
    return title.empty() || dir.empty() ? fs::path() : dir / upper(title);
}

fs::path lock_file(const std::string &title) {
    fs::path dir = game_dir(title);
    return dir.empty() ? dir : dir / "building.lock";
}

fs::path build_dir(const std::string &title) {
    fs::path dir = game_dir(title);
    return dir.empty() ? dir : dir / "build";
}

fs::path update_dir(const std::string &title) {
    fs::path dir = game_dir(title);
    return dir.empty() ? dir : dir / "update";
}

fs::path user_dir() {
    fs::path dir = data_dir();
    return dir.empty() ? dir : dir / "user";
}

fs::path player_toml() {
    fs::path dir = data_dir();
    return dir.empty() ? dir : dir / "player.toml";
}

fs::path player_settings(const Config &config, std::string &error) {
    fs::path path = player_toml();
    if (path.empty()) {
        error = "there is no APPDATA folder";
        return {};
    }
    if (config.game.empty()) {
        error = "no game was chosen";
        return {};
    }
    // the library 3dsrecomp built
    fs::path library;
    std::error_code ec;
    for (const auto &entry : fs::directory_iterator(build_dir(config.title), ec)) {
        auto ext = entry.path().extension();
        if (ext == ".dll" || ext == ".so" || ext == ".dylib") {
            library = entry.path();
            break;
        }
    }
    if (library.empty()) {
        error = "the game was not built: no library";
        return {};
    }
    toml::table t = read_toml(path).value_or(toml::table{});
    // the [game] table anew: the player reads the program from the dumps
    // itself; what else it had (the UI's font and art, the scenes) kept
    toml::table game;
    if (const toml::table *before = t["game"].as_table()) game = *before;
    for (const char *key : {"game", "library", "user", "update", "decomp", "exheader", "code"}) game.erase(key);
    game.insert("game", utf8(config.game));
    game.insert("library", utf8(library));
    game.insert("user", utf8(user_dir()));
    if (!config.update.empty()) game.insert("update", utf8(config.update));
    if (fs::path decomp = config.decomp_dir(); !decomp.empty()) game.insert("decomp", utf8(decomp));
    t.insert_or_assign("game", std::move(game));
    fs::create_directories(user_dir(), ec);
    return write_toml(path, t, error) ? path : fs::path();
}

} // namespace launcher
