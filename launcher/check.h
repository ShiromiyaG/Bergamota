// Whether the files the player picks are one of the port's games and its
// update, said in words a player can act on.
#ifndef LAUNCHER_CHECK_H
#define LAUNCHER_CHECK_H

#include "port.h"

namespace launcher {

struct Game {
    uint64_t program_id = 0;
    std::string name;
    std::optional<uint16_t> tested; // the update the port is tested with (the TMD's version)
    std::string tested_name;

    std::string title() const { return title_of(program_id); }
};

struct Update {
    uint16_t version = 0;
    // as players know it: the port's name for the tested one, else the TMD's
    std::string name(const Game &game) const;
    bool tested(const Game &game) const { return !game.tested || *game.tested == version; }
};

// "1.2.0" from the TMD's 2048
std::string version_name(uint16_t version);

// the game a dump is, or why it is none of the port's
bool check_game(const Port &port, const fs::path &path, Game &game, std::string &error);
// the update a CIA is, or why it is not the game's
bool check_update(const Port &port, const fs::path &path, const Game &game, Update &update, std::string &error);

} // namespace launcher

#endif
