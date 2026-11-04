// Running another program: the C compiler, the linker. What Rust's
// std::process::Command gave 3dsrecomp, as much of it as it used.

#ifndef RECOMP_SUBPROCESS_H
#define RECOMP_SUBPROCESS_H

#include <filesystem>
#include <string>
#include <vector>

namespace recomp {

struct Run {
    std::filesystem::path program;
    std::vector<std::string> args; // UTF-8
    std::filesystem::path dir;     // where it runs, empty for here
    bool lower = false;            // below normal priority
    bool capture = false;          // its output kept, else left to ours
    bool quiet = false;            // its output and errors thrown away
};

struct Ran {
    bool started = false; // false: it could not be run at all, error says why
    int status = -1;      // its exit code
    std::string output;   // with capture
    std::string error;
    bool success() const { return started && status == 0; }
};

Ran run(const Run &command);

// what Rust's Display of an ExitStatus says: "exit code: 1"
std::string describe_status(const Ran &ran);

} // namespace recomp

#endif
