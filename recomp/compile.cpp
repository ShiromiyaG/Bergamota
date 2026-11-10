// Compiling the generated C (compile.h). Translated from 3dsrecomp's
// compile.rs (Rust, MIT, Copyright (c) 2026 fearkov; see LICENSE-3dsrecomp).

#include "compile.h"

#include <cstring>
#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <mutex>
#include <sstream>
#include <thread>

#include "subprocess.h"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#endif

namespace fs = std::filesystem;

namespace recomp {

namespace {

// floating point has to round exactly the way the interpreter does, so
// nothing may be fused into a multiply-add
const std::vector<std::string> FLAGS = {"-ffp-contract=off", "-fno-math-errno", "-w"};
// how much each file is optimized: all of it, but code that nearly never
// runs compiles three times faster without (a fast compile's cold files)
const char *const OPTIMIZED = "-O2";
const char *const QUICK = "-O0";
// what only matters for ELF and Mach-O, and that compilers for Windows
// refuse or ignore
#ifdef _WIN32
const std::vector<std::string> UNIX_FLAGS = {};
#else
const std::vector<std::string> UNIX_FLAGS = {"-fPIC", "-fvisibility=hidden"};
#endif
// gcc collects its garbage far less often on a machine with a lot of
// memory, and with that a file of generated code took 7.8 GB at its peak
// instead of 4.8
const std::vector<std::string> GCC_FLAGS = {"--param", "ggc-min-expand=20", "--param", "ggc-min-heapsize=65536"};
// about how many bytes a compiler takes at its peak for each byte of C it
// compiles: gcc keeping a whole file in memory took around 450 for one of
// 2.9 MB
constexpr uint64_t MEMORY_PER_BYTE = 600;
// the same for clang (and zig cc): all of Pokémon Ultra Sun twelve at a time
// took 132 at the median and 191 at the 95th percentile
constexpr uint64_t CLANG_MEMORY_PER_BYTE = 200;
// what to leave of the memory there is for everything else
constexpr uint64_t RESERVE = 1ull << 30;

fs::path path_of(const std::string &utf8);

std::string read_text(const fs::path &path, bool &ok) {
    std::ifstream in(path, std::ios::binary);
    ok = bool(in);
    return ok ? std::string(std::istreambuf_iterator<char>(in), {}) : std::string();
}

Run command_of(const Compiler &compiler) {
    Run run;
    run.program = compiler.program;
    run.args = compiler.leading;
    return run;
}

// the memory the system has to spare, MemAvailable on Linux and the free
// physical memory on Windows, or none when it can't tell
std::optional<uint64_t> spare_memory() {
#ifdef _WIN32
    MEMORYSTATUSEX status{};
    status.dwLength = sizeof status;
    if (GlobalMemoryStatusEx(&status)) return status.ullAvailPhys;
    return std::nullopt;
#elif defined(__linux__)
    std::ifstream info("/proc/meminfo");
    for (std::string line; std::getline(info, line);) {
        if (line.rfind("MemAvailable:", 0) != 0) continue;
        std::istringstream words(line);
        std::string name;
        uint64_t kib = 0;
        if (words >> name >> kib) return kib * 1024;
    }
    return std::nullopt;
#else
    return std::nullopt;
#endif
}

std::vector<std::string> joined(std::initializer_list<const std::vector<std::string> *> lists) {
    std::vector<std::string> out;
    for (auto *list : lists) out.insert(out.end(), list->begin(), list->end());
    return out;
}

std::string hex16(uint64_t value) {
    char text[17];
    std::snprintf(text, sizeof text, "%016llx", (unsigned long long)value);
    return text;
}

// has the command run in the folder objects are all in and gives their
// names from there, their whole paths when they are not in one: a command
// line on Windows holds 32767 characters, which the whole paths of 600
// objects pass
std::vector<std::string> from_folder(Run &command, const std::vector<fs::path> &objects) {
    std::vector<std::string> out;
    fs::path folder = objects.empty() ? fs::path() : objects.front().parent_path();
    bool shared_folder = !folder.empty() && std::all_of(objects.begin(), objects.end(),
                                                        [&](const fs::path &o) { return o.parent_path() == folder; });
    if (shared_folder) {
        command.dir = folder;
        for (const fs::path &object : objects) {
            auto name = object.filename().u8string();
            out.emplace_back(name.begin(), name.end());
        }
    } else {
        for (const fs::path &object : objects) {
            auto name = object.u8string();
            out.emplace_back(name.begin(), name.end());
        }
    }
    return out;
}

fs::path path_of(const std::string &utf8) { return fs::path(std::u8string(utf8.begin(), utf8.end())); }

std::string utf8(const fs::path &path) {
    auto s = path.u8string();
    return {s.begin(), s.end()};
}

} // namespace

uint64_t fnv(uint64_t hash, const void *bytes, size_t size) {
    const uint8_t *p = static_cast<const uint8_t *>(bytes);
    for (size_t i = 0; i < size; i++) hash = (hash ^ p[i]) * 0x00000100000001B3ull;
    return hash;
}

std::optional<Compiler> Compiler::find() {
    if (const char *cc = std::getenv("CC")) {
        std::string named_value = cc;
        bool blank = named_value.find_first_not_of(" \t\r\n") == std::string::npos;
        if (!blank) return named(named_value);
    }
#ifdef _WIN32
    const char *compilers[] = {"gcc", "clang", "cc"};
#else
    const char *compilers[] = {"cc", "gcc", "clang"};
#endif
    for (const char *name : compilers) {
        Compiler compiler{name, {}};
        if (compiler.runs()) return compiler;
    }
    return std::nullopt;
}

Compiler Compiler::named(const std::string &value) {
    std::error_code ec;
    if (fs::exists(path_of(value), ec)) return Compiler{path_of(value), {}};
    std::istringstream words(value);
    std::string program;
    if (!(words >> program)) return Compiler{path_of(value), {}};
    Compiler compiler{path_of(program), {}};
    for (std::string word; words >> word;) compiler.leading.push_back(word);
    return compiler;
}

bool Compiler::runs() const {
    Run run = command_of(*this);
    run.args.push_back("--version");
    run.quiet = true;
    return recomp::run(run).success();
}

bool Compiler::is_gcc() const {
    Run run = command_of(*this);
    run.args.push_back("--version");
    run.capture = true;
    Ran ran = recomp::run(run);
    return ran.started && ran.output.find("Free Software Foundation") != std::string::npos;
}

std::string Compiler::describe() const {
    std::string out = program.string();
    for (const std::string &arg : leading) out += " " + arg;
    return out;
}

bool check_compiler(const std::optional<Compiler> &given, Compiler &out, std::string &error) {
    std::optional<Compiler> compiler = given ? given : Compiler::find();
    if (compiler && compiler->runs()) {
        out = *compiler;
        return true;
    }
#ifdef _WIN32
    std::string name = compiler ? compiler->describe() : "gcc";
    const char *suggestion = "install MinGW-w64's gcc, from MSYS2 or WinLibs, or LLVM's clang";
#else
    std::string name = compiler ? compiler->describe() : "cc";
    const char *suggestion = "install one such as gcc or clang";
#endif
    error = "there is no C compiler (" + name + "), " + suggestion + ", or name it in CC";
    return false;
}

bool objects(const Compiler &compiler, const fs::path &dir, const std::vector<std::string> &sources,
             const Progress &progress, bool background, const std::function<bool(const std::string &)> &quick,
             std::vector<fs::path> &out, std::string &error) {
    const bool gcc = compiler.is_gcc();
    size_t cores = std::max<size_t>(std::thread::hardware_concurrency(), 1);
    if (!std::thread::hardware_concurrency()) cores = 4;
    const size_t jobs = background ? std::max<size_t>(cores > 0 ? cores - 1 : 0, 1) : cores;
    std::optional<uint64_t> budget;
    if (auto spare = spare_memory()) budget = *spare > RESERVE ? *spare - RESERVE : 0;
    const uint64_t per_byte = gcc ? MEMORY_PER_BYTE : CLANG_MEMORY_PER_BYTE;
    // what every object depends on besides its own C: the compiler, which
    // says what it is when asked its version, the flags and recomp.h. the
    // other headers only declare functions, all alike
    Run version_run = command_of(compiler);
    version_run.args.push_back("--version");
    version_run.capture = true;
    Ran version = run(version_run);
    bool read_ok = false;
    std::string header = read_text(dir / "recomp.h", read_ok);
    static const std::vector<std::string> none;
    const std::vector<std::string> flag_list = joined({&FLAGS, gcc ? &GCC_FLAGS : &none, &UNIX_FLAGS});
    std::string flags;
    for (size_t i = 0; i < flag_list.size(); i++) flags += (i ? " " : "") + flag_list[i];
    const std::string described = compiler.describe();
    uint64_t base = FNV_OFFSET;
    base = fnv(base, described.data(), described.size());
    base = fnv(base, version.output.data(), version.started ? version.output.size() : 0);
    base = fnv(base, flags.data(), flags.size());
    base = fnv(base, header.data(), header.size());
    std::vector<uint64_t> needs;
    for (const std::string &source : sources) {
        std::error_code ec;
        uint64_t size = fs::file_size(dir / path_of(source), ec);
        needs.push_back((ec ? 0 : size) * per_byte);
    }
    // the biggest first, so that none of them starts last and holds the
    // others up (a stable sort, as the Rust's)
    std::vector<size_t> order(sources.size());
    for (size_t i = 0; i < order.size(); i++) order[i] = i;
    std::stable_sort(order.begin(), order.end(), [&](size_t a, size_t b) { return needs[a] > needs[b]; });

    // the next of order to start, and the memory and compilers in use
    std::mutex lock;
    std::condition_variable freed;
    size_t next = 0, running = 0;
    uint64_t using_memory = 0;
    std::atomic<size_t> done{0};
    std::atomic<bool> stopped{false};
    std::vector<std::string> failures;
    auto worker = [&] {
        for (;;) {
            size_t index;
            {
                std::unique_lock guard(lock);
                for (;;) {
                    if (stopped.load() || next == order.size()) return;
                    index = order[next];
                    if (running == 0 || !budget || using_memory + needs[index] <= *budget) {
                        next++;
                        using_memory += needs[index];
                        running++;
                        break;
                    }
                    freed.wait(guard);
                }
            }
            const std::string &source = sources[index];
            fs::path path = dir / path_of(source);
            // an object compiled before from the same C, with the same
            // compiler and flags, is kept
            const char *level = quick(source) ? QUICK : OPTIMIZED;
            bool have = false;
            std::string bytes = read_text(path, have);
            std::optional<std::string> key;
            if (have) key = hex16(fnv(fnv(base, level, std::strlen(level)), bytes.data(), bytes.size()));
            fs::path key_path = fs::path(path).replace_extension("key");
            fs::path object = fs::path(path).replace_extension("o");
            bool kept = false;
            if (key) {
                std::error_code ec;
                bool was_ok = false;
                std::string was = read_text(key_path, was_ok);
                kept = fs::is_regular_file(object, ec) && was_ok && was == *key;
            }
            auto finish = [&] {
                if (progress && !progress(done.fetch_add(1) + 1, sources.size())) stopped = true;
                std::lock_guard guard(lock);
                using_memory -= needs[index];
                running--;
                freed.notify_all();
            };
            if (kept) {
                finish();
                continue;
            }
            // a key is only there for an object that finished
            std::error_code ec;
            fs::remove(key_path, ec);
            Run command = command_of(compiler);
            command.lower = background;
            command.args.push_back(level);
            for (const std::string &flag : flag_list) command.args.push_back(flag);
            command.args.push_back("-I");
            command.args.push_back(utf8(dir));
            command.args.push_back("-c");
            command.args.push_back(utf8(path));
            command.args.push_back("-o");
            command.args.push_back(utf8(object));
            if (!run(command).success()) {
                std::lock_guard guard(lock);
                failures.push_back(source);
            } else if (key) {
                std::ofstream(key_path, std::ios::binary) << *key;
            }
            finish();
        }
    };
    std::vector<std::thread> threads;
    for (size_t i = 0; i < jobs; i++) threads.emplace_back(worker);
    for (std::thread &thread : threads) thread.join();
    if (stopped) {
        error = "stopped";
        return false;
    }
    if (!failures.empty()) {
        error = std::to_string(failures.size()) + " failed to compile,";
        for (const std::string &failure : failures) error += " " + failure;
        return false;
    }
    out.clear();
    for (const std::string &source : sources) out.push_back(fs::path(dir / path_of(source)).replace_extension("o"));
    return true;
}

bool shared(const Compiler &compiler, const std::vector<fs::path> &objects, const fs::path &library, std::string &error) {
    Run command = command_of(compiler);
#ifdef _WIN32
    if (compiler.is_gcc()) command.args.push_back("-static-libgcc");
#endif
    std::error_code ec;
    fs::path absolute = fs::absolute(library, ec);
    if (ec) {
        error = "linking failed, " + ec.message();
        return false;
    }
    std::vector<std::string> names = from_folder(command, objects);
    command.args.push_back("-shared");
    command.args.push_back("-o");
    command.args.push_back(utf8(absolute));
    command.args.insert(command.args.end(), names.begin(), names.end());
    Ran ran = run(command);
    if (!ran.success()) {
        error = "linking failed, " + describe_status(ran);
        return false;
    }
    return true;
}

bool compile(const Compiler &compiler, const fs::path &dir, const std::vector<std::string> &sources,
             const fs::path &library, const Progress &progress, bool background,
             const std::function<bool(const std::string &)> &quick, std::string &error) {
    std::vector<fs::path> made;
    return objects(compiler, dir, sources, progress, background, quick, made, error) &&
           shared(compiler, made, library, error);
}

} // namespace recomp
