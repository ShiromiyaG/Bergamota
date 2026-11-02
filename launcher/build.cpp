#include "build.h"

#include "runtime/dump.h"

#include <SDL3/SDL.h>

#include <chrono>
#include <cstring>
#include <fstream>

namespace launcher {

namespace {

constexpr const char *ZIG = "zig-x86_64-windows-0.17.0";
constexpr const char *ZIG_URL = "https://ziglang.org/download/0.17.0/zig-x86_64-windows-0.17.0.zip";
// from ziglang.org/download/index.json
constexpr const char *ZIG_SHA256 = "b5663f69581dcf391293fbf16c06cb80d81d806545ce618b4d0bab7f0eb8c428";
constexpr uint64_t ZIG_SIZE = 100268137;

const bool WINDOWS = SDL_strcmp(SDL_GetPlatform(), "Windows") == 0;

std::string program(const std::string &name) { return WINDOWS ? name + ".exe" : name; }

// a process started from args, its output read when pipe; background ones
// show no console window
SDL_Process *spawn(const std::vector<std::string> &args, SDL_Environment *env, bool pipe) {
    std::vector<const char *> argv;
    for (const std::string &a : args) argv.push_back(a.c_str());
    argv.push_back(nullptr);
    SDL_PropertiesID props = SDL_CreateProperties();
    SDL_SetPointerProperty(props, SDL_PROP_PROCESS_CREATE_ARGS_POINTER, argv.data());
    if (env) SDL_SetPointerProperty(props, SDL_PROP_PROCESS_CREATE_ENVIRONMENT_POINTER, env);
    SDL_SetBooleanProperty(props, SDL_PROP_PROCESS_CREATE_BACKGROUND_BOOLEAN, true);
    if (pipe) {
        SDL_SetNumberProperty(props, SDL_PROP_PROCESS_CREATE_STDOUT_NUMBER, SDL_PROCESS_STDIO_APP);
        SDL_SetBooleanProperty(props, SDL_PROP_PROCESS_CREATE_STDERR_TO_STDOUT_BOOLEAN, true);
    }
    SDL_Process *process = SDL_CreateProcessWithProperties(props);
    SDL_DestroyProperties(props);
    return process;
}

// whether clang is here and links a DLL
bool clang_links() {
    fs::path dir = fs::temp_directory_path() / ("launcher-probe-" + std::to_string(SDL_GetTicksNS()));
    std::error_code ec;
    fs::create_directories(dir, ec);
    {
        std::ofstream source(dir / "probe.c");
        source << "__declspec(dllexport) int probe(void) { return 1; }\n";
    }
    bool links = false;
    if (SDL_Process *p = spawn({"clang", "-shared", "-o", utf8(dir / "probe.dll"), utf8(dir / "probe.c")}, nullptr, false)) {
        SDL_WaitProcess(p, true, nullptr);
        SDL_DestroyProcess(p);
        links = fs::is_regular_file(dir / "probe.dll", ec);
    }
    fs::remove_all(dir, ec);
    return links;
}

// SHA-256, small: the one file it checks is Zig's
struct Sha256 {
    uint32_t h[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
    uint8_t block[64];
    size_t used = 0;
    uint64_t length = 0;

    static uint32_t rotr(uint32_t x, int n) { return x >> n | x << (32 - n); }

    void compress() {
        static const uint32_t k[64] = {
            0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5, 0xd807aa98, 0x12835b01,
            0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc,
            0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da, 0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147,
            0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
            0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070, 0x19a4c116, 0x1e376c08,
            0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208,
            0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};
        uint32_t w[64];
        for (int i = 0; i < 16; i++) w[i] = uint32_t(block[i * 4]) << 24 | block[i * 4 + 1] << 16 | block[i * 4 + 2] << 8 | block[i * 4 + 3];
        for (int i = 16; i < 64; i++) {
            uint32_t s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ w[i - 15] >> 3;
            uint32_t s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ w[i - 2] >> 10;
            w[i] = w[i - 16] + s0 + w[i - 7] + s1;
        }
        uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4], f = h[5], g = h[6], hh = h[7];
        for (int i = 0; i < 64; i++) {
            uint32_t t1 = hh + (rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25)) + ((e & f) ^ (~e & g)) + k[i] + w[i];
            uint32_t t2 = (rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22)) + ((a & b) ^ (a & c) ^ (b & c));
            hh = g, g = f, f = e, e = d + t1, d = c, c = b, b = a, a = t1 + t2;
        }
        h[0] += a, h[1] += b, h[2] += c, h[3] += d, h[4] += e, h[5] += f, h[6] += g, h[7] += hh;
    }

