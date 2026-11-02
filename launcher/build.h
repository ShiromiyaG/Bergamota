// Recompiling the game with 3dsrecomp, following what it prints; the C
// compiler it builds with (CC, else clang when it links a DLL here, else Zig
// downloaded once into the port's folder); and the programs the launcher
// starts: 3dsrecomp, the runtime's player, itself for the rest of a build.
#ifndef LAUNCHER_BUILD_H
#define LAUNCHER_BUILD_H

#include "library.h"

#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>

struct SDL_Process;
struct SDL_Environment;

namespace launcher {

struct Stage {
    enum Kind { Starting, Downloading, Generating, Compiling, Linking, Done, Failed } kind = Starting;
    uint64_t done = 0, total = 0; // bytes of Zig, or files compiled
    std::string error;

    // how far along, 0 to 1, going by the part that takes longest
    float fraction() const;
    std::string describe() const;
    bool finished() const { return kind == Done || kind == Failed; }
    // where a line 3dsrecomp prints leaves it, none for one that changes nothing
    static std::optional<Stage> after(const std::string &line);
};

struct Tools {
    fs::path recomp, player;
    // both found, where the config says or where the package puts them
    static std::optional<Tools> find(const Library &library);
};

// where a program may be: where LAUNCHER_<NAME> says while developing, in a
// folder of its name beside the launcher, or beside it
std::vector<fs::path> places(const std::string &name);
// where the runtime's player may be: game\<player>.exe as the package names it
std::vector<fs::path> game_places();

// the game's hottest functions (hot\<title>.txt beside the launcher), empty
// when the port has none for it
fs::path hot_list(const std::string &title);

// 3dsrecomp's arguments for building a game, its update laid out first;
// empty with why
std::vector<std::string> recomp_args(const Tools &tools, const Config &config, std::string &error);

// a program started and left to run: the player, a background build
bool start(const std::vector<std::string> &args, std::string &error);

// a recompile running in 3dsrecomp, after the compiler is found
class Job {
public:
    // with hot, only the game's hottest functions; fast, the rest unoptimized
    static std::unique_ptr<Job> start(const Tools &tools, const Config &config, const fs::path &hot, bool cold_fast);
    static std::unique_ptr<Job> failed(const std::string &error);
    ~Job();

    Stage stage();
    void cancel();
    // until it ends (the background build)
    void wait();

private:
    void set(const Stage &next);
    std::mutex lock_;
    Stage stage_;
    SDL_Process *process_ = nullptr;
    std::atomic<bool> cancelled_{false};
    std::thread thread_;
    void work(std::vector<std::string> args);
    // a program run as the job's, which cancelling kills: each line it prints
    // to on_line, while_running called as it runs; false with why
    bool run(const std::vector<std::string> &args, SDL_Environment *env, const std::function<void(const std::string &)> &on_line,
             const std::function<void()> &while_running, std::string &error);
    // CC set in env for 3dsrecomp, Zig downloaded first when there is no other
    bool prepare_compiler(SDL_Environment *env, std::string &error);
};

// the whole of a game built below the game, noted in the config once done:
// the launcher run with --build-full <title>; one at a time
void build_full(const std::string &title);
// the rest of the game built in the background, when only its hottest
// functions are
void finish_in_background(const Config &config);

// the game started in the runtime's player, the rest of its build behind it
bool play(const Tools &tools, const Config &config, std::string &error);

// a shortcut to the launcher on the desktop, named after the port
bool desktop_shortcut(std::string &error);

// SHA-256 of a file as hex, empty when it can't be read
std::string sha256(const fs::path &path);

} // namespace launcher

#endif
