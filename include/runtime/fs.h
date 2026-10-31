// fs:USER and the file and directory sessions it hands out: the game's
// RomFS read from its own dump (the .3ds, and the update's CIA), its saves
// and extra data kept as folders on disk, laid out as Zakuro lays them, so
// the same saves work in both. Translated from Zakuro's services/fs.rs and
// services/host_archive.rs, and zakuro-fs's NCSD, NCCH, CIA and RomFS
// readers (Rust, MIT, Copyright (c) 2026 fearkov; see LICENSE-zakuro).

#ifndef RUNTIME_FS_H
#define RUNTIME_FS_H

#include <cstdint>
#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "runtime/services.h"

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

// the game's name as its icon gives it (the English short title, UTF-8),
// empty when the dump has none
std::string read_title(const std::filesystem::path &dump);

class Fs {
public:
    // user: where saves and extra data are (Zakuro's data folder/user)
    // (a folder that holds a user folder, Zakuro's data folder itself, is
    // taken for that user folder)
    Fs(std::filesystem::path user, uint64_t program_id) : user_(std::move(user)), program_id_(program_id) {
        std::error_code ec;
        if (std::filesystem::is_directory(user_ / "user", ec) && !std::filesystem::exists(user_ / "savedata", ec)) user_ /= "user";
    }

    std::optional<RomFsImage> romfs, update_romfs;

    // fs:USER, FSFile and FSDirectory
    bool handle(System &system, Thread &thread, CommandBuffer &b, const std::string &service, uint32_t subhandle);

private:
    enum class Backing { Rom, Host, Memory };
    struct File {
        Backing backing;
        RomFsImage rom;
        std::filesystem::path host;
        std::vector<uint8_t> bytes;
        uint64_t size() const;
    };
    struct Entry {
        std::string name;
        bool directory;
        uint64_t size;
    };
    struct Directory {
        std::vector<Entry> entries;
        size_t cursor = 0;
    };
    struct Archive {
        uint32_t id;
        std::optional<std::filesystem::path> host; // the writable ones
    };

    bool user(System &, Thread &, CommandBuffer &);
    bool file(System &, Thread &, CommandBuffer &, uint32_t id);
    bool directory(System &, CommandBuffer &, uint32_t id);
    std::optional<std::filesystem::path> host_archive(uint32_t id, const std::vector<uint8_t> &path) const;
    uint32_t open_host_archive(uint32_t id, const std::vector<uint8_t> &path, std::filesystem::path &out) const;
    uint32_t open_host_file(System &, const std::filesystem::path &root, uint32_t type, uint32_t size,
                            uint32_t pointer, uint32_t flags, uint32_t &id);
    std::optional<uint32_t> open_rom_file(System &, uint32_t archive, uint32_t type, uint32_t size, uint32_t pointer);
    std::optional<uint32_t> open_ncch(uint32_t kind);
    Handle session(System &, const char *service, uint32_t id);
    void reply_file(System &, CommandBuffer &, uint32_t command, uint32_t code, uint32_t id);

    std::filesystem::path user_;
    uint64_t program_id_;
    std::map<uint64_t, Archive> archives_;
    uint64_t next_archive_ = 0;
    std::map<uint32_t, File> files_;
    uint32_t next_file_ = 0;
    std::map<uint32_t, Directory> directories_;
    uint32_t next_directory_ = 0;
    uint32_t priority_ = 0;
    uint64_t busy_until_[2]{};
};

} // namespace runtime

#endif