    void update(const uint8_t *data, size_t size) {
        length += size;
        while (size) {
            size_t n = std::min(size, 64 - used);
            std::memcpy(block + used, data, n);
            used += n, data += n, size -= n;
            if (used == 64) compress(), used = 0;
        }
    }

    std::string finish() {
        const uint64_t bits = length * 8;
        const uint8_t one = 0x80, zero = 0;
        update(&one, 1);
        while (used != 56) update(&zero, 1);
        for (int i = 7; i >= 0; i--) {
            uint8_t byte = uint8_t(bits >> (i * 8));
            update(&byte, 1);
        }
        std::string hex;
        char digits[9];
        for (uint32_t v : h) std::snprintf(digits, sizeof digits, "%08x", v), hex += digits;
        return hex;
    }
};

} // namespace

std::string sha256(const fs::path &path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return {};
    Sha256 sum;
    std::vector<char> buffer(1 << 20);
    while (in.read(buffer.data(), std::streamsize(buffer.size())) || in.gcount())
        sum.update(reinterpret_cast<const uint8_t *>(buffer.data()), size_t(in.gcount()));
    return sum.finish();
}

float Stage::fraction() const {
    switch (kind) {
    case Starting: return 0;
    case Downloading: return float(done) / float(std::max<uint64_t>(total, 1)); // a bar of its own
    case Generating: return 0.02f;
    case Compiling: return 0.05f + 0.85f * float(done) / float(std::max<uint64_t>(total, 1));
    case Linking: return 0.9f;
    default: return 1;
    }
}

std::string Stage::describe() const {
    switch (kind) {
    case Starting: return "Starting…";
    case Downloading:
        if (done >= total) return "Unpacking the C compiler (Zig)…";
        return "Downloading the C compiler (Zig), " + std::to_string(done >> 20) + " of " + std::to_string(total >> 20) + " MB";
    case Generating: return "Finding the game's code and writing it as C…";
    case Compiling: return "Compiling, " + std::to_string(done) + " of " + std::to_string(total) + " files";
    case Linking: return "Linking the library…";
    case Done: return "Done.";
    case Failed: return "It failed: " + error;
    }
    return {};
}

std::optional<Stage> Stage::after(const std::string &line) {
    unsigned long long done, total;
    if (std::sscanf(line.c_str(), "compiled %llu of %llu", &done, &total) == 2) {
        Stage s;
        s.kind = done == total ? Linking : Compiling;
        s.done = done, s.total = total;
        return s;
    }
    if (line.rfind("wrote ", 0) == 0 && line.find(" C files") != std::string::npos) return Stage{Generating};
    return std::nullopt;
}

std::vector<fs::path> places(const std::string &name) {
    std::vector<fs::path> found;
    std::string variable = "LAUNCHER_" + upper(name);
    if (const char *named = SDL_GetEnvironmentVariable(SDL_GetEnvironment(), variable.c_str())) found.push_back(path_of(named));
    found.push_back(beside(name) / program(name));
    found.push_back(beside(program(name)));
    return found;
}

std::vector<fs::path> game_places() {
    const Port *p = get_port();
    std::string name = p ? p->player : "player";
    std::vector<fs::path> found = places(name);
    found.insert(found.begin() + std::min<size_t>(found.size(), 1), beside("game") / program(name));
    for (const fs::path &place : places("player")) found.push_back(place);
    return found;
}

