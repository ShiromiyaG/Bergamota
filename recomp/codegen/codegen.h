// Writing a title's functions as C against recomp.h. Translated from
// 3dsrecomp's codegen/ (Rust, MIT, Copyright (c) 2026 fearkov; see
// LICENSE-3dsrecomp). Its output is the Rust's byte for byte: the same files,
// the same names, the same text.

#ifndef RECOMP_CODEGEN_H
#define RECOMP_CODEGEN_H

#include <cstdint>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "../discover.h"
#include "../overrides.h"

namespace recomp {

// recomp.h, the interface the generated code runs against (abi/recomp.h of
// 3dsrecomp, third_party/recomp.h here), written beside it
extern const std::string HEADER;

// how a build lays its functions out, for builds after it in the same place
// to keep what they can
struct Layout {
    // the number each program's functions had in the build before
    // (numbers.txt), by the program's prefix ("" the executable's) and entry
    std::map<std::string, std::map<uint32_t, uint32_t>> numbers;
    // for each program, the functions the game spends nearly all its time
    // in: in files of their own (hot_*.c)
    std::optional<std::vector<std::set<uint32_t>>> hot;

    // numbers.txt as generate writes it: the prefix ("-" the executable's),
    // the entry and its number on each line
    static std::map<std::string, std::map<uint32_t, uint32_t>> read_numbers(const std::string &text);
};

// a program to write as C, the executable or one of its modules
struct Unit {
    // the module's name, none for the executable, whose code does not move
    std::optional<std::string> module;
    const Program *program = nullptr;
    const Analysis *analysis = nullptr;
};

// whether a function becomes C, which it does unless its entry turned out
// not to be code
bool recompiles(uint32_t entry, const Function &function);

// the C sources for the units, the executable first, as file names and
// contents, in the Rust's order; the overrides take the place of the
// functions they replace
std::vector<std::pair<std::string, std::string>> generate(const std::vector<Unit> &units,
                                                          const std::vector<Override> &overrides,
                                                          const Layout &layout);

} // namespace recomp

#endif
