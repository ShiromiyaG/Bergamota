// What the player chose, kept in %APPDATA%\<port>\config.toml so later
// launches go straight to the game: for each of the port's games they set
// up, its dump, its update and how far it is built (each in a folder of its
// own, by title id), the one played last, and the tools when they had to
// point at them.
#ifndef LAUNCHER_LIBRARY_H
#define LAUNCHER_LIBRARY_H

#include "port.h"

#include <map>

namespace launcher {

// one game, as set up
struct Config {
    std::string title; // 16 hex digits, empty before a game is chosen
    fs::path game;     // the player's own dump
    fs::path update;   // its update, empty to play without one
    bool built = false; // recompiled with that game and update, at least its hottest functions
    bool full = false;  // all of it, not only the hottest functions
    fs::path decomp;    // the port's code for the game's ARM11, when not the folder port.toml names
    std::optional<uint64_t> decomp_built; // the fingerprint of the port's code it was built with

    // everything in place to play: built, its files where they were, with
    // the port's code there is now
    bool ready() const;
    // the port's code the game is built and played with, empty for none
    fs::path decomp_dir() const;
    bool operator==(const Config &) const = default;
};

struct Library {
    std::string last; // the title played last
    fs::path recomp, player; // the tools, when the player had to point at them
    // a fast compile, for modest processors: the rest of the game built
    // in the background without optimizing
    bool fast = false;
    std::map<std::string, Config> games; // by title id

    static Library load();
    // false with why
    bool save(std::string &error) const;
    // the game played last, else the first set up, else none chosen yet
    Config current() const;
    // a game set up (or set up again), now the one played last
    void put(const Config &config);
    // the games ready to play, in the port's order
    std::vector<Config> ready() const;
};

// 64-bit FNV-1a over a decomp's decomp.txt and decomp.bin
std::optional<uint64_t> fingerprint(const fs::path &dir);

// the port's folder in %APPDATA%, empty without one
fs::path data_dir();
fs::path game_dir(const std::string &title);
// there while a full build of the game runs in the background
fs::path lock_file(const std::string &title);
fs::path build_dir(const std::string &title);
fs::path update_dir(const std::string &title);
// the saves of all the port's games, laid out as Zakuro lays them
fs::path user_dir();
// the player's settings file
fs::path player_toml();

// the player's settings with their [game] table written from config before
// each start; the file, or empty with why
fs::path player_settings(const Config &config, std::string &error);

} // namespace launcher

#endif
