// Reading a decrypted game dump, a .3ds cartridge image, a .cia or a bare
// .cxi: the code and its layout from the executable partition, and the
// RomFS's directory tree. Translated from 3dsrecomp's rom.rs (Rust, MIT,
// Copyright (c) 2026 fearkov; see LICENSE-3dsrecomp).

#include "rom.h"

#include <cstdio>
#include <cstring>
#include <mutex>

#include "rust.h"

namespace recomp {

namespace {

// offsets and sizes in NCSD and NCCH headers count these
constexpr uint64_t MEDIA_UNIT = 0x200;
constexpr uint64_t NCCH_HEADER_SIZE = 0x200;
constexpr uint64_t EXEFS_HEADER_SIZE = 0x200;
// the RomFS tables' mark for no entry
constexpr uint32_t NONE = 0xFFFFFFFF;
// a .cia's header size, which is also its first field
constexpr uint32_t CIA_HEADER_SIZE = 0x2020;

uint16_t u16_at(const uint8_t *bytes, size_t offset) { return uint16_t(bytes[offset] | bytes[offset + 1] << 8); }

uint32_t u32_at(const uint8_t *bytes, size_t offset) {
    return uint32_t(bytes[offset]) | uint32_t(bytes[offset + 1]) << 8 | uint32_t(bytes[offset + 2]) << 16 |
           uint32_t(bytes[offset + 3]) << 24;
}

uint64_t u64_at(const uint8_t *bytes, size_t offset) {
    return uint64_t(u32_at(bytes, offset)) | uint64_t(u32_at(bytes, offset + 4)) << 32;
}

uint32_t u32_at(const std::vector<uint8_t> &bytes, size_t offset) { return u32_at(bytes.data(), offset); }
uint64_t u64_at(const std::vector<uint8_t> &bytes, size_t offset) { return u64_at(bytes.data(), offset); }

// u64::next_multiple_of, wrapping as a release build does
uint64_t next_multiple_of(uint64_t n, uint64_t rhs) {
    uint64_t r = n % rhs;
    return r == 0 ? n : n + (rhs - r);
}

std::string ascii(const uint8_t *bytes, size_t size) {
    size_t end = 0;
    while (end < size && bytes[end] != 0) end++;
    return rust::from_utf8_lossy(bytes, end);
}

CodeSetInfo read_code_set(const std::vector<uint8_t> &bytes, size_t offset) {
    return CodeSetInfo{u32_at(bytes, offset), u32_at(bytes, offset + 4), u32_at(bytes, offset + 8)};
}

// String::from_utf16_lossy of the little endian units in bytes, a byte left
// over dropped
std::string utf16(const uint8_t *bytes, size_t size) {
    std::vector<uint32_t> units;
    for (size_t i = 0; i + 2 <= size; i += 2) units.push_back(u16_at(bytes, i));
    std::string out;
    for (size_t i = 0; i < units.size(); i++) {
        uint32_t unit = units[i], c = unit;
        if (unit >= 0xD800 && unit <= 0xDBFF && i + 1 < units.size() && units[i + 1] >= 0xDC00 &&
            units[i + 1] <= 0xDFFF) {
            c = 0x10000 + ((unit - 0xD800) << 10) + (units[i + 1] - 0xDC00);
            i++;
        } else if (unit >= 0xD800 && unit <= 0xDFFF) {
            c = 0xFFFD;
        }
        rust::push_char(out, c);
    }
    return out;
}

bool eq_ignore_ascii_case(const std::string &a, const std::string &b) {
    if (a.size() != b.size()) return false;
    auto lower = [](char c) { return c >= 'A' && c <= 'Z' ? char(c - 'A' + 'a') : c; };
    for (size_t i = 0; i < a.size(); i++)
        if (lower(a[i]) != lower(b[i])) return false;
    return true;
}

// len bytes of file from offset, read_exact's way
bool read_at(std::FILE *file, uint64_t offset, size_t len, std::vector<uint8_t> &out, std::string &error) {
    out.assign(len, 0);
    if (!rust::seek(file, offset)) {
        error = rust::os_error(errno);
        return false;
    }
    if (len && std::fread(out.data(), 1, len, file) != len) {
        std::clearerr(file);
        error = "failed to fill whole buffer";
        return false;
    }
    return true;
}

// where the executable content of a .cia starts. the header, certificates,
// ticket, TMD and contents each start on 64 bytes, and the contents follow
// in the order of the TMD's records, those the header's bitmap has.
bool cia_executable(const std::vector<uint8_t> &header, std::FILE *file, uint64_t &out, std::string &error) {
    auto align = [](uint64_t n) { return next_multiple_of(n, 0x40); };
    std::vector<uint8_t> bitmap;
    if (!read_at(file, 0x20, 0x2000, bitmap, error)) return false;
    uint64_t tmd_offset =
        align(align(align(CIA_HEADER_SIZE) + u32_at(header, 0x08)) + u32_at(header, 0x0C));
    uint64_t contents = align(tmd_offset + u32_at(header, 0x10));
    std::vector<uint8_t> tmd;
    if (!read_at(file, tmd_offset, u32_at(header, 0x10), tmd, error)) return false;
    // the Rust indexes the TMD unchecked and panics past its end
    bool short_tmd = false;
    auto be16 = [&](size_t at) -> uint64_t {
        if (at + 2 > tmd.size()) {
            short_tmd = true;
            return 0;
        }
        return uint64_t(tmd[at]) << 8 | tmd[at + 1];
    };
    auto be64 = [&](size_t at) -> uint64_t {
        if (at + 8 > tmd.size()) {
            short_tmd = true;
            return 0;
        }
        uint64_t value = 0;
        for (size_t i = 0; i < 8; i++) value = value << 8 | tmd[at + i];
        return value;
    };
    auto cut_short = [&] {
        error = "the .cia's TMD is cut short";
        return false;
    };
    if (tmd.size() < 4) return cut_short();
    // the signature's size goes by its type, padded to 64 bytes
    size_t signature;
    switch ((uint32_t(tmd[0]) << 24 | uint32_t(tmd[1]) << 16 | uint32_t(tmd[2]) << 8 | tmd[3]) & 0xFFFF) {
    case 0x0000:
    case 0x0003: signature = 0x240; break;
    case 0x0001:
    case 0x0004: signature = 0x140; break;
    case 0x0002:
    case 0x0005: signature = 0x80; break;
    default: error = "the .cia's TMD has an unknown signature"; return false;
    }
    uint64_t kind = be64(signature + 0x4C) >> 32;
    if (short_tmd) return cut_short();
    if (kind == 0x0004000E) {
        error = "this .cia is an update, not a game";
        return false;
    }
    if (kind == 0x0004008C) {
        error = "this .cia is downloadable content, not a game";
        return false;
    }
    uint64_t offset = contents;
    size_t count = size_t(be16(signature + 0x9E));
    if (short_tmd) return cut_short();
    for (size_t i = 0; i < count; i++) {
        size_t record = signature + 0xC4 + 64 * 0x24 + i * 0x30;
        size_t index = size_t(be16(record + 4));
        if (short_tmd) return cut_short();
        if ((bitmap[index / 8] & (0x80 >> (index % 8))) == 0) continue;
        if (index == 0) {
            uint64_t type = be16(record + 6);
            if (short_tmd) return cut_short();
            if (type & 1) {
                error = "the .cia is encrypted, it has to be decrypted first";
                return false;
            }
            out = offset;
            return true;
        }
        uint64_t size = be64(record + 8);
        if (short_tmd) return cut_short();
        offset = align(offset + size);
    }
    error = "the .cia has no executable";
    return false;
}

// the level 3 tables of the RomFS whose IVFC header is at offset
bool read_romfs_tables(std::FILE *file, uint64_t offset, RomFs &out, std::string &error) {
    std::vector<uint8_t> ivfc;
    if (!read_at(file, offset, 0x60, ivfc, error)) return false;
    if (std::memcmp(ivfc.data(), "IVFC", 4) != 0) {
        error = "the RomFS has no IVFC header";
        return false;
    }
    // level 3 starts after the header and the master hash, on a block of
    // its own size
    uint64_t master_hash = u32_at(ivfc, 0x08);
    uint32_t shift = u32_at(ivfc, 0x0C + 2 * 0x18 + 0x10);
    uint64_t block = uint64_t(1) << (shift < 31 ? shift : 31);
    uint64_t level3 = offset + next_multiple_of(0x60 + master_hash, block);
    std::vector<uint8_t> header;
    if (!read_at(file, level3, 0x28, header, error)) return false;
    if (u32_at(header, 0) != 0x28) {
        error = "the RomFS level 3 header is not 0x28 bytes";
        return false;
    }
    auto table = [&](size_t at, std::vector<uint8_t> &bytes) {
        return read_at(file, level3 + u32_at(header, at), u32_at(header, at + 4), bytes, error);
    };
    RomFs romfs;
    if (!table(0x0C, romfs.dir_table)) return false;
    if (!table(0x1C, romfs.file_table)) return false;
    romfs.data = level3 + u32_at(header, 0x24);
    out = std::move(romfs);
    return true;
}

} // namespace

ExHeader ExHeader::read(const std::vector<uint8_t> &ex) {
    ExHeader header;
    header.title = ascii(ex.data(), 8);
    header.compress_code = (ex[0x0D] & 1) != 0;
    header.text = read_code_set(ex, 0x10);
    header.rodata = read_code_set(ex, 0x20);
    header.data = read_code_set(ex, 0x30);
    header.bss_size = u32_at(ex, 0x3C);
    return header;
}

std::optional<DirEntry> RomFs::dir(uint32_t offset) const {
    size_t at = offset;
    if (at + 0x18 > dir_table.size()) return std::nullopt;
    const uint8_t *entry = dir_table.data() + at;
    size_t length = u32_at(entry, 0x14);
    if (at + 0x18 + length > dir_table.size()) return std::nullopt;
    DirEntry out;
    out.next_sibling = u32_at(entry, 0x04);
    out.first_child = u32_at(entry, 0x08);
    out.first_file = u32_at(entry, 0x0C);
    out.name = utf16(dir_table.data() + at + 0x18, length);
    return out;
}

std::optional<FileEntry> RomFs::file(uint32_t offset) const {
    size_t at = offset;
    if (at + 0x20 > file_table.size()) return std::nullopt;
    const uint8_t *entry = file_table.data() + at;
    size_t length = u32_at(entry, 0x1C);
    if (at + 0x20 + length > file_table.size()) return std::nullopt;
    FileEntry out;
    out.next_sibling = u32_at(entry, 0x04);
    out.data_offset = u64_at(entry, 0x08);
    out.data_size = u64_at(entry, 0x10);
    out.name = utf16(file_table.data() + at + 0x20, length);
    return out;
}

std::optional<DirEntry> RomFs::root(std::string &error) const {
    auto root = dir(0);
    if (!root) error = "the RomFS has no root";
    return root;
}

std::vector<std::pair<uint32_t, DirEntry>> RomFs::subdirs(const DirEntry &parent) const {
    std::vector<std::pair<uint32_t, DirEntry>> out;
    uint32_t cursor = parent.first_child;
    while (cursor != NONE) {
        auto entry = dir(cursor);
        if (!entry) break;
        uint32_t next = entry->next_sibling;
        out.emplace_back(cursor, std::move(*entry));
        cursor = next;
    }
    return out;
}

std::vector<std::pair<uint32_t, FileEntry>> RomFs::files(const DirEntry &parent) const {
    std::vector<std::pair<uint32_t, FileEntry>> out;
    uint32_t cursor = parent.first_file;
    while (cursor != NONE) {
        auto entry = file(cursor);
        if (!entry) break;
        uint32_t next = entry->next_sibling;
        out.emplace_back(cursor, std::move(*entry));
        cursor = next;
    }
    return out;
}

std::optional<FileEntry> RomFs::lookup(const std::string &path, std::string &error) const {
    auto missing = [&]() -> std::optional<FileEntry> {
        error = path + " is not in the RomFS";
        return std::nullopt;
    };
    auto found = root(error);
    if (!found) return std::nullopt;
    DirEntry current = std::move(*found);
    // the contents hang off a nameless folder under the root, which no path
    // spells out
    for (auto &[_, d] : subdirs(current)) {
        if (d.name.empty()) {
            current = d;
            break;
        }
    }
    std::vector<std::string> parts;
    size_t start = 0;
    while (true) {
        size_t slash = path.find('/', start);
        std::string part = path.substr(start, slash == std::string::npos ? std::string::npos : slash - start);
        if (!part.empty()) parts.push_back(part);
        if (slash == std::string::npos) break;
        start = slash + 1;
    }
    if (parts.empty()) return missing();
    std::string name = parts.back();
    parts.pop_back();
    for (const std::string &part : parts) {
        bool moved = false;
        for (auto &[_, d] : subdirs(current)) {
            if (eq_ignore_ascii_case(d.name, part)) {
                current = d;
                moved = true;
                break;
            }
        }
        if (!moved) return missing();
    }
    for (auto &[_, f] : files(current))
        if (eq_ignore_ascii_case(f.name, name)) return f;
    return missing();
}

struct Title::File {
    std::FILE *handle = nullptr;
    std::mutex lock;
    ~File() {
        if (handle) std::fclose(handle);
    }
};

Title::Title() = default;
Title::Title(Title &&) noexcept = default;
Title &Title::operator=(Title &&) noexcept = default;
Title::~Title() = default;

bool Title::load(const std::filesystem::path &path, Title &out, std::string &error) {
    auto file = std::make_unique<File>();
    file->handle = rust::open(path, error);
    if (!file->handle) return false;
    std::FILE *handle = file->handle;

    // a cartridge image holds the executable as its first partition, a .cia
    // as its first content, and a .cxi is that partition by itself
    std::vector<uint8_t> start;
    if (!read_at(handle, 0, 0x200, start, error)) return false;
    uint64_t ncch;
    if (std::memcmp(start.data() + 0x100, "NCSD", 4) == 0) {
        ncch = uint64_t(u32_at(start, 0x120)) * MEDIA_UNIT;
    } else if (std::memcmp(start.data() + 0x100, "NCCH", 4) == 0) {
        ncch = 0;
    } else if (u32_at(start, 0) == CIA_HEADER_SIZE) {
        if (!cia_executable(start, handle, ncch, error)) return false;
    } else {
        error = "not a 3DS cartridge image, .cia or executable";
        return false;
    }
    std::vector<uint8_t> header;
    if (!read_at(handle, ncch, NCCH_HEADER_SIZE, header, error)) return false;
    if (std::memcmp(header.data() + 0x100, "NCCH", 4) != 0) {
        error = "the executable partition has no NCCH header";
        return false;
    }
    const uint8_t *flags = header.data() + 0x188;
    // the partition is readable as it is when it says so, or when it was
    // never encrypted
    if ((flags[7] & 0x04) == 0 && flags[3] != 0) {
        error = "the dump is encrypted, it has to be decrypted first";
        return false;
    }
    uint64_t program_id = u64_at(header, 0x118);
    uint64_t exefs = ncch + uint64_t(u32_at(header, 0x1A0)) * MEDIA_UNIT;
    uint64_t romfs_offset = ncch + uint64_t(u32_at(header, 0x1B0)) * MEDIA_UNIT;
    bool has_romfs = u32_at(header, 0x1B4) != 0 && (flags[7] & 0x02) == 0;

    std::vector<uint8_t> ex;
    if (!read_at(handle, ncch + NCCH_HEADER_SIZE, 0x400, ex, error)) return false;
    ExHeader exheader = ExHeader::read(ex);

    // ten files, a name, an offset past the header and a size each
    std::vector<uint8_t> table;
    if (!read_at(handle, exefs, EXEFS_HEADER_SIZE, table, error)) return false;
    std::optional<std::pair<uint64_t, uint64_t>> code;
    for (size_t i = 0; i < 10 && (i + 1) * 16 <= table.size(); i++) {
        const uint8_t *entry = table.data() + i * 16;
        if (ascii(entry, 8) == ".code") {
            code = {exefs + EXEFS_HEADER_SIZE + u32_at(entry, 8), u32_at(entry, 12)};
            break;
        }
    }
    if (!code) {
        error = "the ExeFS has no .code";
        return false;
    }

    std::optional<RomFs> romfs;
    if (has_romfs) {
        RomFs tables;
        std::string ignored;
        if (read_romfs_tables(handle, romfs_offset, tables, ignored)) romfs = std::move(tables);
    }
    Title title;
    title.file = std::move(file);
    title.program_id_ = program_id;
    title.exheader = std::move(exheader);
    title.code_ = *code;
    title.romfs = std::move(romfs);
    out = std::move(title);
    return true;
}

bool Title::read(uint64_t offset, size_t len, std::vector<uint8_t> &out, std::string &error) const {
    std::lock_guard guard(file->lock);
    return read_at(file->handle, offset, len, out, error);
}

bool Title::code(std::vector<uint8_t> &out, std::string &error) const {
    std::vector<uint8_t> raw;
    if (!read(code_.first, size_t(code_.second), raw, error)) return false;
    if (exheader.compress_code) return decompress(raw, out, error);
    out = std::move(raw);
    return true;
}

std::optional<std::vector<uint8_t>> Title::read_romfs(const FileEntry &entry, uint64_t offset, size_t len) const {
    if (!romfs) return std::nullopt;
    uint64_t left = entry.data_size > offset ? entry.data_size - offset : 0;
    if (left < len) len = size_t(left);
    std::vector<uint8_t> bytes;
    std::string error;
    if (!read(romfs->data + entry.data_offset + offset, len, bytes, error)) return std::nullopt;
    return bytes;
}

bool decompress(const std::vector<uint8_t> &compressed, std::vector<uint8_t> &result, std::string &error) {
    size_t size = compressed.size();
    if (size < 8) {
        error = "compressed code shorter than its footer";
        return false;
    }
    size_t out_size = size + u32_at(compressed, size - 4);
    uint32_t footer = u32_at(compressed, size - 8);
    // the high byte counts the footer and padding, the rest how far back the
    // compressed part starts
    size_t footer_size = footer >> 24;
    size_t span = footer & 0x00FFFFFF;
    if (footer_size > size || span > size) {
        error = "the compressed code's footer points outside it";
        return false;
    }

    std::vector<uint8_t> out(out_size, 0);
    std::memcpy(out.data(), compressed.data(), size);
    size_t index = size - footer_size;
    size_t stop = size - span;
    size_t cursor = out_size;
    while (index > stop) {
        index -= 1;
        uint8_t control = compressed[index];
        for (int bit = 0; bit < 8; bit++) {
            if (index <= stop || index == 0 || cursor == 0) break;
            if (control & 0x80) {
                if (index < 2) {
                    error = "a back reference is cut short";
                    return false;
                }
                index -= 2;
                size_t pair = u16_at(compressed.data(), index);
                // runs are never shorter than three, nor closer than two
                size_t length = (pair >> 12) + 3, distance = (pair & 0xFFF) + 2;
                if (cursor < length) {
                    error = "a run goes past the start";
                    return false;
                }
                for (size_t i = 0; i < length; i++) {
                    size_t from = cursor + distance;
                    if (from >= out.size()) {
                        error = "a run reads past the end";
                        return false;
                    }
                    uint8_t byte = out[from];
                    cursor -= 1;
                    out[cursor] = byte;
                }
            } else {
                index -= 1;
                cursor -= 1;
                out[cursor] = compressed[index];
            }
            control = uint8_t(control << 1);
        }
    }
    result = std::move(out);
    return true;
}

} // namespace recomp
