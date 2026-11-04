// 3dsrecomp, a static recompiler for 3DS titles, whose code runs on the
// runtime (or any host that implements recomp.h). analyze reports how much
// of a title's code a generic pass discovers on its own, build turns what
// it found into C and compiles it into the library the runtime loads.
// Translated from 3dsrecomp's main.rs (Rust, MIT, Copyright (c) 2026
// fearkov; see LICENSE-3dsrecomp), without port and verify, which were
// Zakuro's.

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <set>
#include <string>
#include <vector>

#include "build.h"
#include "codegen/codegen.h"
#include "cro.h"
#include "decomp.h"
#include "discover.h"
#include "programs.h"
#include "rom.h"

namespace fs = std::filesystem;
using namespace recomp;

namespace {

const char *const USAGE = "\
usage, 3dsrecomp analyze <rom>\n\
       3dsrecomp build <rom> [<dir>] [--overrides <file or dir>] [--c-only]\n\
       3dsrecomp hints <rom> [<hints file>]\n\
       3dsrecomp symbols <rom> <dir>\n\
each takes --decomp <dir>, code written apart from the game (decomp.bin and\n\
decomp.txt) that replaces some of its functions, and --mods <dir>, a mod or\n\
an unpacked update laid out the way Luma3DS has them, code.bin (decompressed,\n\
or in exefs/), exheader.bin and romfs/, read in place of the game's own code\n\
and modules. build takes --hot <file>, the heat of a run, to build only the\n\
functions the game spends nearly all its time in, in a fraction of the time,\n\
with those the host was seen to interpret (its hints); with --cold-fast as\n\
well, build all of it, the rest compiled without optimizing, three times\n\
faster. --c-only writes the C and compiles nothing.\n\
\n\
symbols writes, for each program, the names the title's modules and its\n\
static.crs export and the functions the analysis finds, for a disassembler.\n\
\n\
build without a dir works in the cache and installs the library where the\n\
runtime finds it on its own. hints prints the game's section for hints.txt,\n\
from the hints the runtime wrote next to its library or the file given.";

std::string utf8(const fs::path &path) {
    auto s = path.u8string();
    return {s.begin(), s.end()};
}

fs::path path_of(const std::string &text) { return fs::path(std::u8string(text.begin(), text.end())); }

std::optional<std::vector<uint8_t>> read_file(const fs::path &path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return std::nullopt;
    return std::vector<uint8_t>(std::istreambuf_iterator<char>(in), {});
}

// a mod, or an update unpacked, laid out the way Luma3DS reads them:
// code.bin, decompressed, in it or in exefs/, exheader.bin, and the RomFS
// files it replaces under romfs/
struct ModDir {
    std::optional<std::vector<uint8_t>> code, exheader;
    std::optional<fs::path> romfs;

    static ModDir read(const fs::path &dir) {
        ModDir mod;
        for (const char *name : {"code.bin", "exefs/code.bin"}) {
            if ((mod.code = read_file(dir / name))) break;
        }
        mod.exheader = read_file(dir / "exheader.bin");
        std::error_code ec;
        if (fs::is_directory(dir / "romfs", ec)) mod.romfs = dir / "romfs";
        if (!mod.code && !mod.exheader && !mod.romfs) {
            std::fprintf(stderr, "%s has no code.bin, exheader.bin or romfs\n", utf8(dir).c_str());
            std::exit(1);
        }
        return mod;
    }
};

// what the options on the command line ask for
struct Options {
    std::optional<fs::path> overrides;
    std::optional<ModDir> mods;
    std::optional<Decomp> decomp;
    std::optional<fs::path> hot;
    bool cold_fast = false;
    bool c_only = false;