std::optional<Tools> Tools::find(const Library &library) {
    std::error_code ec;
    auto first = [&](const fs::path &chosen, const std::vector<fs::path> &candidates) -> fs::path {
        if (!chosen.empty() && fs::is_regular_file(chosen, ec)) return chosen;
        for (const fs::path &p : candidates)
            if (fs::is_regular_file(p, ec)) return p;
        return {};
    };
    Tools tools{first(library.recomp, places("3dsrecomp")), first(library.player, game_places())};
    if (tools.recomp.empty() || tools.player.empty()) return std::nullopt;
    return tools;
}

fs::path hot_list(const std::string &title) {
    fs::path list = folder("hot") / (upper(title) + ".txt");
    std::error_code ec;
    return fs::is_regular_file(list, ec) ? list : fs::path();
}

std::vector<std::string> recomp_args(const Tools &tools, const Config &config, std::string &error) {
    if (config.game.empty()) {
        error = "no game was chosen";
        return {};
    }
    fs::path build = build_dir(config.title);
    if (build.empty()) {
        error = "there is no APPDATA folder to build in";
        return {};
    }
    std::vector<std::string> args{utf8(tools.recomp), "build", utf8(config.game), utf8(build)};
    // the update laid out for 3dsrecomp, which reads no CIA
    if (!config.update.empty()) {
        fs::path dir = update_dir(config.title);
        if (!runtime::unpack_update(config.update, dir, error)) return {};
        args.insert(args.end(), {"--mods", utf8(dir)});
    }
    // the port's code recompiled along, as a module of its own
    if (fs::path decomp = config.decomp_dir(); !decomp.empty()) args.insert(args.end(), {"--decomp", utf8(decomp)});
    return args;
}

bool start(const std::vector<std::string> &args, std::string &error) {
    SDL_Process *p = spawn(args, nullptr, false);
    if (!p) {
        error = "could not start " + args.front() + ", " + SDL_GetError();
        return false;
    }
    SDL_DestroyProcess(p); // it carries on
    return true;
}

std::unique_ptr<Job> Job::start(const Tools &tools, const Config &config, const fs::path &hot, bool cold_fast) {
    std::string error;
    std::vector<std::string> args = recomp_args(tools, config, error);
    if (args.empty()) return failed(error);
    if (!hot.empty()) args.insert(args.end(), {"--hot", utf8(hot)});
    if (cold_fast) args.push_back("--cold-fast");
    std::unique_ptr<Job> job(new Job);
    job->thread_ = std::thread(&Job::work, job.get(), std::move(args));
    return job;
}

std::unique_ptr<Job> Job::failed(const std::string &error) {
    std::unique_ptr<Job> job(new Job);
    job->stage_ = Stage{Stage::Failed, 0, 0, error};
    return job;
}

Job::~Job() {
    if (thread_.joinable()) {
        cancel();
        thread_.join();
    }
}

Stage Job::stage() {
    std::lock_guard<std::mutex> hold(lock_);
    return stage_;
}

void Job::set(const Stage &next) {
    std::lock_guard<std::mutex> hold(lock_);
    if (!stage_.finished()) stage_ = next; // a cancelled job stays so
}

void Job::cancel() {
    cancelled_ = true;
    std::lock_guard<std::mutex> hold(lock_);
    if (process_) SDL_KillProcess(process_, true);
    stage_ = Stage{Stage::Failed, 0, 0, "cancelled"};
}

void Job::wait() {
    if (thread_.joinable()) thread_.join();
}

