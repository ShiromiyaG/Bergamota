#include "check.h"

#include "runtime/dump.h"

namespace launcher {

std::string Update::name(const Game &game) const {
    if (game.tested && *game.tested == version && !game.tested_name.empty()) return game.tested_name;
    return "version " + version_name(version);
}

std::string version_name(uint16_t version) {
    return std::to_string(version >> 10) + "." + std::to_string(version >> 4 & 0x3F) + "." + std::to_string(version & 0xF);
}

bool check_game(const Port &port, const fs::path &path, Game &game, std::string &error) {
    runtime::DumpInfo info;
    std::string detail;
    switch (runtime::read_dump(path, info, detail)) {
    case runtime::DumpProblem::Encrypted: error = "This dump is encrypted. " + port.name + " needs a decrypted one."; return false;
    case runtime::DumpProblem::Unknown: error = "This is not a 3DS game (.3ds, .cci, .cxi or .cia)."; return false;
    case runtime::DumpProblem::Unreadable: error = "This file can't be read as a 3DS game: " + detail + "."; return false;
    case runtime::DumpProblem::None: break;
    }
    if (info.title_id >> 32 == 0x0004000E) {
        error = "This is an update. Pick the game first, then its update.";
        return false;
    }
    const GameInfo *known = port.game(info.title_id);
    if (!known) {
        error = "This is title " + title_of(info.title_id) + ". " + port.name + " is for " + port.names() + ".";
        return false;
    }
    game = Game{info.title_id, known->name, known->update, known->update_name};
    return true;
}

bool check_update(const Port &port, const fs::path &path, const Game &game, Update &update, std::string &error) {
    runtime::DumpInfo info;
    std::string detail;
    switch (runtime::read_dump(path, info, detail)) {
    case runtime::DumpProblem::Encrypted: error = "This update is encrypted. " + port.name + " needs a decrypted one."; return false;
    case runtime::DumpProblem::Unknown: error = "This file can't be read as an update: it is no CIA."; return false;
    case runtime::DumpProblem::Unreadable: error = "This file can't be read as an update: " + detail + "."; return false;
    case runtime::DumpProblem::None: break;
    }
    // an update's title id is the game's with 000E where the game has 0000
    if (!info.cia || info.title_id != (game.program_id | 0x0000000E00000000ull)) {
        error = "This update is for title " + title_of(info.title_id) + ", not " + game.name + ".";
        return false;
    }
    update = Update{info.version};
    return true;
}

} // namespace launcher
