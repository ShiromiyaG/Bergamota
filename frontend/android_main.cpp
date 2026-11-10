// Android: the program (libmain.so, whose SDL_main SDL's activity runs). It
// unpacks what the APK carries for the port into the app's own storage the
// first time (and after an update): the port's files (assets/port/, listed
// in files.txt) and Zig's lib folder (assets/zig/lib.tar); sets up what the
// launcher hands its programs (3dsrecomp and Zig, which Android lets run
// only from the APK's library folder, as lib3dsrecomp.so and libzig.so);
// then runs the launcher, and the player when the launcher closes to play.
// The APK is laid out by sdk/android.py (port.py package --android).

#include "android.h"
#include "port.h"

#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>
#include <android/log.h>
#include <sched.h>
#include <jni.h>
#include <sys/system_properties.h>
#include <unistd.h>

#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

int launcher_main(int argc, char **argv);
int player_main(int argc, char **argv);

namespace {

namespace fs = std::filesystem;

// a file of the APK's assets, whole; false when there is none
bool read_asset(const std::string &name, std::string &out) {
    size_t size = 0;
    void *data = SDL_LoadFile(name.c_str(), &size);
    if (!data) return false;
    out.assign(static_cast<const char *>(data), size);
    SDL_free(data);
    return true;
}

bool write_file(const fs::path &path, const char *data, size_t size) {
    std::error_code ec;
    fs::create_directories(path.parent_path(), ec);
    std::ofstream out(path, std::ios::binary);
    out.write(data, std::streamsize(size));
    return bool(out);
}

// the APK's folder of native libraries, where Android lets programs run
std::string native_library_dir() {
    auto *env = static_cast<JNIEnv *>(SDL_GetAndroidJNIEnv());
    auto activity = static_cast<jobject>(SDL_GetAndroidActivity());
    if (!env || !activity) return {};
    std::string dir;
    jclass type = env->GetObjectClass(activity);
    jmethodID info_of = env->GetMethodID(type, "getApplicationInfo", "()Landroid/content/pm/ApplicationInfo;");
    jobject info = info_of ? env->CallObjectMethod(activity, info_of) : nullptr;
    if (info) {
        jclass info_type = env->GetObjectClass(info);
        jfieldID field = env->GetFieldID(info_type, "nativeLibraryDir", "Ljava/lang/String;");
        if (auto text = field ? static_cast<jstring>(env->GetObjectField(info, field)) : nullptr) {
            const char *chars = env->GetStringUTFChars(text, nullptr);
            dir = chars;
            env->ReleaseStringUTFChars(text, chars);
            env->DeleteLocalRef(text);
        }
        env->DeleteLocalRef(info_type);
        env->DeleteLocalRef(info);
    }
    if (env->ExceptionCheck()) env->ExceptionClear();
    env->DeleteLocalRef(type);
    env->DeleteLocalRef(activity);
    return dir;
}

// the assets' stamp against the one unpacked: the same, nothing to do
bool current(const std::string &stamp_asset, const fs::path &stamp_file, std::string &stamp) {
    if (!read_asset(stamp_asset, stamp)) return true; // nothing to unpack
    std::ifstream in(stamp_file, std::ios::binary);
    std::string had((std::istreambuf_iterator<char>(in)), {});
    return had == stamp;
}

// assets/port/ into dir: files.txt names each file, a line each
bool unpack_port(const fs::path &dir) {
    std::string stamp, list;
    if (current("port/stamp", dir / "stamp", stamp)) return true;
    if (!read_asset("port/files.txt", list)) return false;
    std::error_code ec;
    fs::remove_all(dir, ec);
    size_t at = 0;
    while (at < list.size()) {
        size_t end = list.find('\n', at);
        if (end == std::string::npos) end = list.size();
        std::string name = list.substr(at, end - at);
        at = end + 1;
        if (!name.empty() && name.back() == '\r') name.pop_back();
        if (name.empty()) continue;
        std::string data;
        if (!read_asset("port/" + name, data) || !write_file(dir / launcher::path_of(name), data.data(), data.size())) return false;
    }
    return write_file(dir / "stamp", stamp.data(), stamp.size());
}

// a tar (ustar, with GNU's long names) into dir, read as it streams from
// the asset (Zig's lib folder: some thousands of files)
bool unpack_tar(const std::string &asset, const fs::path &dir) {
    SDL_IOStream *io = SDL_IOFromFile(asset.c_str(), "rb");
    if (!io) return false;
    auto read = [&](void *to, size_t size) { return SDL_ReadIO(io, to, size) == size; };
    auto octal = [](const char *field, size_t width) {
        uint64_t value = 0;
        for (size_t i = 0; i < width && field[i]; i++)
            if (field[i] >= '0' && field[i] <= '7') value = value * 8 + uint64_t(field[i] - '0');
        return value;
    };
    std::string long_name;
    std::vector<char> data;
    bool ok = true;
    for (;;) {
        char header[512];
        if (!read(header, sizeof header)) break;
        if (header[0] == 0) break; // the end's empty blocks
        const uint64_t size = octal(header + 124, 12);
        const char type = header[156];
        std::string name;
        if (!long_name.empty()) {
            name = long_name;
            long_name.clear();
        } else {
            std::string prefix(header + 345, strnlen(header + 345, 155)), base(header, strnlen(header, 100));
            name = prefix.empty() ? base : prefix + "/" + base;
        }
        data.resize(size_t((size + 511) / 512 * 512));
        if (!data.empty() && !read(data.data(), data.size())) {
            ok = false;
            break;
        }
        if (type == 'L') {
            long_name.assign(data.data(), strnlen(data.data(), size_t(size)));
        } else if (type == '0' || type == '\0') {
            if (!write_file(dir / launcher::path_of(name), data.data(), size_t(size))) {
                ok = false;
                break;
            }
        } else if (type == '5') {
            std::error_code ec;
            fs::create_directories(dir / launcher::path_of(name), ec);
        }
    }
    SDL_CloseIO(io);
    return ok;
}

bool unpack_zig(const fs::path &dir) {
    std::string stamp;
    if (current("zig/stamp", dir / "stamp", stamp)) return true;
    std::error_code ec;
    fs::remove_all(dir, ec);
    if (!unpack_tar("zig/lib.tar", dir / "lib")) return false;
    return write_file(dir / "stamp", stamp.data(), stamp.size());
}

void set(const char *name, const std::string &value) {
    setenv(name, value.c_str(), 1);
    SDL_SetEnvironmentVariable(SDL_GetEnvironment(), name, value.c_str(), true);
}

// for looking into a phone from a computer: what the program prints or writes to
// stderr goes to Android's log (adb logcat -s Bergamota), and the variables
// the runtime reads (RUNTIME_...) are taken from a system property,
//   adb shell setprop debug.bergamota.env "RUNTIME_STATS=1,RUNTIME_GENERIC_FRAG=1"
// BERGAMOTA_PLAY=1 among them starting the game played last at once
void debug_aids() {
    int ends[2];
    if (pipe(ends) == 0) {
        dup2(ends[1], STDERR_FILENO);
        dup2(ends[1], STDOUT_FILENO);
        setvbuf(stdout, nullptr, _IOLBF, 0);
        close(ends[1]);
        setvbuf(stderr, nullptr, _IOLBF, 0);
        std::thread([in = ends[0]] {
            std::string line;
            char buffer[512];
            for (ssize_t n; (n = read(in, buffer, sizeof buffer)) > 0;) {
                for (ssize_t i = 0; i < n; i++) {
                    if (buffer[i] != '\n') {
                        line += buffer[i];
                        continue;
                    }
                    __android_log_write(ANDROID_LOG_INFO, "Bergamota", line.c_str());
                    line.clear();
                }
            }
        }).detach();
    }
    char value[PROP_VALUE_MAX] = {};
    if (__system_property_get("debug.bergamota.env", value) <= 0) return;
    std::string all = value;
    for (size_t from = 0; from < all.size();) {
        size_t to = all.find(',', from);
        if (to == std::string::npos) to = all.size();
        std::string one = all.substr(from, to - from);
        if (size_t eq = one.find('='); eq != std::string::npos && eq > 0) {
            setenv(one.substr(0, eq).c_str(), one.substr(eq + 1).c_str(), 1);
            SDL_SetEnvironmentVariable(SDL_GetEnvironment(), one.substr(0, eq).c_str(), one.substr(eq + 1).c_str(), true);
            std::fprintf(stderr, "debug.bergamota.env: %s\n", one.c_str());
        }
        from = to + 1;
    }
}

// every core for this thread and the ones it starts (the game's, the
// GPU's, the shaders' workers): a phone hands a game's main thread its two
// big cores alone, and all of them shared those, the six others idle
void every_core() {
    cpu_set_t all;
    CPU_ZERO(&all);
    for (int cpu = 0; cpu < CPU_SETSIZE; cpu++) CPU_SET(cpu, &all);
    sched_setaffinity(0, sizeof all, &all);
}

} // namespace

