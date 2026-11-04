// Building first only the functions a game spends nearly all its time in,
// from the heat Zakuro writes with --heat: samples by program and address, a
// module's by its offset from where its file starts. The rest stays in the
// interpreter until a full build, which a host can run in the background
// while the game is played. Translated from 3dsrecomp's hot.rs (Rust, MIT,
// Copyright (c) 2026 fearkov; see LICENSE-3dsrecomp).

#ifndef RECOMP_HOT_H
#define RECOMP_HOT_H

#include <cstdint>
#include <filesystem>
#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "discover.h"

namespace recomp::hot {

// how much of the samples the functions built first cover
constexpr double SHARE = 0.999;

// samples by program name, "executable" or a module's, and address
struct Heat {
    // (a HashMap in the Rust, only ever looked up)
    std::map<std::string, std::map<uint32_t, uint64_t>> samples;

    // false with why
    static bool load(const std::filesystem::path &path, Heat &out, std::string &error);
    static Heat parse(const std::string &text);
};

// what select kept, for telling
struct Selection {
    // for each program, the entries of the functions to build
    std::vector<std::set<uint32_t>> keep;
    size_t functions = 0;
    size_t of = 0;
    // the share of the samples in the functions found that they cover
    double covered = 0;
};

// the functions of each program, hottest first, until they hold share of
// the samples that fall in any function. a place several functions share
// counts for each of them.
Selection select(const std::vector<std::pair<std::string, Program>> &programs, const std::vector<Analysis> &analyses,
                 const Heat &heat, double share);

// leaves in each analysis only the functions keep names, and those in also,
// the ones overrides replace, which have to be generated for them
void prune(std::vector<Analysis> &analyses, const std::vector<std::set<uint32_t>> &keep,
           const std::map<size_t, std::set<uint32_t>> &also);

} // namespace recomp::hot

#endif
