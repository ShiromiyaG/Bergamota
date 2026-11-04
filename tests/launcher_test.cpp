// The launcher without its window: port.toml read, 3dsrecomp's lines moving
// the stage on, versions as players know them, SHA-256. With
// LAUNCHER_TEST_GAME, LAUNCHER_TEST_UPDATE and LAUNCHER_TEST_UNPACKED
// (ctrtool's extraction of that update) also the real dumps: the game and
// its update known, and the update laid out as ctrtool lays it.

#include "build.h"
#include "check.h"

#include "runtime/decomp_options.h"
#include "runtime/dump.h"
#include "../frontend/settings.h"

#include <SDL3/SDL.h>

#include <cstdio>
#include <fstream>

using namespace launcher;

namespace {

int failures = 0;

#define CHECK(cond)                                                                                                    \
    do {                                                                                                               \
        if (!(cond)) {                                                                                                 \
            std::printf("%s:%d: %s\n", __FILE__, __LINE__, #cond);                                                     \
            failures++;                                                                                                \
        }                                                                                                              \
    } while (0)

std::vector<char> bytes(const fs::path &path) {
    std::ifstream in(path, std::ios::binary);
    return std::vector<char>((std::istreambuf_iterator<char>(in)), {});
}

const char *env(const char *name) { return SDL_GetEnvironmentVariable(SDL_GetEnvironment(), name); }

} // namespace

int main(int, char **) {
    Port port;
    std::string error;
    CHECK(parse_port("name = \"Solstice\"\nplayer = \"SolsticeGame\"\n[[game]]\ntitle = \"00040000001B5000\"\nname = \"Pokémon Ultra Sun\"\n"
                     "update = 2048\nupdate_name = \"v1.2\"\ndecomp = \"decomp\"\ncolor = \"#F08A24\"\n[[game]]\ntitle = \"00040000001B5100\"\n"
                     "name = \"Pokémon Ultra Moon\"\n",
                     port, error));
    CHECK(port.games.size() == 2);
    CHECK(port.game(0x00040000001B5100ull) && port.game(0x00040000001B5100ull)->name == "Pokémon Ultra Moon");
    CHECK(port.game("00040000001b5000") && port.game("00040000001b5000")->color == "#F08A24");
    CHECK(!port.game(0x0004000000055D00ull));
    CHECK(port.names() == "Pokémon Ultra Sun (00040000001B5000) or Pokémon Ultra Moon (00040000001B5100)");
    Port bad;
    CHECK(!parse_port("name = \"x\"\nplayer = \"y\"\n[[game]]\ntitle = \"zz\"\nname = \"n\"\n", bad, error));

    auto compiling = Stage::after("compiled 100 of 891");
    CHECK(compiling && compiling->kind == Stage::Compiling && compiling->done == 100 && compiling->total == 891);
    CHECK(Stage::after("compiled 891 of 891")->kind == Stage::Linking);
    CHECK(Stage::after("wrote 891 C files, 830 MiB, 0 overrides")->kind == Stage::Generating);
    CHECK(!Stage::after("built G:/x/00040000001B5000.dll in 648.1s"));

    Game sun{0x00040000001B5000ull, "Pokémon Ultra Sun", 2048, "v1.2"};
    CHECK(Update{2048}.name(sun) == "v1.2");
    CHECK(Update{1040}.name(sun) == "version 1.1.0");
    CHECK(Update{2048}.tested(sun) && !Update{1040}.tested(sun));
    CHECK(Update{1040}.tested(Game{sun.program_id, sun.name, std::nullopt, ""}));
    CHECK(title_of(0x0004000000055D00ull) == "0004000000055D00");

    // the port's options: kept in [port], the old [video] no_ghosting taken in
    fs::path toml = fs::temp_directory_path() / "launcher-test-player.toml";
    std::ofstream(toml, std::ios::binary) << "[video]\nno_ghosting = false\n";
    frontend::Settings old;
    CHECK(frontend::load(toml, old, error) && old.port.count("no_ghosting") && old.port["no_ghosting"] == 0);
    old.port["speed"] = 3;
    CHECK(frontend::save(toml, old, error));
    frontend::Settings again;
    CHECK(frontend::load(toml, again, error) && again.port == old.port);
    fs::remove(toml);
    runtime::DecompOption option;
    CHECK(runtime::parse_option("option port_speed int 2 1 4 Speed|How fast", option) && option.name == "speed" &&
          !option.toggle && option.max == 4 && option.help == "How fast");
    CHECK(!runtime::parse_option("option port_speed float 2 Speed", option));

    fs::path abc = fs::temp_directory_path() / "launcher-test-abc.txt";
    std::ofstream(abc, std::ios::binary) << "abc";
    CHECK(sha256(abc) == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    fs::remove(abc);

    // the real dumps, when named
    const char *game_file = env("LAUNCHER_TEST_GAME"), *update_file = env("LAUNCHER_TEST_UPDATE"), *unpacked = env("LAUNCHER_TEST_UNPACKED");
    if (game_file && update_file) {
        runtime::DumpInfo info;
        std::string detail;
        CHECK(runtime::read_dump(path_of(game_file), info, detail) == runtime::DumpProblem::None);
        Port any;
        any.name = "Test";
        any.games.push_back(GameInfo{title_of(info.title_id), "The game"});
        Game game;
        CHECK(check_game(any, path_of(game_file), game, error));
        Update update;
        CHECK(check_update(any, path_of(update_file), game, update, error));
        std::printf("game %s, update version %s\n", game.title().c_str(), version_name(update.version).c_str());
        if (unpacked) {
            fs::path out = fs::temp_directory_path() / "launcher-test-unpack";
            fs::remove_all(out);
            CHECK(runtime::unpack_update(path_of(update_file), out, error));
            fs::path known = path_of(unpacked);
            fs::path code = fs::exists(known / "code.bin") ? known / "code.bin" : known / "exefs" / "code.bin";
            CHECK(bytes(out / "code.bin") == bytes(code));
            // ctrtool leaves the exheader saying compressed; ours says what code.bin is
            auto ours = bytes(out / "exheader.bin"), theirs = bytes(known / "exheader.bin");
            CHECK(ours.size() == theirs.size() && ours.size() > 0x0D);
            if (ours.size() == theirs.size() && ours.size() > 0x0D) {
                ours[0x0D] = theirs[0x0D] = 0;
                CHECK(ours == theirs);
            }
            size_t modules = 0;
            for (auto &entry : fs::recursive_directory_iterator(out / "romfs")) modules += entry.is_regular_file();
            std::printf("%zu modules\n", modules);
            CHECK(modules > 0);
            fs::remove_all(out);
        }
    }

    if (failures) std::printf("%d failures\n", failures);
    else std::printf("launcher: ok\n");
    return failures != 0;
}
