// The recompiler's driver without a game: CC read as 3dsrecomp reads it, a
// compiler that does not run turned down, many objects linked whatever
// their paths, and the bundled hints going to the code they are for.
// Translated from 3dsrecomp's compile.rs and build.rs tests.

#include <cstdio>
#include <fstream>
#include <string>
#include <vector>

#include "build.h"
#include "compile.h"

using namespace recomp;
namespace fs = std::filesystem;

namespace {

int failures = 0;

#define CHECK(cond)                                                                                                    \
    do {                                                                                                               \
        if (!(cond)) {                                                                                                 \
            std::printf("%s:%d: %s\n", __FILE__, __LINE__, #cond);                                                     \
            failures++;                                                                                                \
        }                                                                                                              \
    } while (0)

void cc_names_a_program_and_what_goes_before_the_rest() {
    CHECK((Compiler::named("zig cc") == Compiler{"zig", {"cc"}}));
    CHECK(Compiler::named("zig cc").is_zig());
    CHECK(Compiler::named("/data/app/lib/arm64/libzig.so cc -target aarch64-linux-musl").is_zig());
    CHECK(!Compiler::named("clang").is_zig());
    CHECK((Compiler::named("gcc") == Compiler{"gcc", {}}));
    // a program that is there stays whole, spaces and all
    fs::path dir = fs::temp_directory_path() / "3dsrecomp cc test";
    fs::create_directories(dir);
    fs::path program = dir / "my cc";
    std::ofstream(program) << "";
    auto s = program.u8string();
    CHECK((Compiler::named(std::string(s.begin(), s.end())) == Compiler{program, {}}));
    fs::remove_all(dir);
}

void a_compiler_that_does_not_run_is_turned_down() {
    Compiler out;
    std::string error;
    CHECK(!check_compiler(Compiler{"/nowhere/cc", {}}, out, error));
    CHECK(error.rfind("there is no C compiler (/nowhere/cc)", 0) == 0);
}

// a big game's objects link however long their paths are, on Windows too,
// where a command line holds 32767 characters
void objects_link_however_long_their_paths_are() {
    auto compiler = Compiler::find();
    if (!compiler) return;
    std::string folder = "3dsrecomp ";
    for (int i = 0; i < 3; i++) folder += "a folder with a long name ";
    folder += "1"; // as the Rust's ends in its process id: Windows drops a trailing space
    fs::path dir = fs::temp_directory_path() / folder;
    fs::create_directories(dir);
    std::vector<std::string> sources;
    size_t length = 0;
    for (int i = 0; i < 400; i++) {
        char name[32];
        std::snprintf(name, sizeof name, "code_%04d.c", i);
        sources.push_back(name);
        std::ofstream(dir / name) << "int f" << i << "(void) { return " << i << "; }\n";
        length += fs::path(dir / name).replace_extension("o").native().size() + 1;
    }
    CHECK(length > 32767);
#ifdef _WIN32
    fs::path library = dir / "many.dll";
#else
    fs::path library = dir / "libmany.so";
#endif
    std::string error;
    bool built = compile(*compiler, dir, sources, library, [](size_t, size_t) { return true; }, false,
                         [](const std::string &) { return false; }, error);
    CHECK(built);
    if (!built) std::printf("%s\n", error.c_str());
    CHECK(fs::exists(library));
    std::error_code ec;
    fs::remove_all(dir, ec);
}

// a game's hints in the list go to the code they were gathered on
void the_list_gives_hints_to_the_code_they_are_for() {
    std::string list = "# a note\n= 0004000000033500 00000000000000AB\n00100000\n00100011\n\n= 0004000000033500 00000000000000AC\n00200000\n";
    CHECK((section(list, 0x0004000000033500ull, 0xAB) == std::vector<uint32_t>{0x00100000, 0x00100011}));
    CHECK((section(list, 0x0004000000033500ull, 0xAC) == std::vector<uint32_t>{0x00200000}));
    CHECK(section(list, 0x0004000000033600ull, 0xAB).empty());
    CHECK(section(list, 0x0004000000033500ull, 0xAD).empty());
}

void every_line_of_the_list_reads() {
    size_t headers = 0, addresses = 0;
    size_t at = 0;
    while (at < BUNDLED.size()) {
        size_t end = BUNDLED.find('\n', at);
        std::string line = BUNDLED.substr(at, end == std::string::npos ? std::string::npos : end - at);
        at = end == std::string::npos ? BUNDLED.size() : end + 1;
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
        if (line.empty() || line[0] == '#') continue;
        if (line[0] == '=') headers++;
        else addresses++;
    }
    CHECK(headers > 0 && addresses > 0);
    // what section reads of them: every address under some header
    CHECK(read_hints("# x\n0x00100000\n00100001\nzz\n") == (std::vector<uint32_t>{0x00100000, 0x00100001}));
}

} // namespace

int main() {
    cc_names_a_program_and_what_goes_before_the_rest();
    a_compiler_that_does_not_run_is_turned_down();
    objects_link_however_long_their_paths_are();
    the_list_gives_hints_to_the_code_they_are_for();
    every_line_of_the_list_reads();
    if (failures) std::printf("%d failed\n", failures);
    return failures ? 1 : 0;
}