int main(int argc, char *argv[]) {
    (void)argc, (void)argv;
    every_core();
    debug_aids();
    // landscape, and Android's back button to the program (the player's menu)
    SDL_SetHint(SDL_HINT_ORIENTATIONS, "LandscapeLeft LandscapeRight");
    SDL_SetHint(SDL_HINT_ANDROID_TRAP_BACK_BUTTON, "1");
    const char *storage = SDL_GetAndroidInternalStoragePath();
    const char *cache = SDL_GetAndroidCachePath();
    const std::string libraries = native_library_dir();
    if (!storage || !cache || libraries.empty()) {
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "Bergamota", "The app's folders could not be found.", nullptr);
        return 1;
    }
    const fs::path files = launcher::path_of(storage), port = files / "port", zig = files / "zig";
    if (!unpack_port(port) || !unpack_zig(zig)) {
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "Bergamota",
                                 "The app's files could not be unpacked; is there room on the phone?", nullptr);
        return 1;
    }
    launcher::set_base(port);
    // what the launcher's programs run with: 3dsrecomp and Zig from the APK
    // (Zig with its lib folder unpacked, its caches in the app's), the game
    // built for AArch64 without a C library (recomp/compile.cpp)
    const fs::path cache_dir = launcher::path_of(cache);
    set("LAUNCHER_3DSRECOMP", libraries + "/lib3dsrecomp.so");
    set("CC", libraries + "/libzig.so cc -target aarch64-linux-musl");
    set("ZIG_LIB_DIR", launcher::utf8(zig / "lib"));
    set("ZIG_GLOBAL_CACHE_DIR", launcher::utf8(cache_dir / "zig"));
    set("ZIG_LOCAL_CACHE_DIR", launcher::utf8(cache_dir / "zig-local"));
    set("HOME", launcher::utf8(files));
    set("TMPDIR", launcher::utf8(cache_dir));
    const char *path = getenv("PATH");
    set("PATH", libraries + ":" + (path ? path : "/system/bin"));
    // the player is this program: the launcher's check that it is there
    // finds libmain.so under the player's name
    if (const launcher::Port *p = launcher::get_port()) {
        set(("LAUNCHER_" + launcher::upper(p->player)).c_str(), libraries + "/libmain.so");
    }

    char name[] = "launcher", play[] = "--play";
    char *launcher_argv[] = {name, play, nullptr};
    launcher::play_request().clear();
    int result = launcher_main(getenv("BERGAMOTA_PLAY") ? 2 : 1, launcher_argv);
    if (launcher::play_request().empty()) return result;
    std::vector<std::string> args = launcher::play_request();
    std::vector<char *> player_argv;
    for (std::string &a : args) player_argv.push_back(a.data());
    player_argv.push_back(nullptr);
    return player_main(int(args.size()), player_argv.data());
}
