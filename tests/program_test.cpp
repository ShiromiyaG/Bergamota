// read_program against ctrtool's extraction of the same dump, when
// RUNTIME_TEST_DUMP, RUNTIME_TEST_EXHEADER and RUNTIME_TEST_CODE name them
// (no game here otherwise: skipped)
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "runtime/fs.h"

static std::vector<uint8_t> file(const char *path) {
    std::ifstream in(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), {}};
}

int main() {
    const char *dump = std::getenv("RUNTIME_TEST_DUMP"), *ex = std::getenv("RUNTIME_TEST_EXHEADER"),
               *known = std::getenv("RUNTIME_TEST_CODE");
    if (!dump || !ex || !known) {
        std::printf("skipped: no dump named\n");
        return 0;
    }
    std::vector<uint8_t> exheader, code;
    std::string error;
    if (!runtime::read_program(dump, exheader, code, error)) {
        std::printf("error: %s\n", error.c_str());
        return 1;
    }
    bool same_ex = exheader == file(ex), same_code = code == file(known);
    std::printf("title: %s\n", runtime::read_title(dump).c_str());
    std::printf("exheader %s, code %s (%zu bytes)\n", same_ex ? "same" : "DIFFERS", same_code ? "same" : "DIFFERS", code.size());
    return same_ex && same_code ? 0 : 1;
}