bool Job::run(const std::vector<std::string> &args, SDL_Environment *env, const std::function<void(const std::string &)> &on_line,
              const std::function<void()> &while_running, std::string &error) {
    SDL_Process *p = spawn(args, env, bool(on_line));
    if (!p) {
        error = "could not start " + args.front() + ", " + SDL_GetError();
        return false;
    }
    {
        std::lock_guard<std::mutex> hold(lock_);
        process_ = p;
        if (cancelled_) SDL_KillProcess(p, true);
    }
    SDL_IOStream *out = on_line ? SDL_GetProcessOutput(p) : nullptr;
    std::string line;
    char buffer[4096];
    for (;;) {
        bool exited = SDL_WaitProcess(p, false, nullptr);
        bool read_any = false;
        while (out) {
            size_t n = SDL_ReadIO(out, buffer, sizeof buffer);
            if (!n) break;
            read_any = true;
            for (size_t i = 0; i < n; i++) {
                if (buffer[i] == '\n' || buffer[i] == '\r') {
                    if (!line.empty()) on_line(line);
                    line.clear();
                } else {
                    line += buffer[i];
                }
            }
        }
        if (exited && !read_any) break;
        if (while_running) while_running();
        if (!read_any) SDL_Delay(100);
    }
    if (!line.empty()) on_line(line);
    {
        std::lock_guard<std::mutex> hold(lock_);
        process_ = nullptr;
    }
    SDL_DestroyProcess(p);
    if (cancelled_) {
        error = "cancelled";
        return false;
    }
    return true;
}

bool Job::prepare_compiler(SDL_Environment *env, std::string &error) {
    const char *named = SDL_GetEnvironmentVariable(env, "CC");
    if (!WINDOWS || (named && *named)) return true;
    if (clang_links()) {
        SDL_SetEnvironmentVariable(env, "CC", "clang", true);
        return true;
    }
    fs::path dir = data_dir() / "zig";
    if (data_dir().empty()) {
        error = "there is no APPDATA folder to keep the compiler in";
        return false;
    }
    std::error_code ec;
    if (!fs::is_regular_file(dir / ZIG / "zig.exe", ec)) {
        // Windows' own curl and tar, the download checked against its SHA-256
        const char *root = SDL_GetEnvironmentVariable(env, "SystemRoot");
        fs::path system = path_of(root ? root : "C:\\Windows") / "System32";
        fs::create_directories(dir, ec);
        fs::path zip = dir / (std::string(ZIG) + ".zip");
        fs::remove(zip, ec);
        auto progress = [&] {
            std::error_code e;
            uint64_t size = fs::exists(zip, e) ? fs::file_size(zip, e) : 0;
            set(Stage{Stage::Downloading, size, ZIG_SIZE});
        };
        if (!run({utf8(system / "curl.exe"), "-L", "-f", "-s", "-o", utf8(zip), ZIG_URL}, nullptr, {}, progress, error)) {
            error = "could not download Zig, " + error;
            return false;
        }
        set(Stage{Stage::Downloading, ZIG_SIZE, ZIG_SIZE});
        if (sha256(zip) != ZIG_SHA256) {
            fs::remove(zip, ec);
            error = "the Zig downloaded is not the one expected (its SHA-256 differs; is the connection all right?)";
            return false;
        }
        // unpacked beside, then moved in, so a half unpacked Zig is never taken for one
        fs::path unpacking = dir / "unpacking";
        fs::remove_all(unpacking, ec);
        fs::create_directories(unpacking, ec);
        if (!run({utf8(system / "tar.exe"), "-xf", utf8(zip), "-C", utf8(unpacking)}, nullptr, {}, {}, error)) {
            error = "could not unpack Zig, " + error;
            return false;
        }
        fs::remove_all(dir / ZIG, ec);
        fs::rename(unpacking / ZIG, dir / ZIG, ec);
        fs::remove_all(unpacking, ec);
        fs::remove(zip, ec);
        if (!fs::is_regular_file(dir / ZIG / "zig.exe", ec)) {
            error = "Zig was not unpacked";
            return false;
        }
    }
    // zig's folder first on the PATH, so CC needs no path, which could hold spaces
    const char *path = SDL_GetEnvironmentVariable(env, "PATH");
    std::string with = utf8(dir / ZIG) + ";" + (path ? path : "");
    SDL_SetEnvironmentVariable(env, "PATH", with.c_str(), true);
    SDL_SetEnvironmentVariable(env, "CC", "zig cc", true);
    return true;
}

