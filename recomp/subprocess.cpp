// Running another program (subprocess.h).

#include "subprocess.h"

#include <cstdio>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#else
#include <fcntl.h>
#include <spawn.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <unistd.h>
extern char **environ;
#endif

namespace recomp {

#ifdef _WIN32
namespace {

std::wstring wide(const std::string &utf8) {
    if (utf8.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, utf8.data(), int(utf8.size()), nullptr, 0);
    std::wstring out(size_t(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8.data(), int(utf8.size()), out.data(), n);
    return out;
}

// one argument as CommandLineToArgvW (and the C runtime) reads it back
void quote(std::wstring &line, const std::wstring &arg) {
    if (!arg.empty() && arg.find_first_of(L" \t\n\v\"") == std::wstring::npos) {
        line += arg;
        return;
    }
    line += L'"';
    for (size_t i = 0;; i++) {
        size_t slashes = 0;
        while (i < arg.size() && arg[i] == L'\\') i++, slashes++;
        if (i == arg.size()) {
            line.append(slashes * 2, L'\\');
            break;
        }
        if (arg[i] == L'"') {
            line.append(slashes * 2 + 1, L'\\');
        } else {
            line.append(slashes, L'\\');
        }
        line += arg[i];
    }
    line += L'"';
}

// the program as CreateProcess wants it: as given when it has a folder in
// it, else looked for in PATH with the usual extensions, as Rust's Command
// does
std::wstring resolve(const std::filesystem::path &program) {
    if (program.has_parent_path()) return program.wstring();
    std::wstring name = program.wstring();
    wchar_t found[MAX_PATH];
    for (const wchar_t *extension : {L".exe", L".com", L".bat", L".cmd", L""}) {
        if (SearchPathW(nullptr, name.c_str(), *extension ? extension : nullptr, MAX_PATH, found, nullptr)) {
            return found;
        }
    }
    return name;
}

} // namespace

Ran run(const Run &command) {
    Ran ran;
    std::wstring application = resolve(command.program);
    std::wstring line;
    quote(line, command.program.wstring());
    for (const std::string &arg : command.args) {
        line += L' ';
        quote(line, wide(arg));
    }
    SECURITY_ATTRIBUTES inherit{sizeof inherit, nullptr, TRUE};
    HANDLE read_end = nullptr, write_end = nullptr, null = nullptr;
    STARTUPINFOW startup{};
    startup.cb = sizeof startup;
    if (command.capture || command.quiet) {
        startup.dwFlags = STARTF_USESTDHANDLES;
        startup.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
        null = CreateFileW(L"NUL", GENERIC_WRITE, FILE_SHARE_WRITE, &inherit, OPEN_EXISTING, 0, nullptr);
        startup.hStdError = command.quiet ? null : GetStdHandle(STD_ERROR_HANDLE);
        if (command.capture) {
            CreatePipe(&read_end, &write_end, &inherit, 0);
            SetHandleInformation(read_end, HANDLE_FLAG_INHERIT, 0);
            startup.hStdOutput = write_end;
        } else {
            startup.hStdOutput = null;
        }
    }
    PROCESS_INFORMATION process{};
    DWORD flags = CREATE_UNICODE_ENVIRONMENT | (command.lower ? BELOW_NORMAL_PRIORITY_CLASS : 0);
    std::wstring dir = command.dir.empty() ? std::wstring() : command.dir.wstring();
    BOOL ok = CreateProcessW(application.c_str(), line.data(), nullptr, nullptr, TRUE, flags, nullptr,
                             dir.empty() ? nullptr : dir.c_str(), &startup, &process);
    if (write_end) CloseHandle(write_end);
    if (!ok) {
        ran.error = "the program could not be started (" + std::to_string(GetLastError()) + ")";
        if (read_end) CloseHandle(read_end);
        if (null) CloseHandle(null);
        return ran;
    }
    ran.started = true;
    if (read_end) {
        char buffer[4096];
        DWORD got = 0;
        while (ReadFile(read_end, buffer, sizeof buffer, &got, nullptr) && got) ran.output.append(buffer, got);
        CloseHandle(read_end);
    }
    WaitForSingleObject(process.hProcess, INFINITE);
    DWORD code = 0;
    GetExitCodeProcess(process.hProcess, &code);
    ran.status = int(code);
    CloseHandle(process.hProcess);
    CloseHandle(process.hThread);
    if (null) CloseHandle(null);
    return ran;
}

#else

Ran run(const Run &command) {
    Ran ran;
    int pipe_ends[2] = {-1, -1};
    if (command.capture && pipe(pipe_ends) != 0) {
        ran.error = "no pipe";
        return ran;
    }
    pid_t pid = fork();
    if (pid < 0) {
        ran.error = "could not fork";
        return ran;
    }
    if (pid == 0) {
        if (command.lower) setpriority(PRIO_PROCESS, 0, 10);
        if (!command.dir.empty() && chdir(command.dir.c_str()) != 0) _exit(127);
        int null = (command.quiet || command.capture) ? open("/dev/null", O_WRONLY) : -1;
        if (command.capture) {
            dup2(pipe_ends[1], 1);
            close(pipe_ends[0]);
            close(pipe_ends[1]);
        } else if (command.quiet) {
            dup2(null, 1);
        }
        if (command.quiet) dup2(null, 2);
        std::vector<char *> argv;
        std::string program = command.program.string();
        argv.push_back(program.data());
        std::vector<std::string> args = command.args;
        for (std::string &arg : args) argv.push_back(arg.data());
        argv.push_back(nullptr);
        execvp(program.c_str(), argv.data());
        _exit(127);
    }
    if (command.capture) {
        close(pipe_ends[1]);
        char buffer[4096];
        ssize_t got;
        while ((got = read(pipe_ends[0], buffer, sizeof buffer)) > 0) ran.output.append(buffer, size_t(got));
        close(pipe_ends[0]);
    }
    int status = 0;
    waitpid(pid, &status, 0);
    if (WIFEXITED(status) && WEXITSTATUS(status) == 127) {
        // execvp failed: as Rust says when the program is not there
        ran.error = "No such file or directory";
        return ran;
    }
    ran.started = true;
    ran.status = WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status);
    return ran;
}

#endif

std::string describe_status(const Ran &ran) {
    if (!ran.started) return ran.error;
    return "exit code: " + std::to_string(ran.status);
}

} // namespace recomp
