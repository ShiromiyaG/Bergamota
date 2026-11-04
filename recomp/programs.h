// A title's programs, the executable and its modules, each named and ready
// for discovery, made from what a mod changes in place of the title's own.
// Translated from 3dsrecomp's lib.rs (Rust, MIT, Copyright (c) 2026 fearkov;
// see LICENSE-3dsrecomp).

#ifndef RECOMP_PROGRAMS_H
#define RECOMP_PROGRAMS_H

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "cro.h"
#include "decomp.h"
#include "discover.h"
#include "rom.h"

namespace recomp {

// what a mod changes in a title, read in place of the title's own code and
// files. the default changes nothing.
struct Mods {
    // the executable's code, decompressed, its segments one after another
    // the way the title's own has them, or the way exheader says
    const std::vector<uint8_t> *code = nullptr;
    // the extended header the mod gives in place of the title's, at least
    // its first 0x400 bytes, which says where the segments of code lie when
    // the mod's code moves them
    const std::vector<uint8_t> *exheader = nullptr;
    // the RomFS files the mod replaces, none to keep the title's. the
    // modules and static.crs are read through it, by their paths from the
    // RomFS's root, / separated and spelled the way the title has them, like
    // static.crs or cro/Battle.cro. empty when the mod replaces no files.
    std::function<std::optional<std::vector<uint8_t>>(const std::string &path)> romfs;
    // code written apart from the title that replaces some of its
    // functions, see decomp.h
    const Decomp *decomp = nullptr;

    // the RomFS file at path, the bytes given in its place or else the
    // title's
    std::optional<std::vector<uint8_t>> read(const Title &title, const std::string &path,
                                             const FileEntry &file) const;
};

// a module and its file
using ModuleFile = std::pair<cro::Module, std::vector<uint8_t>>;

// the title's programs, the executable first and then its modules, each
// named and ready for discovery, made from what mods change in place of the
// title's own; false with why
bool programs(const Title &title, const Mods &mods, std::vector<std::pair<std::string, Program>> &out,
              std::string &error);

// the code addresses in module that the other modules, and the executable
// through its crs, take from it without a name, which nothing else may lead
// to
std::vector<uint32_t> imported(const std::vector<ModuleFile> &files, const cro::Module *crs,
                               const cro::Module &module);

// the main executable's module description, the static.crs every title
// carries in its RomFS, or the one a mod gives in its place
std::optional<cro::Module> static_module(const Title &title, const Mods &mods);

// every CRO module in the RomFS with its file, the one a mod gives in its
// place when it gives one
std::vector<ModuleFile> module_files(const Title &title, const Mods &mods);

// the CRO files in dir and in the folders in it, with their paths, path
// being dir's
void find_modules(const RomFs &romfs, const DirEntry &dir, const std::string &path,
                  std::vector<std::pair<std::string, FileEntry>> &files);

// the path of name in the folder at path. paths skip the nameless folders,
// the root and the one some titles keep everything in.
std::string join(const std::string &path, const std::string &name);

} // namespace recomp

#endif
