// A port's options: decomp.txt's option lines, which build.py writes from
// the port's code (native/'s PORT_OPTION). Apart from decomp.h so the
// launcher reads them without the rest of the runtime.

#ifndef RUNTIME_DECOMP_OPTIONS_H
#define RUNTIME_DECOMP_OPTIONS_H

#include <cstdint>
#include <string>
#include <vector>

namespace runtime {

// an option of the port's own code: a word of the decomp's (symbol) the
// player writes, shown in its menu and the launcher by label and help and
// kept in the settings' [port] by name (the symbol without port_). A toggle
// is 0 or 1; otherwise a number from min to max. value: the code's default
struct DecompOption {
    std::string symbol, name, label, help;
    bool toggle = true;
    int32_t value = 0, min = 0, max = 1;
};

// one line, "option <symbol> bool <default> <label>|<help>" or
// "option <symbol> int <default> <min> <max> <label>|<help>"; false when it
// is not one
bool parse_option(const std::string &line, DecompOption &out);
// the option lines of dir/decomp.txt (dir in UTF-8), none without one
std::vector<DecompOption> read_options(const std::string &dir);

} // namespace runtime

#endif
