// The port this launcher is for, from its port.toml: its name, its games and
// where its pieces are. One launcher for the ports on the runtime (Solstice,
// Duality, Tidefall), each packaged with its own port.toml beside the
// launcher; --port <file> names another while developing.
//
//   name = "Solstice"          # the window, the folder in %APPDATA%, the shortcut
//   player = "SolsticeGame"    # the runtime's player as the package names it (game\<player>.exe)
//   about = "..."              # what the port is
//
//   [[game]]
//   title = "00040000001B5000" # its title id, as the cartridge has it
//   name = "Pokémon Ultra Sun"
//   update = 2048              # the update it is tested with (the TMD's version), if any
//   update_name = "v1.2"       # and as players know it
//   decomp = "decomp"          # the port's code for it (decomp.bin, decomp.txt), a folder beside the launcher
//   color = "#F08A24"          # its color in the launcher (the glow, Play)
//
// The port tool (sdk/port.py) reads more keys of the same file, which the
// launcher leaves alone: all of them in docs/porting.md.
#ifndef LAUNCHER_PORT_H
#define LAUNCHER_PORT_H

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace launcher {

namespace fs = std::filesystem;

struct GameInfo {
    std::string title, name;
    std::optional<uint16_t> update;
    std::string update_name, decomp, color;

    std::optional<uint64_t> program_id() const;
};

struct Port {
    std::string name, player, about;
    std::vector<GameInfo> games;

    const GameInfo *game(uint64_t program_id) const;
    const GameInfo *game(const std::string &title) const;
    // the games' names, for saying which ones the port is for
    std::string names() const;
};

// false with why
bool parse_port(const std::string &text, Port &port, std::string &error);

// the command line, kept for what reads it later (--port, --play...)
void set_args(int argc, char **argv);
const std::vector<std::string> &args();
bool has_arg(const std::string &name);
// the value after an argument
std::optional<std::string> arg_after(const std::string &name);

// the port, read once; none with why
const Port *get_port(std::string *error = nullptr);
// the port, once the launcher has started with one
const Port &port();

// the launcher itself, and a file beside it
fs::path exe();
fs::path beside(const std::string &name);
// a folder the port's file names, beside the port's file
fs::path folder(const std::string &name);

// a path as UTF-8 and back
std::string utf8(const fs::path &path);
fs::path path_of(const std::string &utf8);

// title ids as files name them: 16 hex digits, upper case
std::string title_of(uint64_t program_id);
std::string upper(std::string text);

} // namespace launcher

#endif
