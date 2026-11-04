// Recompiling a title from end to end: writing the C, compiling it and
// installing the library, for the command line and for programs that run it
// themselves and follow it through its events. Translated from 3dsrecomp's
// build.rs and abi's paths (Rust, MIT, Copyright (c) 2026 fearkov; see
// LICENSE-3dsrecomp).

#ifndef RECOMP_BUILD_H
#define RECOMP_BUILD_H

#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <variant>
#include <vector>

#include "compile.h"
#include "discover.h"
#include "programs.h"

namespace recomp {

class Title;

// what happens along the way
struct Event {
    enum Kind { Generated, Note, Compiled, Built, Installed } kind = Note;
    size_t files = 0, bytes = 0, overrides = 0; // Generated
    std::string note;                           // Note
    size_t done = 0, total = 0;                 // Compiled
    std::filesystem::path library;              // Built, Installed
    std::chrono::nanoseconds took{0};           // Built
};
using Events = std::function<void(const Event &)>;

// what a build is to do
struct BuildOptions {
    // where to work, the cache when none, and then the library gets
    // installed where hosts find it
    std::optional<std::filesystem::path> dir;
    // C files of functions written by hand
    std::optional<std::filesystem::path> overrides;
    // set from elsewhere to stop it
    const std::atomic<bool> *cancel = nullptr;
    // the code and modules a mod changes, recompiled in place of the
    // title's own
    Mods mods;
    // the C compiler, the one CC names or the first there is when none
    std::optional<Compiler> compiler;
    // compile below normal priority and leave a core
    bool background = false;
    // the heat for the title: build only the functions that cover nearly
    // all of it, and those the host had to interpret
    std::optional<std::filesystem::path> hot;
    // with hot: build all of it, the hot functions optimized and the rest
    // compiled without optimizing
    bool cold_fast = false;
    // the C written, nothing compiled (for comparing what two builds write)
    bool c_only = false;
};

// the title's code written as C
struct Generated {
    std::unique_ptr<Title> title;
    std::filesystem::path dir;
    // the files to compile, names inside dir
    std::vector<std::string> sources;
};

// writes the title at rom as C in the dir pick chooses, the overrides with
// it, what the mods change in place of the title's own; with hot and
// cold_fast all the code, the hot functions in files of their own
bool generate(const std::filesystem::path &rom, const std::function<std::filesystem::path(const Title &)> &pick,
              const std::optional<std::filesystem::path> &overrides, const Mods &mods,
              const std::optional<std::filesystem::path> &hot, bool cold_fast, const Events &events, Generated &out,
              std::string &error);

// recompiles the title at rom into a library; where it ended up, installed
// when the options name no dir
bool build(const std::filesystem::path &rom, const BuildOptions &options, const Events &events,
           std::filesystem::path &library, std::string &error);

// takes in where the host had to interpret the title's last library: the
// places outside the functions found start functions, found by analyzing
// the executable again. how many, and the places
std::pair<size_t, std::vector<uint32_t>> apply_hints(const Title &title, std::vector<std::pair<std::string, Program>> &programs,
                                                     std::vector<Analysis> &analyses);
// the hinted places none of the functions found holds, odd for Thumb
std::vector<uint32_t> new_functions(const Analysis &analysis, const std::vector<uint32_t> &hinted);
// the addresses the host wrote down next to a title's library
std::vector<uint32_t> hints(uint64_t program_id);
std::vector<uint32_t> read_hints(const std::string &text);
// FNV-1a over the executable's code, so one revision's hints stay out of
// another's
uint64_t code_hash(const Program &program);
// the hints the bundled list (hints.txt) has for a title's code
std::vector<uint32_t> bundled(uint64_t program_id, uint64_t hash);
std::vector<uint32_t> section(const std::string &list, uint64_t program_id, uint64_t hash);

// where build works on a title whose library it installs
std::filesystem::path cache_dir(uint64_t program_id);
// where libraries are installed for hosts to find, one per title
std::optional<std::filesystem::path> library_dir();
// a title's library's name: its title id and the system's extension
std::string library_name(uint64_t program_id);
// copies the library to where hosts look for it; false with why
bool install(const std::filesystem::path &library, uint64_t program_id, std::filesystem::path &installed,
             std::string &error);

// the bundled list of hints, built in
extern const std::string BUNDLED;

} // namespace recomp

#endif
