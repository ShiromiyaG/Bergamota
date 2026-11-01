// Sampling where a thread spends its time (include/runtime/sampler.h).

#include "runtime/sampler.h"
#include "runtime/loader.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <unordered_map>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <dbghelp.h>
#include <timeapi.h>
#endif

namespace runtime {

Sampler::Sampler() = default;

Sampler::~Sampler() { stop(); }

void Sampler::start_here() {
#ifdef _WIN32
    HANDLE self = nullptr;
    DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &self,
                    THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, 0);
    thread_ = self;
    stop_ = false;
    sampler_ = std::thread([this] { run(); });
#endif
}

void Sampler::start(void *thread) {
#ifdef _WIN32
    HANDLE copy = nullptr;
    DuplicateHandle(GetCurrentProcess(), thread, GetCurrentProcess(), &copy,
                    THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, 0);
    thread_ = copy;
    stop_ = false;
    sampler_ = std::thread([this] { run(); });
#else
    (void)thread;
#endif
}

void Sampler::stop() {
    stop_ = true;
    if (sampler_.joinable()) sampler_.join();
#ifdef _WIN32
    if (thread_) CloseHandle(thread_), thread_ = nullptr;
#endif
}

void Sampler::run() {
#ifdef _WIN32
    timeBeginPeriod(1);
    while (!stop_) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
        if (SuspendThread(thread_) == DWORD(-1)) continue;
        CONTEXT c{};
        c.ContextFlags = CONTEXT_CONTROL;
        if (GetThreadContext(thread_, &c)) {
#if defined(_M_X64) || defined(__x86_64__)
            hits_[uintptr_t(c.Rip)]++;
#elif defined(_M_ARM64) || defined(__aarch64__)
            hits_[uintptr_t(c.Pc)]++;
#endif
            total_++;
        }
        ResumeThread(thread_);
    }
    timeEndPeriod(1);
#endif
}

std::vector<std::string> Sampler::report(const Library &library, size_t count) const {
    std::vector<std::string> lines;
    if (!total_) return lines;
    // the game's functions by where their code starts
    std::vector<std::pair<uintptr_t, std::string>> game;
    library.functions(game);
    std::sort(game.begin(), game.end());
    std::unordered_map<std::string, uint64_t> by_name;
#ifdef _WIN32
    HANDLE process = GetCurrentProcess();
    SymSetOptions(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS);
    bool symbols = SymInitialize(process, nullptr, TRUE);
    // the module the game's code is in: its samples are named by the library
    HMODULE game_module = nullptr;
    if (!game.empty()) {
        GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCSTR>(game.front().first), &game_module);
    }
#endif
    for (auto [at, hits] : hits_) {
        std::string name;
#ifdef _WIN32
        HMODULE module = nullptr;
        GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCSTR>(at), &module);
        if (module && module == game_module) {
            auto it = std::upper_bound(game.begin(), game.end(), std::make_pair(at, std::string("\xff")));
            name = it == game.begin() ? "game: before its first function" : "game " + std::prev(it)->second;
        } else {
            char buffer[sizeof(SYMBOL_INFO) + 256] = {};
            auto *symbol = reinterpret_cast<SYMBOL_INFO *>(buffer);
            symbol->SizeOfStruct = sizeof(SYMBOL_INFO);
            symbol->MaxNameLen = 255;
            char file[MAX_PATH] = "?";
            if (module) {
                GetModuleFileNameA(module, file, MAX_PATH);
                const char *slash = std::max(std::strrchr(file, '\\'), std::strrchr(file, '/'));
                if (slash) std::memmove(file, slash + 1, std::strlen(slash));
            }
            name = file;
            if (symbols && SymFromAddr(process, at, nullptr, symbol)) name += std::string("!") + symbol->Name;
        }
#endif
        by_name[name] += hits;
    }
#ifdef _WIN32
    if (symbols) SymCleanup(process);
#endif
    std::vector<std::pair<uint64_t, std::string>> sorted;
    for (auto &[name, hits] : by_name) sorted.push_back({hits, name});
    std::sort(sorted.rbegin(), sorted.rend());
    for (size_t i = 0; i < sorted.size() && i < count; i++) {
        char line[512];
        std::snprintf(line, sizeof line, "%5.1f%% %s", 100.0 * sorted[i].first / total_, sorted[i].second.c_str());
        lines.push_back(line);
    }
    return lines;
}

} // namespace runtime
