// Recompiling a title from end to end (build.h). Translated from 3dsrecomp's
// build.rs and abi's paths (Rust, MIT, Copyright (c) 2026 fearkov; see
// LICENSE-3dsrecomp).

#include "build.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <map>
#include <set>
#include <sstream>
#include <unordered_set>

#include "codegen/codegen.h"
#include "decomp.h"
#include "hot.h"
#include "overrides.h"
#include "rom.h"

namespace fs = std::filesystem;

namespace recomp {

namespace {

#ifdef _WIN32
constexpr const char *DLL_EXTENSION = "dll";
#elif defined(__APPLE__)
constexpr const char *DLL_EXTENSION = "dylib";
#else
constexpr const char *DLL_EXTENSION = "so";
#endif

std::optional<fs::path> var(const char *name) {
    const char *value = std::getenv(name);
    if (!value || !*value) return std::nullopt;
    return fs::path(std::u8string(reinterpret_cast<const char8_t *>(value)));
}

std::string utf8(const fs::path &path) {
    auto s = path.u8string();
    return {s.begin(), s.end()};
}

bool read_text(const fs::path &path, std::string &out) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return false;
    out.assign(std::istreambuf_iterator<char>(in), {});
    return true;
}

std::string trim(const std::string &s) {
    size_t a = s.find_first_not_of(" \t\r\n"), b = s.find_last_not_of(" \t\r\n");
    return a == std::string::npos ? std::string() : s.substr(a, b - a + 1);
}

// str::lines: split at \n, a \r before it dropped
std::vector<std::string> lines(const std::string &text) {
    std::vector<std::string> out;
    size_t at = 0;
    while (at < text.size()) {
        size_t end = text.find('\n', at);
        std::string line = text.substr(at, end == std::string::npos ? std::string::npos : end - at);
        if (!line.empty() && line.back() == '\r') line.pop_back();
        out.push_back(line);
        if (end == std::string::npos) break;
        at = end + 1;
    }
    return out;
}

// u32::from_str_radix(text, 16), the whole of it
std::optional<uint64_t> hex(const std::string &text, int bits) {
    if (text.empty()) return std::nullopt;
    uint64_t value = 0;
    size_t start = text[0] == '+' ? 1 : 0;
    if (start == text.size()) return std::nullopt;
    for (size_t i = start; i < text.size(); i++) {
        char c = text[i];
        int digit = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
        if (digit < 0) return std::nullopt;
        if (value >> (bits - 4)) return std::nullopt; // past the type
        value = value << 4 | uint64_t(digit);
    }
    return value;
}

std::string trim_start_0x(const std::string &line) {
    std::string out = line;
    while (out.rfind("0x", 0) == 0) out = out.substr(2);
    return out;
}

// moves the library at target out of the way, when there is one: Windows
// refuses to replace a library a game being played has loaded, but lets it
// be renamed. those moved away earlier go when nothing holds them any more
bool step_aside(const fs::path &target, std::error_code &ec) {
#ifdef _WIN32
    if (!fs::exists(target)) return true;
    auto aside = [&](unsigned n) { return fs::path(target).replace_extension(std::string(DLL_EXTENSION) + ".old" + std::to_string(n)); };
    for (unsigned n = 0; n < 16; n++) {
        std::error_code ignored;
        fs::remove(aside(n), ignored); // still loaded, or not there
    }
    fs::path free = aside(16);
    for (unsigned n = 0; n < 16; n++) {
        if (!fs::exists(aside(n))) {
            free = aside(n);
            break;
        }
    }
    fs::rename(target, free, ec);
    return !ec;
#else
    (void)target;
    (void)ec;
    return true;
#endif
}

} // namespace

std::optional<fs::path> library_dir() {
#ifdef _WIN32
    auto dir = var("APPDATA");
#elif defined(__APPLE__)
    auto home = var("HOME");
    auto dir = home ? std::optional<fs::path>(*home / "Library/Application Support") : std::nullopt;
#else
    auto dir = var("XDG_DATA_HOME");
    if (!dir) {
        if (auto home = var("HOME")) dir = *home / ".local/share";
    }
#endif
    if (!dir) return std::nullopt;
    return *dir / "3dsrecomp";
}

std::string library_name(uint64_t program_id) {
    char name[48];
    std::snprintf(name, sizeof name, "%016llX.%s", (unsigned long long)program_id, DLL_EXTENSION);
    return name;
}

