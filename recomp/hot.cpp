// Building first only the functions a game spends nearly all its time in,
// from the heat Zakuro writes with --heat. Translated from 3dsrecomp's hot.rs
// (Rust, MIT, Copyright (c) 2026 fearkov; see LICENSE-3dsrecomp).

#include "hot.h"

#include <algorithm>
#include <tuple>

#include "rust.h"

namespace recomp::hot {

bool Heat::load(const std::filesystem::path &path, Heat &out, std::string &error) {
    std::string text, why;
    if (!rust::read_to_string(path, text, why)) {
        error = "could not read " + rust::display(path) + ", " + why;
        return false;
    }
    out = parse(text);
    return true;
}

Heat Heat::parse(const std::string &text) {
    Heat heat;
    for (std::string_view untrimmed : rust::lines(text)) {
        std::string_view line = rust::trim(untrimmed);
        if (line.empty() || line[0] == '#') continue;
        std::vector<std::string_view> fields = rust::split_whitespace(line);
        if (fields.size() != 3) continue;
        auto at = rust::parse_u32(fields[1], 16);
        auto hits = rust::parse_unsigned(fields[2], 10, UINT64_MAX);
        if (!at || !hits) continue;
        // the Thumb bit says nothing of which function holds the place
        heat.samples[std::string(fields[0])][*at & ~1u] += *hits;
    }
    return heat;
}

Selection select(const std::vector<std::pair<std::string, Program>> &programs, const std::vector<Analysis> &analyses,
                 const Heat &heat, double share) {
    std::vector<std::tuple<uint64_t, size_t, uint32_t>> ranked;
    size_t count = std::min(programs.size(), analyses.size());
    for (size_t index = 0; index < count; index++) {
        auto found = heat.samples.find(programs[index].first);
        if (found == heat.samples.end()) continue;
        const auto &samples = found->second;
        for (const auto &[entry, function] : analyses[index].functions) {
            uint64_t hits = 0;
            for (uint32_t at : function.instructions) {
                auto sample = samples.find(at);
                if (sample != samples.end()) hits += sample->second;
            }
            if (hits > 0) ranked.emplace_back(hits, index, entry);
        }
    }
    // every entry is there once, so the order is the one the Rust's
    // unstable sort gives too
    std::sort(ranked.begin(), ranked.end(), [](const auto &a, const auto &b) { return b < a; });
    uint64_t total = 0;
    for (const auto &item : ranked) total += std::get<0>(item);
    Selection selection;
    selection.keep.assign(programs.size(), {});
    uint64_t taken = 0;
    for (const auto &[hits, index, entry] : ranked) {
        if (double(taken) >= share * double(total)) break;
        selection.keep[index].insert(entry);
        taken += hits;
    }
    for (const auto &kept : selection.keep) selection.functions += kept.size();
    for (const auto &analysis : analyses) selection.of += analysis.functions.size();
    selection.covered = total == 0 ? 0.0 : double(taken) / double(total);
    return selection;
}

void prune(std::vector<Analysis> &analyses, const std::vector<std::set<uint32_t>> &keep,
           const std::map<size_t, std::set<uint32_t>> &also) {
    const std::set<uint32_t> none;
    for (size_t index = 0; index < analyses.size(); index++) {
        auto found = also.find(index);
        const std::set<uint32_t> &extra = found == also.end() ? none : found->second;
        auto &functions = analyses[index].functions;
        for (auto it = functions.begin(); it != functions.end();) {
            const auto &[entry, function] = *it;
            bool kept = keep[index].count(entry) != 0 ||
                        std::any_of(function.labels.begin(), function.labels.end(),
                                    [&](uint32_t label) { return extra.count(label) != 0; });
            it = kept ? std::next(it) : functions.erase(it);
        }
    }
}

} // namespace recomp::hot