    // what the mod changes, its RomFS files read from its folder
    Mods to_mods() const {
        Mods out;
        out.decomp = decomp ? &*decomp : nullptr;
        if (!mods) return out;
        out.code = mods->code ? &*mods->code : nullptr;
        out.exheader = mods->exheader ? &*mods->exheader : nullptr;
        if (mods->romfs) {
            fs::path root = *mods->romfs;
            out.romfs = [root](const std::string &path) { return read_file(root / path_of(path)); };
        }
        return out;
    }
};

[[noreturn]] void fail(const std::string &message, int code = 1) {
    std::fprintf(stderr, "%s\n", message.c_str());
    std::exit(code);
}

// the title and its programs, the executable first and then its modules
std::unique_ptr<Title> load(const std::string &path, const Mods &mods, std::vector<std::pair<std::string, Program>> &list) {
    std::string error;
    auto title = std::make_unique<Title>();
    if (!Title::load(path_of(path), *title, error)) fail("could not load " + path + ", " + error);
    if (!programs(*title, mods, list, error)) fail("could not read the code, " + error);
    return title;
}

// Rust's Debug of a Duration, with a precision: the largest unit that
// is not zero, the digits after rounded half up
std::string duration(std::chrono::nanoseconds took, int precision) {
    uint64_t total = uint64_t(std::max<int64_t>(took.count(), 0));
    uint64_t secs = total / 1000000000, nanos = total % 1000000000;
    auto decimal = [&](uint64_t integer, uint32_t fractional, uint32_t divisor, const char *postfix) {
        char buf[9] = {'0', '0', '0', '0', '0', '0', '0', '0', '0'};
        size_t pos = 0, end = size_t(std::min(precision, 9));
        while (fractional > 0 && pos < end) {
            buf[pos] = char('0' + fractional / divisor);
            fractional %= divisor;
            divisor /= 10;
            pos++;
        }
        if (fractional > 0 && fractional >= divisor * 5) {
            bool carry = true;
            for (size_t at = pos; carry && at > 0;) {
                at--;
                if (buf[at] < '9') buf[at]++, carry = false;
                else buf[at] = '0';
            }
            if (carry) integer++;
        }
        std::string out = std::to_string(integer);
        if (end > 0) out += "." + std::string(buf, end) + std::string(size_t(std::max(precision - 9, 0)), '0');
        return out + postfix;
    };
    if (secs > 0) return decimal(secs, uint32_t(nanos), 100000000, "s");
    if (nanos >= 1000000) return decimal(nanos / 1000000, uint32_t(nanos % 1000000), 100000, "ms");
    if (nanos >= 1000) return decimal(nanos / 1000, uint32_t(nanos % 1000), 100, "\xC2\xB5s");
    return decimal(nanos, 0, 1, "ns");
}

// what a build does, compiling every twentieth of the way, which a
// frontend running build can follow
void print_event(const Event &event) {
    switch (event.kind) {
    case Event::Generated:
        std::printf("wrote %zu files, %zu MiB of C, %zu overrides\n", event.files, event.bytes >> 20, event.overrides);
        break;
    case Event::Note: std::printf("%s\n", event.note.c_str()); break;
    case Event::Compiled:
        if (event.done == event.total || event.done * 20 / event.total != (event.done - 1) * 20 / event.total) {
            std::printf("compiled %zu of %zu\n", event.done, event.total);
        }
        break;
    case Event::Built: std::printf("built %s in %s\n", utf8(event.library).c_str(), duration(event.took, 1).c_str()); break;
    case Event::Installed:
        std::printf("installed %s, which the runtime runs for the game from now on\n", utf8(event.library).c_str());
        break;
    }
    std::fflush(stdout);
}

// prints the section of hints.txt for the game at path: the places in the
// hints the host wrote next to its library, or in file, that start
// functions the analysis does not find, along with those the list has
void hints_command(const std::string &path, const std::optional<fs::path> &file, const Options &options) {
    std::vector<std::pair<std::string, Program>> list;
    auto title = load(path, options.to_mods(), list);
    auto found = std::find_if(list.begin(), list.end(), [](const auto &p) { return p.first == "executable"; });
    if (found == list.end()) fail(path + " has no executable");
    const Program &program = found->second;
    std::vector<uint32_t> hinted;
    if (file) {
        auto bytes = read_file(*file);
        if (!bytes) fail("could not read " + utf8(*file));
        hinted = read_hints(std::string(bytes->begin(), bytes->end()));
    } else {
        hinted = hints(title->program_id());
    }
    uint64_t hash = code_hash(program);
    std::vector<uint32_t> more = bundled(title->program_id(), hash);
    hinted.insert(hinted.end(), more.begin(), more.end());
    const uint32_t begin = program.text.base, end = program.text.end();
    hinted.erase(std::remove_if(hinted.begin(), hinted.end(),
                                [&](uint32_t a) { return !((a & ~1u) >= begin && (a & ~1u) < end); }),
                 hinted.end());
    std::sort(hinted.begin(), hinted.end());
    hinted.erase(std::unique(hinted.begin(), hinted.end()), hinted.end());
    std::vector<uint32_t> fresh = new_functions(analyze(program), hinted);
    std::printf("= %016llX %016llX\n", (unsigned long long)title->program_id(), (unsigned long long)hash);
    for (uint32_t address : fresh) std::printf("%08X\n", address);
}

// writes dir/<program>.txt for each program: a line for each name its CRO,
// or static.crs for the executable, exports, and one for each function the
// analysis finds that has none. "name address kind", kind f for an ARM
// function, t for a Thumb one and l for data
void symbols_command(const std::string &path, const fs::path &dir, const Options &options) {
    Mods mods = options.to_mods();
    std::vector<std::pair<std::string, Program>> list;
    auto title = load(path, mods, list);
    auto modules = module_files(*title, mods);
    auto crs = static_module(*title, mods);
    std::error_code ec;
    fs::create_directories(dir, ec);
    if (ec) fail("could not create " + utf8(dir) + ", " + ec.message());
    size_t named = 0, found = 0;
    for (const auto &[name, program] : list) {
        const cro::Module *exporter = nullptr;
        if (name == "executable") {
            exporter = crs ? &*crs : nullptr;
        } else {
            for (const auto &[module, bytes] : modules) {
                if (module.name == name) {
                    exporter = &module;
                    break;
                }
            }
        }
        Analysis analysis = analyze(program);
        auto thumb = [&](uint32_t at) {
            if (at & 1) return true;
            auto f = analysis.functions.find(at & ~1u);
            return f != analysis.functions.end() && f->second.mode == Mode::Thumb;
        };
        std::string text = "# " + name + ": name address kind (f ARM function, t Thumb function, l data)\n";
        std::set<uint32_t> has_name;
        if (exporter) {
            for (const auto &[symbol, tag] : exporter->exports) {
                uint32_t at;
                char kind;
                if (auto code = exporter->code_address(tag)) {
                    at = *code & ~1u;
                    kind = thumb(*code) ? 't' : 'f';
                } else if (auto data = exporter->resolve(tag)) {
                    at = *data;
                    kind = 'l';
                } else {
                    continue;
                }
                char line[16];
                std::snprintf(line, sizeof line, " %08X %c\n", at, kind);
                text += symbol + line;
                has_name.insert(at);
                named++;
            }
        }
        for (const auto &[entry, function] : analysis.functions) {
            if (has_name.insert(entry).second) {
                char kind = function.mode == Mode::Thumb ? 't' : 'f';
                char line[48];
                std::snprintf(line, sizeof line, "%c_%08X %08X %c\n", kind, entry, entry, kind);
                text += line;
                found++;
            }
        }
        fs::path out = dir / path_of(name + ".txt");
        std::ofstream file(out, std::ios::binary | std::ios::trunc);
        if (!(file << text)) fail("could not write " + utf8(out));
    }
    std::printf("%zu programs, %zu exported names, %zu functions found without one, in %s\n", list.size(), named, found,
                utf8(dir).c_str());
}

// builds the library in dir, or when there is none in the cache, and then
// installs it where hosts look for it
void build_command(const std::string &path, const std::optional<fs::path> &dir, const Options &options) {
    BuildOptions build_options;
    build_options.dir = dir;
    build_options.overrides = options.overrides;
    build_options.mods = options.to_mods();
    build_options.hot = options.hot;
    build_options.cold_fast = options.cold_fast;
    build_options.c_only = options.c_only;
    fs::path library;
    std::string error;
    if (!build(path_of(path), build_options, print_event, library, error)) fail(error);
}

// the numbers the report shows for a program, or for several added up
struct Totals {
    size_t text = 0, functions = 0, code = 0, literal = 0, indirect = 0, switches = 0, svc = 0, dead_ends = 0;