fs::path cache_dir(uint64_t program_id) {
#ifdef _WIN32
    auto base = var("LOCALAPPDATA");
#elif defined(__APPLE__)
    auto home = var("HOME");
    auto base = home ? std::optional<fs::path>(*home / "Library/Caches") : std::nullopt;
#else
    auto base = var("XDG_CACHE_HOME");
    if (!base) {
        if (auto home = var("HOME")) base = *home / ".cache";
    }
#endif
    char id[24];
    std::snprintf(id, sizeof id, "%016llX", (unsigned long long)program_id);
    return base.value_or(fs::temp_directory_path()) / "3dsrecomp" / id;
}

bool install(const fs::path &library, uint64_t program_id, fs::path &installed, std::string &error) {
    auto dir = library_dir();
    if (!dir) {
        error = "there is no place to install the library";
        return false;
    }
    fs::path target = *dir / library_name(program_id);
    fs::path partial = fs::path(target).replace_extension(std::string(DLL_EXTENSION) + ".new");
    std::error_code ec;
    fs::create_directories(*dir, ec);
    if (!ec) fs::copy_file(library, partial, fs::copy_options::overwrite_existing, ec);
    if (!ec) step_aside(target, ec);
    if (!ec) fs::rename(partial, target, ec);
    if (ec) {
        error = "could not install " + utf8(target) + ", " + ec.message();
        return false;
    }
    installed = target;
    return true;
}

std::vector<uint32_t> read_hints(const std::string &text) {
    std::vector<uint32_t> out;
    for (const std::string &raw : lines(text)) {
        std::string line = trim(raw);
        if (line.empty() || line[0] == '#') continue;
        if (auto value = hex(trim_start_0x(line), 32)) out.push_back(uint32_t(*value));
    }
    return out;
}

std::vector<uint32_t> hints(uint64_t program_id) {
    auto dir = library_dir();
    if (!dir) return {};
    fs::path path = fs::path(*dir / library_name(program_id)).replace_extension("hints");
    std::string text;
    return read_text(path, text) ? read_hints(text) : std::vector<uint32_t>();
}

uint64_t code_hash(const Program &program) {
    uint64_t hash = 0xCBF29CE484222325ull;
    for (uint8_t byte : program.text.bytes) hash = (hash ^ byte) * 0x00000100000001B3ull;
    return hash;
}

std::vector<uint32_t> section(const std::string &list, uint64_t program_id, uint64_t hash) {
    bool taking = false;
    std::vector<uint32_t> found;
    for (const std::string &raw : lines(list)) {
        std::string line = trim(raw);
        if (line.empty() || line[0] == '#') continue;
        if (line[0] == '=') {
            std::istringstream fields(line.substr(1));
            std::string a, b;
            std::optional<uint64_t> first, second;
            if (fields >> a) first = hex(a, 64);
            if (fields >> b) second = hex(b, 64);
            taking = first == program_id && second == hash;
        } else if (taking) {
            if (auto value = hex(trim_start_0x(line), 32)) found.push_back(uint32_t(*value));
        }
    }
    return found;
}

std::vector<uint32_t> bundled(uint64_t program_id, uint64_t hash) { return section(BUNDLED, program_id, hash); }

std::vector<uint32_t> new_functions(const Analysis &analysis, const std::vector<uint32_t> &hinted) {
    std::unordered_set<uint32_t> owned;
    for (const auto &[entry, function] : analysis.functions) {
        uint32_t thumb = function.mode == Mode::Thumb ? 1 : 0;
        for (uint32_t address : function.instructions) owned.insert(address | thumb);
    }
    std::vector<uint32_t> out;
    for (uint32_t address : hinted) {
        if (!owned.count(address)) out.push_back(address);
    }
    return out;
}

std::pair<size_t, std::vector<uint32_t>> apply_hints(const Title &title, std::vector<std::pair<std::string, Program>> &programs,
                                                     std::vector<Analysis> &analyses) {
    size_t index = programs.size();
    for (size_t i = 0; i < programs.size(); i++) {
        if (programs[i].first == "executable") {
            index = i;
            break;
        }
    }
    if (index == programs.size()) return {0, {}};
    Program &program = programs[index].second;
    const uint32_t begin = program.text.base, end = program.text.end();
    // this player's and everyone's who ran the same code
    std::vector<uint32_t> hinted = hints(title.program_id());
    std::vector<uint32_t> more = bundled(title.program_id(), code_hash(program));
    hinted.insert(hinted.end(), more.begin(), more.end());
    hinted.erase(std::remove_if(hinted.begin(), hinted.end(),
                                [&](uint32_t address) { return !((address & ~1u) >= begin && (address & ~1u) < end); }),
                 hinted.end());
    std::sort(hinted.begin(), hinted.end());
    hinted.erase(std::unique(hinted.begin(), hinted.end()), hinted.end());
    if (hinted.empty()) return {0, hinted};
    std::vector<uint32_t> outside = new_functions(analyses[index], hinted);
    if (!outside.empty()) {
        for (uint32_t address : outside) program.seeds.emplace_back(address, Source::Hint);
        analyses[index] = analyze(program);
    }
    return {outside.size(), hinted};
}

