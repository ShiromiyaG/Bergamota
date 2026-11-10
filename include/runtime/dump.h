// A game's dumps as the 3DS lays them out (src/dump.cpp): what the player
// reads the game from, and what the launcher checks and lays out for
// 3dsrecomp. Nothing of the rest of the runtime.

#ifndef RUNTIME_DUMP_H
#define RUNTIME_DUMP_H

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace runtime {

// a window into a file on disk: a RomFS inside a game's dump
struct RomFsImage {
    std::filesystem::path file;
    uint64_t offset = 0, size = 0; // level 3, to the RomFS's end
};

// the RomFS of a .3ds (NCSD, its first partition) or of a CIA's first
// content, both decrypted; nullopt with why when it is not one
std::optional<RomFsImage> find_romfs(const std::filesystem::path &dump, std::string &error);

// the program a dump runs, its exheader (0x800 bytes, as ctrtool writes it)
// and its code, decompressed: from a .3ds, or from an update's CIA, whose
// program takes the game's place. false with why in error
bool read_program(const std::filesystem::path &dump, std::vector<uint8_t> &exheader, std::vector<uint8_t> &code,
                  std::string &error);

// an ExeFS's .code as the SDK compresses it (backwards LZSS, "BLZ"),
// decompressed in place; false with why
bool blz_decompress(std::vector<uint8_t> &data, std::string &error);

// what a dump is, for the launcher: its title id (an update's, for an
// update's CIA) and, for a CIA, its version (the TMD's)
struct DumpInfo {
    uint64_t title_id = 0;
    uint16_t version = 0;
    bool cia = false;
};
enum class DumpProblem { None, Unreadable, Unknown, Encrypted };

// a .3ds, .cci, .cxi or .cia read for what it is; the problem with it, and
// detail for an unreadable one, when it can't be
DumpProblem read_dump(const std::filesystem::path &dump, DumpInfo &info, std::string &detail);

// an update's CIA laid out as 3dsrecomp's --mods takes it (Luma's layout):
// exheader.bin, code.bin decompressed, and its modules (.cro, .crr,
// static.crs) under romfs/ at their paths; false with why
bool unpack_update(const std::filesystem::path &cia, const std::filesystem::path &dir, std::string &error);

// the game's name as its icon gives it (the English short title, UTF-8),
// empty when the dump has none
std::string read_title(const std::filesystem::path &dump);

// the game's icon from its SMDH, as the HOME Menu shows it: the large one
// (48 x 48) and the small one (24 x 24), each pixel RGBA (R in the low byte),
// row by row; false when the dump has none
struct Icon {
    std::vector<uint32_t> large, small;
};
bool read_icon(const std::filesystem::path &dump, Icon &icon);

} // namespace runtime

#endif