    static Totals of(const Analysis &analysis) {
        return Totals{analysis.map.size(),     analysis.functions.size(), analysis.count(Byte::Code),
                      analysis.count(Byte::Literal), analysis.indirect_sites, analysis.jump_tables,
                      analysis.svc_sites,      analysis.dead_ends};
    }
    Totals add(const Totals &o) const {
        return Totals{text + o.text,         functions + o.functions, code + o.code, literal + o.literal,
                      indirect + o.indirect, switches + o.switches,   svc + o.svc,   dead_ends + o.dead_ends};
    }
    void print(const char *name) const {
        auto percent = [&](size_t bytes) { return double(bytes) * 100.0 / double(std::max<size_t>(text, 1)); };
        size_t unreached = text - code - literal;
        std::printf("%-12s %9zu %10zu %5.1f%% %8.1f%% %9.1f%% %9zu %9zu %6zu %10zu\n", name, text / 1024, functions,
                    percent(code), percent(literal), percent(unreached), indirect, switches, svc, dead_ends);
    }
};

void analyze_command(const std::string &path, const Options &options) {
    std::vector<std::pair<std::string, Program>> list;
    auto title = load(path, options.to_mods(), list);
    size_t module_count = list.size() - 1;
    auto start = std::chrono::steady_clock::now();
    std::vector<Analysis> analyses;
    for (const auto &[name, program] : list) analyses.push_back(analyze(program));
    auto [functions, hinted] = apply_hints(*title, list, analyses);
    auto elapsed = std::chrono::steady_clock::now() - start;

    std::printf("title       %s, %zu modules\n", title->exheader.title.c_str(), module_count);
    if (functions > 0) std::printf("hints       %zu new functions, from where the host interpreted\n", functions);
    std::printf("\n");
    std::printf("                  KiB  functions   code  literals  unreached  indirect  switches    svc  dead ends\n");
    Totals executable = Totals::of(analyses[0]);
    Totals modules;
    for (size_t i = 1; i < analyses.size(); i++) modules = modules.add(Totals::of(analyses[i]));
    executable.print("executable");
    modules.print("modules");
    executable.add(modules).print("all");
    std::printf("\n");

    auto from = [&](Source source) {
        size_t n = 0;
        for (const Analysis &a : analyses)
            for (const auto &[entry, f] : a.functions) n += f.source == source;
        return n;
    };
    std::printf("found by    %zu calls, %zu pointers, %zu relocations, %zu exports, %zu imports, %zu scanned, %zu hinted, %zu entry\n",
                from(Source::Call), from(Source::Pointer), from(Source::Relocation), from(Source::Export),
                from(Source::Import), from(Source::Scan), from(Source::Hint), from(Source::Entry));
    size_t thumb = 0, instructions = 0;
    std::vector<std::pair<size_t, uint32_t>> largest;
    for (const Analysis &a : analyses) {
        for (const auto &[entry, f] : a.functions) {
            thumb += f.mode == Mode::Thumb;
            instructions += f.instructions.size();
            largest.emplace_back(f.instructions.size(), entry);
        }
    }
    std::printf("thumb       %zu functions\n", thumb);
    std::printf("decoded     %zu instructions, shared code once per function\n", instructions);
    // a function far bigger than the rest takes the compiler most of the
    // build, alone
    std::sort(largest.begin(), largest.end(), [](const auto &a, const auto &b) { return b < a; });
    std::string top;
    for (size_t i = 0; i < largest.size() && i < 3; i++) {
        char item[48];
        std::snprintf(item, sizeof item, "%s%zu at %08X", i ? ", " : "", largest[i].first, largest[i].second);
        top += item;
    }
    std::printf("largest     %s\n", top.c_str());
    std::printf("analysis    %s\n", duration(std::chrono::duration_cast<std::chrono::nanoseconds>(elapsed), 2).c_str());
    std::printf("\n");

    std::printf("largest unreached runs\n");
    struct Gap {
        const std::string *name;
        uint32_t start;
        size_t length;
    };
    std::vector<Gap> gaps;
    for (size_t i = 0; i < list.size(); i++) {
        for (auto [start_at, length] : analyses[i].largest_gaps(list[i].second.text.base, 8)) {
            gaps.push_back(Gap{&list[i].first, start_at, length});
        }
    }
    std::stable_sort(gaps.begin(), gaps.end(), [](const Gap &a, const Gap &b) { return a.length > b.length; });
    for (size_t i = 0; i < gaps.size() && i < 10; i++) {
        std::printf("  %-28s 0x%08X  %zu bytes\n", gaps[i].name->c_str(), gaps[i].start, gaps[i].length);
    }
}

} // namespace

