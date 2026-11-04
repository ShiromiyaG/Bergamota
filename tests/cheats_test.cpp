// The player's changes without a game: Azahar's cheats file read, the
// Gateway code types run on memory of their own, IPS patches laid over
// bytes.

#include "runtime/cheats.h"

#include <cstdio>

using namespace runtime;

namespace {

int failures = 0;

#define CHECK(cond)                                                                                                    \
    do {                                                                                                               \
        if (!(cond)) {                                                                                                 \
            std::printf("%s:%d: %s\n", __FILE__, __LINE__, #cond);                                                     \
            failures++;                                                                                                \
        }                                                                                                              \
    } while (0)

constexpr uint32_t TEXT = 0x00100000, DATA = 0x08000000;

// a page of code (read only) and one of data, as the loader maps them
void map(Memory &memory) {
    auto code = memory.allocate(Region::Application, map::PAGE_SIZE);
    memory.map(TEXT, code->paddr, map::PAGE_SIZE, READ | EXECUTE, MemoryState::Code);
    auto data = memory.allocate(Region::Application, map::PAGE_SIZE);
    memory.map(DATA, data->paddr, map::PAGE_SIZE, READ | WRITE, MemoryState::Private);
}

Cheat cheat(std::vector<std::pair<uint32_t, uint32_t>> lines) { return Cheat{"test", true, std::move(lines)}; }

void reads_azahars_file() {
    auto cheats = parse_cheats("[Infinite money]\r\n*citra_enabled\r\n20000000 000000FF\r\n0012AB00 E1A00000\r\n"
                               "\n[Off one]\n*a note\nD3000000 08000000\nnot a code\n");
    CHECK(cheats.size() == 2);
    if (cheats.size() != 2) return;
    CHECK(cheats[0].name == "Infinite money" && cheats[0].enabled && cheats[0].lines.size() == 2);
    CHECK(cheats[0].lines[1].first == 0x0012AB00 && cheats[0].lines[1].second == 0xE1A00000);
    CHECK(cheats[1].name == "Off one" && !cheats[1].enabled && cheats[1].lines.size() == 1);
}

void writes_with_the_offset() {
    Memory memory(false, 64u << 20);
    map(memory);
    CheatRun run;
    // a word, then the offset set and a halfword and a byte through it
    run_cheat(cheat({{0x08000000, 0x11223344}, {0xD3000000, 0x08000010}, {0x10000002, 0x0000BEEF}, {0x20000004, 0x7F}}),
              memory, 0, TEXT, TEXT + map::PAGE_SIZE, run);
    CHECK(memory.read32(DATA) == 0x11223344);
    CHECK(memory.read16(DATA + 0x12) == 0xBEEF && memory.read8(DATA + 0x14) == 0x7F);
    CHECK(run.writes == 3 && !run.code);
    // the same again writes nothing new
    CheatRun again;
    run_cheat(cheat({{0x08000000, 0x11223344}}), memory, 0, TEXT, TEXT + map::PAGE_SIZE, again);
    CHECK(again.writes == 0);
}

void conditions_skip_to_their_end() {
    Memory memory(false, 64u << 20);
    map(memory);
    memory.write32(DATA, 5);
    CheatRun run;
    // if the word is 6 (it is not): skipped, a nested one with it; after the
    // D0 lines run again
    run_cheat(cheat({{0x58000000, 6}, {0x08000004, 1}, {0x38000000, 100}, {0x08000008, 1}, {0xD0000000, 0},
                     {0xD0000000, 0}, {0x0800000C, 1}}),
              memory, 0, TEXT, TEXT + map::PAGE_SIZE, run);
    CHECK(memory.read32(DATA + 4) == 0 && memory.read32(DATA + 8) == 0 && memory.read32(DATA + 12) == 1);
    // if below 6 (it is): runs
    run_cheat(cheat({{0x38000000, 6}, {0x08000010, 2}, {0xD0000000, 0}}), memory, 0, TEXT, TEXT + map::PAGE_SIZE, run);
    CHECK(memory.read32(DATA + 0x10) == 2);
    // a halfword with a mask: the low byte left out
    memory.write16(DATA + 0x20, 0x12FF);
    run_cheat(cheat({{0x98000020, 0x00FF1200}, {0x08000024, 3}, {0xD2000000, 0}}), memory, 0, TEXT, TEXT + map::PAGE_SIZE,
              run);
    CHECK(memory.read32(DATA + 0x24) == 3);
    // the buttons: A held, A and B asked for
    run_cheat(cheat({{0xDD000000, 3}, {0x08000028, 4}, {0xD0000000, 0}}), memory, 1, TEXT, TEXT + map::PAGE_SIZE, run);
    CHECK(memory.read32(DATA + 0x28) == 0);
    run_cheat(cheat({{0xDD000000, 3}, {0x08000028, 4}, {0xD0000000, 0}}), memory, 3, TEXT, TEXT + map::PAGE_SIZE, run);
    CHECK(memory.read32(DATA + 0x28) == 4);
}

void the_data_register_loops_and_runs() {
    Memory memory(false, 64u << 20);
    map(memory);
    memory.write32(DATA + 0x100, 0x08000040); // a pointer the game keeps
    CheatRun run;
    // through the pointer: offset = [0x100], then 7 written there
    run_cheat(cheat({{0xB8000100, 0}, {0x00000000, 7}, {0xD2000000, 0}}), memory, 0, TEXT, TEXT + map::PAGE_SIZE, run);
    CHECK(memory.read32(DATA + 0x40) == 7);
    // the register: 9, stored three times a word apart by a loop (2 more)
    run_cheat(cheat({{0xD3000000, 0x08000080}, {0xD5000000, 9}, {0xC0000000, 2}, {0xD6000000, 0}, {0xD1000000, 0},
                     {0xD2000000, 0}}),
              memory, 0, TEXT, TEXT + map::PAGE_SIZE, run);
    CHECK(memory.read32(DATA + 0x80) == 9 && memory.read32(DATA + 0x84) == 9 && memory.read32(DATA + 0x88) == 9);
    CHECK(memory.read32(DATA + 0x8C) == 0);
    // a run of bytes from the lines after it
    run_cheat(cheat({{0xE8000200, 6}, {0x44332211, 0x00006655}, {0x08000300, 1}}), memory, 0, TEXT, TEXT + map::PAGE_SIZE,
              run);
    CHECK(memory.read32(DATA + 0x200) == 0x44332211 && memory.read16(DATA + 0x204) == 0x6655 &&
          memory.read8(DATA + 0x206) == 0);
    CHECK(memory.read32(DATA + 0x300) == 1);
}

void code_is_written_and_said() {
    Memory memory(false, 64u << 20);
    map(memory);
    CheatRun run;
    // a beq made a b, as the no-ghosting code does: into read-only code
    run_cheat(cheat({{0x00100010, 0xEA000003}}), memory, 0, TEXT, TEXT + map::PAGE_SIZE, run);
    CHECK(memory.read32(TEXT + 0x10) == 0xEA000003 && run.code);
}

void ips_patches() {
    std::vector<uint8_t> data(16, 0);
    std::string error;
    // three bytes at 2, a run of four 0x7F at 8, then the end
    std::vector<uint8_t> patch = {'P', 'A', 'T', 'C', 'H', 0, 0, 2, 0, 3, 0xA, 0xB, 0xC, 0, 0, 8, 0, 0, 0, 4, 0x7F,
                                  'E', 'O', 'F'};
    CHECK(apply_ips(patch, data, error));
    CHECK(data[2] == 0xA && data[4] == 0xC && data[8] == 0x7F && data[11] == 0x7F && data[12] == 0 && data.size() == 16);
    // a size to cut to after EOF
    std::vector<uint8_t> cut = {'P', 'A', 'T', 'C', 'H', 'E', 'O', 'F', 0, 0, 10};
    CHECK(apply_ips(cut, data, error) && data.size() == 10);
    std::vector<uint8_t> bad = {'P', 'A', 'T', 'C', 'X', 'E', 'O', 'F'};
    CHECK(!apply_ips(bad, data, error) && !error.empty());
    std::vector<uint8_t> short_one = {'P', 'A', 'T', 'C', 'H', 0, 0, 2, 0, 9, 1};
    CHECK(!apply_ips(short_one, data, error));
}

} // namespace

int main() {
    reads_azahars_file();
    writes_with_the_offset();
    conditions_skip_to_their_end();
    the_data_register_loops_and_runs();
    code_is_written_and_said();
    ips_patches();
    if (failures) std::printf("%d failed\n", failures);
    return failures ? 1 : 0;
}
