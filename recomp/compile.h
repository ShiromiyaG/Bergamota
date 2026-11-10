// Compiling the generated C, a compiler per core as long as the memory holds
// them, into a shared library a host loads. Translated from 3dsrecomp's
// compile.rs (Rust, MIT, Copyright (c) 2026 fearkov; see LICENSE-3dsrecomp).

#ifndef RECOMP_COMPILE_H
#define RECOMP_COMPILE_H

#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace recomp {

// what progress hears after each file, how many are done and of how many,
// answering whether to go on
using Progress = std::function<bool(size_t done, size_t total)>;

// a C compiler: a program, and the arguments that go before the rest, none
// for gcc, cc for zig, whose compiler is zig cc
struct Compiler {
    std::filesystem::path program;
    std::vector<std::string> leading;

    // the compiler CC names, or else the first of the usual ones that runs;
    // on Windows MinGW's gcc comes first, it links a DLL with nothing else
    static std::optional<Compiler> find();
    // what CC holds: a program, or a program and the arguments that go
    // first, separated by spaces, like zig cc
    static Compiler named(const std::string &value);
    // whether it runs, asked for its version
    bool runs() const;
    // whether it is Zig (zig cc; libzig.so on Android), which adds flags of
    // its own
    bool is_zig() const;
    // whether it is gcc, whose options for its garbage the others refuse
    bool is_gcc() const;
    // how it is called, for messages
    std::string describe() const;
    bool operator==(const Compiler &) const = default;
};

// the compiler to build with, the one given or else the one find() picks,
// when it runs; false with why
bool check_compiler(const std::optional<Compiler> &given, Compiler &out, std::string &error);

// compiles sources, file names inside dir, each into an object beside it
// (kept when made before from the same C, compiler and flags), the ones
// quick says without optimizing, and links them into library. in the
// background the compilers run below normal priority and leave a core
bool compile(const Compiler &compiler, const std::filesystem::path &dir, const std::vector<std::string> &sources,
             const std::filesystem::path &library, const Progress &progress, bool background,
             const std::function<bool(const std::string &)> &quick, std::string &error);

// the objects alone
bool objects(const Compiler &compiler, const std::filesystem::path &dir, const std::vector<std::string> &sources,
             const Progress &progress, bool background, const std::function<bool(const std::string &)> &quick,
             std::vector<std::filesystem::path> &out, std::string &error);

// links objects into a shared library
bool shared(const Compiler &compiler, const std::vector<std::filesystem::path> &objects,
            const std::filesystem::path &library, std::string &error);

// 64-bit FNV-1a over bytes, going on from hash
uint64_t fnv(uint64_t hash, const void *bytes, size_t size);
constexpr uint64_t FNV_OFFSET = 0xCBF29CE484222325ull;

} // namespace recomp

#endif