bool generate(const fs::path &rom, const std::function<fs::path(const Title &)> &pick,
              const std::optional<fs::path> &overrides, const Mods &mods, const std::optional<fs::path> &hot,
              bool cold_fast, const Events &events, Generated &out, std::string &error) {
    std::string why;
    auto title = std::make_unique<Title>();
    if (!Title::load(rom, *title, why)) {
        error = "could not load " + utf8(rom) + ", " + why;
        return false;
    }
    std::vector<std::pair<std::string, Program>> list;
    if (!programs(*title, mods, list, why)) {
        error = "could not read the code, " + why;
        return false;
    }
    fs::path dir = pick(*title);
    std::vector<OverrideFile> files;
    if (overrides && !load_overrides(*overrides, files, error)) return false;
    std::vector<Override> replaced;
    for (const OverrideFile &file : files) replaced.insert(replaced.end(), file.overrides.begin(), file.overrides.end());
    // whatever an override replaces has to be generated too, for it to call
    for (const Override &item : replaced) {
        std::string name = item.module.value_or("executable");
        auto found = std::find_if(list.begin(), list.end(), [&](const auto &p) { return p.first == name; });
        if (found == list.end()) {
            error = "the title has no module called " + name;
            return false;
        }
        found->second.seeds.insert(found->second.seeds.begin(), {item.address, Source::Override});
    }

    std::vector<Analysis> analyses;
    for (const auto &[name, program] : list) analyses.push_back(analyze(program));
    auto [functions, hinted] = apply_hints(*title, list, analyses);
    auto note = [&](std::string text) {
        Event event;
        event.kind = Event::Note;
        event.note = std::move(text);
        events(event);
    };
    if (functions > 0) note(std::to_string(functions) + " of the places Zakuro interpreted start new functions");
    auto index_of = [&](const std::optional<std::string> &module) {
        for (size_t i = 0; i < list.size(); i++) {
            if (module && list[i].first == *module) return i;
        }
        return size_t(0);
    };
    for (const Override &item : replaced) {
        size_t index = index_of(item.module);
        bool thumb = item.address & 1;
        bool found = false;
        for (const auto &[entry, f] : analyses[index].functions) {
            if ((f.mode == Mode::Thumb) == thumb && f.labels.count(item.address & ~1u)) {
                found = true;
                break;
            }
        }
        if (!found) {
            char text[160];
            std::snprintf(text, sizeof text, "no code at 0x%08X for %s to replace, so it cannot call the original",
                          item.address, item.name.c_str());
            note(text);
        }
    }
    // the numbers the functions had in the build before here, kept
    Layout layout;
    if (std::string text; read_text(dir / "numbers.txt", text)) layout.numbers = Layout::read_numbers(text);
    if (hot) {
        hot::Heat heat;
        if (!hot::Heat::load(*hot, heat, error)) return false;
        hot::Selection selection = hot::select(list, analyses, heat, hot::SHARE);
        // what the host was seen to interpret counts as hot too
        size_t executable = 0;
        for (size_t i = 0; i < list.size(); i++) {
            if (list[i].first == "executable") {
                executable = i;
                break;
            }
        }
        size_t learned = 0;
        if (executable < analyses.size()) {
            for (const auto &[entry, function] : analyses[executable].functions) {
                uint32_t thumb = function.mode == Mode::Thumb ? 1 : 0;
                if (selection.keep[executable].count(entry)) continue;
                bool seen = std::any_of(hinted.begin(), hinted.end(), [&](uint32_t at) {
                    return (at & 1) == thumb && function.labels.count(at & ~1u);
                });
                if (seen) {
                    selection.keep[executable].insert(entry);
                    learned++;
                }
            }
        }
        if (cold_fast) {
            note("building all " + std::to_string(selection.of) + " functions, the " + std::to_string(selection.functions) +
                 " hottest and " + std::to_string(learned) + " the game was seen to run optimized");
            layout.hot = selection.keep;
        } else {
            std::map<size_t, std::set<uint32_t>> also;
            for (const Override &item : replaced) also[index_of(item.module)].insert(item.address & ~1u);
            hot::prune(analyses, selection.keep, also);
            char covered[32];
            std::snprintf(covered, sizeof covered, "%.1f", selection.covered * 100.0);
            note("building the " + std::to_string(selection.functions) + " hottest of " + std::to_string(selection.of) +
                 " functions, " + covered + "% of where the game spends its time, and " + std::to_string(learned) +
                 " it was seen to run");
        }
    }
    std::vector<Unit> units;
    for (size_t i = 0; i < list.size(); i++) {
        Unit unit;
        if (i > 0) unit.module = list[i].first;
        unit.program = &list[i].second;
        unit.analysis = &analyses[i];
        units.push_back(unit);
    }
    std::vector<std::pair<std::string, std::string>> generated = recomp::generate(units, replaced, layout);

    auto write = [&](const fs::path &path, const std::string &contents) {
        std::ofstream file(path, std::ios::binary | std::ios::trunc);
        if (!(file << contents)) {
            error = "could not write " + utf8(path);
            return false;
        }
        return true;
    };
    fs::path folder = files.empty() ? dir : dir / "hand";
    std::error_code ec;
    fs::create_directories(folder, ec);
    if (ec) {
        error = "could not create " + utf8(folder) + ", " + ec.message();
        return false;
    }
    for (const auto &[name, contents] : generated) {
        if (!write(dir / fs::path(std::u8string(name.begin(), name.end())), contents)) return false;
    }
    std::vector<std::string> sources;
    for (const auto &[name, contents] : generated) {
        if (name.size() >= 2 && name.compare(name.size() - 2, 2, ".c") == 0) sources.push_back(name);
    }
    for (const OverrideFile &file : files) {
        std::string name = "hand/" + utf8(file.path.filename());
        if (!write(dir / fs::path(std::u8string(name.begin(), name.end())), file.source)) return false;
        sources.push_back(name);
    }
    size_t bytes = 0;
    for (const auto &[name, contents] : generated) bytes += contents.size();
    Event event;
    event.kind = Event::Generated;
    event.files = generated.size();
    event.bytes = bytes;
    event.overrides = replaced.size();
    events(event);
    out.title = std::move(title);
    out.dir = dir;
    out.sources = std::move(sources);
    return true;
}

