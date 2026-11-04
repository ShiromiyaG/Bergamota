// Changes a player makes to a game without a compiler: Gateway (Action
// Replay) codes, the cheats Azahar and Luma take, run every frame; and an
// IPS patch of the program (Luma's code.ips), laid over code.bin as it
// loads. What either changes in the game's code runs in the interpreter
// (the library's check finds it), until the game is recompiled with it.

#ifndef RUNTIME_CHEATS_H
#define RUNTIME_CHEATS_H

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "runtime/memory.h"

namespace runtime {

// one cheat of a file: [its name], then its code lines, two words each;
// "*citra_enabled" marks the ones on (Azahar's files)
struct Cheat {
    std::string name;
    bool enabled = false;
    std::vector<std::pair<uint32_t, uint32_t>> lines;
};

// Azahar's (Citra's) cheats file, as it keeps them in cheats/<title id>.txt;
// Luma's are the same lines without the marks
std::vector<Cheat> parse_cheats(const std::string &text);

// what a frame of cheats did: words written, and whether any of them was in
// the game's code (from text to text_end), which the library then checks
struct CheatRun {
    uint32_t writes = 0;
    bool code = false;
};

// runs one cheat's lines once, as Azahar does each frame: buttons are the
// 3DS's (services.h's Input), for the codes that wait for them (DD)
void run_cheat(const Cheat &cheat, Memory &memory, uint32_t buttons, uint32_t text, uint32_t text_end,
               CheatRun &run);

// an IPS patch ("PATCH", its records, "EOF", maybe a size to cut to) laid
// over data; false with why when it is not one
bool apply_ips(const std::vector<uint8_t> &patch, std::vector<uint8_t> &data, std::string &error);

} // namespace runtime

#endif