int main(int argc, char **argv) {
    std::vector<std::string> words;
    Options options;
    for (int i = 1; i < argc; i++) {
        std::string arg = argv[i];
        auto value = [&]() -> std::string {
            if (i + 1 >= argc) fail(arg + " needs a value", 2);
            return argv[++i];
        };
        if (arg == "--overrides") {
            options.overrides = path_of(value());
        } else if (arg == "--mods") {
            options.mods = ModDir::read(path_of(value()));
        } else if (arg == "--decomp") {
            Decomp decomp;
            std::string error;
            if (!Decomp::read(path_of(value()), decomp, error)) fail(error, 2);
            options.decomp = std::move(decomp);
        } else if (arg == "--hot") {
            options.hot = path_of(value());
        } else if (arg == "--cold-fast") {
            options.cold_fast = true;
        } else if (arg == "--c-only") {
            options.c_only = true;
        } else if (arg.rfind("--", 0) == 0) {
            fail("unknown option " + arg + "\n" + USAGE, 2);
        } else {
            words.push_back(arg);
        }
    }
    const size_t n = words.size();
    const std::string command = n ? words[0] : "";
    if (command == "analyze" && n == 2) analyze_command(words[1], options);
    else if (command == "build" && n == 2) build_command(words[1], std::nullopt, options);
    else if (command == "build" && n == 3) build_command(words[1], path_of(words[2]), options);
    else if (command == "symbols" && n == 3) symbols_command(words[1], path_of(words[2]), options);
    else if (command == "hints" && n == 2) hints_command(words[1], std::nullopt, options);
    else if (command == "hints" && n == 3) hints_command(words[1], path_of(words[2]), options);
    else fail(USAGE, 2);
    return 0;
}
