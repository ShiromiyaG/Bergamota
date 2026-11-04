// Reading a decrypted game dump, a .3ds cartridge image, a .cia or a bare
// .cxi, for what recompiling needs: the code and its layout from the
// executable partition, and the modules and their descriptions from its
// RomFS. Only what is read is loaded, dumps run to gigabytes. Translated from
// 3dsrecomp's rom.rs (Rust, MIT, Copyright (c) 2026 fearkov; see
// LICENSE-3dsrecomp).

#ifndef RECOMP_ROM_H
#define RECOMP_ROM_H

#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace recomp {

// where one segment of the code is mapped, from the exheader
struct CodeSetInfo {
    uint32_t address = 0;
    // the pages it is mapped in
    uint32_t num_pages = 0;
    // the bytes it really has
    uint32_t size = 0;
};

// the parts of the extended header recompiling cares about
struct ExHeader {
    std::string title;
    // the ExeFS .code is compressed
    bool compress_code = false;
    CodeSetInfo text, rodata, data;
    uint32_t bss_size = 0;

    // the parts of an extended header read from its first 0x400 bytes (it
    // needs 0x40 of them)
    static ExHeader read(const std::vector<uint8_t> &ex);
};

struct DirEntry {
    uint32_t first_child = 0;
    uint32_t first_file = 0;
    uint32_t next_sibling = 0;
    std::string name;
};

struct FileEntry {
    uint32_t next_sibling = 0;
    // where its bytes start in the file data
    uint64_t data_offset = 0;
    uint64_t data_size = 0;
    std::string name;
};

// the directory tree of a RomFS, the tables read in, the files left in the
// dump
struct RomFs {
    // where the file data starts in the dump
    uint64_t data = 0;
    std::vector<uint8_t> dir_table;
    std::vector<uint8_t> file_table;

    std::optional<DirEntry> dir(uint32_t offset) const;
    std::optional<FileEntry> file(uint32_t offset) const;
    // "the RomFS has no root"
    std::optional<DirEntry> root(std::string &error) const;
    // the folders in dir, with where each is in the table
    std::vector<std::pair<uint32_t, DirEntry>> subdirs(const DirEntry &dir) const;
    // the files in dir, with where each is in the table
    std::vector<std::pair<uint32_t, FileEntry>> files(const DirEntry &dir) const;
    // a file by its path, / separated, the case ignored
    std::optional<FileEntry> lookup(const std::string &path, std::string &error) const;
};

// a game's executable partition
class Title {
public:
    Title();
    Title(Title &&) noexcept;
    Title &operator=(Title &&) noexcept;
    ~Title();

    // false with why
    static bool load(const std::filesystem::path &path, Title &out, std::string &error);

    uint64_t program_id() const { return program_id_; }

    // the code, the three segments one after another, decompressed
    bool code(std::vector<uint8_t> &out, std::string &error) const;
    // up to len bytes of a RomFS file from offset
    std::optional<std::vector<uint8_t>> read_romfs(const FileEntry &file, uint64_t offset, size_t len) const;

    ExHeader exheader;
    std::optional<RomFs> romfs;

private:
    bool read(uint64_t offset, size_t len, std::vector<uint8_t> &out, std::string &error) const;

    // the dump, open, behind a mutex (rom.cpp)
    struct File;
    std::unique_ptr<File> file;
    uint64_t program_id_ = 0;
    // where .code is in the dump, and how big
    std::pair<uint64_t, uint64_t> code_{0, 0};
};

// undoes the backwards LZSS the SDK compresses .code with, which works from
// the end of the buffer towards its start; false with why
bool decompress(const std::vector<uint8_t> &compressed, std::vector<uint8_t> &out, std::string &error);

} // namespace recomp

#endif