void Job::work(std::vector<std::string> args) {
    SDL_Environment *env = SDL_CreateEnvironment(true);
    std::string error;
    if (!prepare_compiler(env, error)) {
        set(Stage{Stage::Failed, 0, 0, error});
        SDL_DestroyEnvironment(env);
        return;
    }
    set(Stage{Stage::Starting});
    // the last error said is kept for a failure
    std::string said;
    bool ran = run(args, env, [&](const std::string &line) {
        if (line.rfind("error", 0) == 0 || line.find(" ERROR ") != std::string::npos) said = line;
        if (auto next = Stage::after(line)) set(*next);
    }, {}, error);
    SDL_DestroyEnvironment(env);
    if (!ran) set(Stage{Stage::Failed, 0, 0, error});
    else if (!said.empty()) set(Stage{Stage::Failed, 0, 0, said});
    else set(Stage{Stage::Done});
}

void build_full(const std::string &title_in) {
    const std::string title = upper(title_in);
    Library library = Library::load();
    auto found = library.games.find(title);
    auto tools = Tools::find(library);
    if (found == library.games.end() || !tools) return;
    const Config config = found->second;
    // one at a time: a lock younger than a day says another is running
    fs::path lock = lock_file(title);
    std::error_code ec;
    if (fs::exists(lock, ec)) {
        auto age = fs::file_time_type::clock::now() - fs::last_write_time(lock, ec);
        if (!ec && age < std::chrono::hours(24)) return;
    }
    {
        std::ofstream out(lock);
        out << "building";
        if (!out) return;
    }
    // a fast compile: the hot code optimized, the rest not
    fs::path hot = hot_list(title);
    bool fast = library.fast && !hot.empty();
    auto job = Job::start(*tools, config, fast ? hot : fs::path(), fast);
    job->wait();
    const bool built = job->stage().kind == Stage::Done;
    // the player may have chosen other files, or the port's code was built
    // again, meanwhile
    Library now = Library::load();
    if (auto it = now.games.find(title); built && it != now.games.end()) {
        Config &c = it->second;
        if (c.game == config.game && c.update == config.update && c.decomp_built == config.decomp_built) {
            c.full = true;
            std::string error;
            now.save(error);
        }
    }
    fs::remove(lock, ec);
}

void finish_in_background(const Config &config) {
    if (config.full) return;
    std::vector<std::string> args{utf8(exe())};
    if (auto named = arg_after("--port")) args.insert(args.end(), {"--port", *named});
    args.insert(args.end(), {"--build-full", config.title});
    std::string error;
    start(args, error);
}

bool play(const Tools &tools, const Config &config, std::string &error) {
    fs::path settings = player_settings(config, error);
    if (settings.empty()) return false;
    Library library = Library::load();
    library.put(config);
    std::string ignored;
    library.save(ignored);
    finish_in_background(config);
    return start({utf8(tools.player), "--config", utf8(settings)}, error);
}

bool desktop_shortcut(std::string &error) {
    auto quote = [](std::string s) {
        for (size_t at = 0; (at = s.find('\'', at)) != std::string::npos; at += 2) s.insert(at, "'");
        return s;
    };
    const std::string target = quote(utf8(exe())), dir = quote(utf8(exe().parent_path())), name = quote(port().name);
    std::string script = "$s = (New-Object -ComObject WScript.Shell).CreateShortcut([Environment]::GetFolderPath('Desktop') + '\\" + name +
                         ".lnk'); $s.TargetPath = '" + target + "'; $s.WorkingDirectory = '" + dir + "'; $s.Save()";
    SDL_Process *p = spawn({"powershell", "-NoProfile", "-NonInteractive", "-Command", script}, nullptr, false);
    if (!p) {
        error = "could not run PowerShell";
        return false;
    }
    SDL_WaitProcess(p, true, nullptr);
    SDL_DestroyProcess(p);
    return true;
}

} // namespace launcher