bool build(const fs::path &rom, const BuildOptions &options, const Events &events, fs::path &library, std::string &error) {
    Compiler compiler;
    if (!options.c_only && !check_compiler(options.compiler, compiler, error)) return false;
    auto stopped = [&] { return options.cancel && options.cancel->load(); };
    // a hot build works apart from the full one, whose objects it would
    // otherwise replace
    auto pick = [&](const Title &title) {
        fs::path cache = cache_dir(title.program_id());
        if (options.hot) {
            char name[40];
            std::snprintf(name, sizeof name, "%016llX-hot", (unsigned long long)title.program_id());
            cache = cache.parent_path() / name;
        }
        return options.dir.value_or(cache);
    };
    Generated generated;
    if (!generate(rom, pick, options.overrides, options.mods, options.hot, options.cold_fast, events, generated, error)) {
        return false;
    }
    if (options.c_only) {
        library = generated.dir;
        return true;
    }
    if (stopped()) {
        error = "stopped";
        return false;
    }
    uint64_t program_id = generated.title->program_id();
    library = generated.dir / library_name(program_id);
    auto start = std::chrono::steady_clock::now();
    Progress progress = [&](size_t done, size_t total) {
        Event event;
        event.kind = Event::Compiled;
        event.done = done;
        event.total = total;
        events(event);
        return !stopped();
    };
    // linked beside it, then put in its place: the library being played
    // can't be written over on Windows, but can be moved aside
    fs::path fresh = fs::path(library).replace_extension(std::string(DLL_EXTENSION) + ".new");
    const bool cold_fast = options.cold_fast;
    auto quick = [cold_fast](const std::string &source) { return cold_fast && source.rfind("code_", 0) == 0; };
    if (!compile(compiler, generated.dir, generated.sources, fresh, progress, options.background, quick, error)) return false;
    std::error_code ec;
    if (step_aside(library, ec)) fs::rename(fresh, library, ec);
    if (ec) {
        error = "could not put " + utf8(library) + " in place, " + ec.message();
        return false;
    }
    Event built;
    built.kind = Event::Built;
    built.library = library;
    built.took = std::chrono::steady_clock::now() - start;
    events(built);
    if (options.dir) return true;
    fs::path installed;
    if (!install(library, program_id, installed, error)) return false;
    Event done;
    done.kind = Event::Installed;
    done.library = installed;
    events(done);
    library = installed;
    return true;
}

} // namespace recomp
