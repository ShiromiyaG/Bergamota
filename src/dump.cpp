// A game's dumps as the 3DS lays them out: the executable NCCH of a .3ds
// (NCSD), .cxi or CIA, its program, icon and RomFS, a CIA's TMD, and an
// update laid out for 3dsrecomp. Translated from zakuro-fs's NCSD, NCCH,
// CIA, ExeFS, RomFS and LZ77 readers (Rust, MIT, Copyright (c) 2026 fearkov;
// see LICENSE-zakuro). The launcher builds this file alone.

#include "runtime/dump.h"

#include <cstring>
#include <fstream>

namespace runtime {

namespace fsys = std::filesystem;

namespace {

template <typename T> T read_le(std::ifstream &in, uint64_t at) {
    T v{};
    in.seekg(static_cast<std::streamoff>(at));
    in.read(reinterpret_cast<char *>(&v), sizeof v);
    return v;
}

uint64_t align(uint64_t v, uint64_t a) { return (v + a - 1) & ~(a - 1); }

} // namespace

// where the executable NCCH of a .3ds (its first partition) or a CIA (its
// first content) starts in it
static std::optional<uint64_t> find_ncch(std::ifstream &in, const fsys::path &dump, std::string &error) {
    if (!in) {
        error = "cannot open " + dump.string();
        return std::nullopt;
    }
    uint64_t ncch;
    char magic[4];
    in.seekg(0x100);
    in.read(magic, 4);
    if (std::memcmp(magic, "NCSD", 4) == 0) {
        ncch = uint64_t(read_le<uint32_t>(in, 0x120)) * 0x200; // the first partition
    } else if (std::memcmp(magic, "NCCH", 4) == 0) { // a .cxi: the NCCH alone
        ncch = 0;
    } else if (read_le<uint32_t>(in, 0) == 0x2020) {           // a CIA: its first content
        uint64_t certs = read_le<uint32_t>(in, 0x08), ticket = read_le<uint32_t>(in, 0x0C),
                 tmd = read_le<uint32_t>(in, 0x10);
        uint64_t tmd_offset = align(align(align(0x2020, 64) + certs, 64) + ticket, 64);
        ncch = align(tmd_offset + tmd, 64);
    } else {
        error = dump.string() + " is neither a .3ds nor a CIA";
        return std::nullopt;
    }
    in.seekg(static_cast<std::streamoff>(ncch + 0x100));
    in.read(magic, 4);
    if (std::memcmp(magic, "NCCH", 4) != 0) {
        error = dump.string() + ": no NCCH where one should be (is it decrypted?)";
        return std::nullopt;
    }
    return ncch;
}

// backwards LZSS ("BLZ"), as the SDK compresses an ExeFS's .code: from the
// end back, a control byte, then per bit a byte copied or a run from what
// was already written. Translated from zakuro-fs's lz77.rs
bool blz_decompress(std::vector<uint8_t> &data, std::string &error) {
    size_t size = data.size();
    if (size < 8) {
        error = "the code is shorter than its footer";
        return false;
    }
    auto u32 = [&](size_t at) { return uint32_t(data[at]) | data[at + 1] << 8 | data[at + 2] << 16 | uint32_t(data[at + 3]) << 24; };
    uint32_t footer = u32(size - 8);
    size_t footer_size = footer >> 24, payload = footer & 0xFFFFFF, out_size = size + u32(size - 4);
    if (footer_size > size || payload > size) {
        error = "the code's footer points outside it";
        return false;
    }
    std::vector<uint8_t> in = data;
    data.resize(out_size);
    size_t index = size - footer_size, stop = size - payload, out = out_size;
    while (index > stop) {
        uint8_t control = in[--index];
        for (int bit = 0; bit < 8; bit++, control <<= 1) {
            if (index <= stop || index == 0 || out == 0) break;
            if (control & 0x80) {
                if (index < 2) {
                    error = "the code's back-reference is cut short";
                    return false;
                }
                index -= 2;
                unsigned raw = in[index] | in[index + 1] << 8;
                size_t run = ((raw >> 12) & 0xF) + 3, offset = (raw & 0xFFF) + 2;
                if (out < run) {
                    error = "the code's run goes before its start";
                    return false;
                }
                for (size_t i = 0; i < run; i++) {
                    if (out + offset >= out_size) {
                        error = "the code's run reads past its end";
                        return false;
                    }
                    uint8_t byte = data[out + offset];
                    data[--out] = byte;
                }
            } else {
                data[--out] = in[--index];
            }
        }
    }
    return true;
}

bool read_program(const fsys::path &dump, std::vector<uint8_t> &exheader, std::vector<uint8_t> &code, std::string &error) {
    std::ifstream in(dump, std::ios::binary);
    std::optional<uint64_t> ncch = find_ncch(in, dump, error);
    if (!ncch) return false;
    exheader.resize(0x800);
    in.seekg(static_cast<std::streamoff>(*ncch + 0x200));
    in.read(reinterpret_cast<char *>(exheader.data()), 0x800);
    uint64_t exefs = *ncch + uint64_t(read_le<uint32_t>(in, *ncch + 0x1A0)) * 0x200;
    for (int i = 0; i < 10; i++) {
        char name[8];
        in.seekg(static_cast<std::streamoff>(exefs + i * 0x10));
        in.read(name, 8);
        if (std::strncmp(name, ".code", 8) != 0) continue;
        uint32_t offset = read_le<uint32_t>(in, exefs + i * 0x10 + 8), size = read_le<uint32_t>(in, exefs + i * 0x10 + 12);
        code.resize(size);
        in.seekg(static_cast<std::streamoff>(exefs + 0x200 + offset));
        in.read(reinterpret_cast<char *>(code.data()), size);
        if (!in) {
            error = dump.string() + ": its code is cut short";
            return false;
        }
        // the exheader's flags, bit 0: the code is compressed; once it is
        // not, it says so (X and Y's, Omega Ruby and Alpha Sapphire's are)
        if (!(exheader[0x0D] & 1)) return true;
        if (!blz_decompress(code, error)) return false;
        exheader[0x0D] &= ~1;
        return true;
    }
    error = dump.string() + ": no code in its ExeFS";
    return false;
}

namespace {

// where the dump's SMDH (its ExeFS's "icon") starts, none without one
std::optional<uint64_t> find_smdh(std::ifstream &in, const fsys::path &dump) {
    std::string error;
    std::optional<uint64_t> ncch = find_ncch(in, dump, error);
    if (!ncch) return std::nullopt;
    uint64_t exefs = *ncch + uint64_t(read_le<uint32_t>(in, *ncch + 0x1A0)) * 0x200;
    for (int i = 0; i < 10; i++) {
        char name[8];
        in.seekg(static_cast<std::streamoff>(exefs + i * 0x10));
        in.read(name, 8);
        if (std::strncmp(name, "icon", 8) != 0) continue;
        uint64_t smdh = exefs + 0x200 + read_le<uint32_t>(in, exefs + i * 0x10 + 8);
        char magic[4];
        in.seekg(static_cast<std::streamoff>(smdh));
        in.read(magic, 4);
        if (!in || std::memcmp(magic, "SMDH", 4) != 0) return std::nullopt;
        return smdh;
    }
    return std::nullopt;
}

// an SMDH icon: RGB565 in 8 x 8 tiles, row by row, each tile's pixels in
// Morton (Z) order
bool read_tiled(std::ifstream &in, uint64_t at, int size, std::vector<uint32_t> &out) {
    std::vector<uint8_t> raw(size_t(size) * size * 2);
    in.seekg(static_cast<std::streamoff>(at));
    in.read(reinterpret_cast<char *>(raw.data()), std::streamsize(raw.size()));
    if (!in) return false;
    out.assign(size_t(size) * size, 0);
    for (int y = 0; y < size; y++) {
        for (int x = 0; x < size; x++) {
            int tile = (y / 8) * (size / 8) + x / 8, tx = x % 8, ty = y % 8;
            int morton = (tx & 1) | (ty & 1) << 1 | (tx & 2) << 1 | (ty & 2) << 2 | (tx & 4) << 2 | (ty & 4) << 3;
            size_t i = (size_t(tile) * 64 + size_t(morton)) * 2;
            uint32_t v = uint32_t(raw[i]) | uint32_t(raw[i + 1]) << 8;
            uint32_t r = (v >> 11 & 0x1F) * 255 / 31, g = (v >> 5 & 0x3F) * 255 / 63, b = (v & 0x1F) * 255 / 31;
            out[size_t(y) * size + x] = r | g << 8 | b << 16 | 0xFFu << 24;
        }
    }
    return true;
}

} // namespace

std::string read_title(const fsys::path &dump) {
    std::ifstream in(dump, std::ios::binary);
    std::optional<uint64_t> smdh = find_smdh(in, dump);
    if (!smdh) return {};
    // the SMDH: "SMDH", then per language a short title (0x80 bytes of
    // UTF-16), a long one and the publisher; English is the second
    std::string title;
    for (int c = 0; c < 0x40; c++) {
        uint16_t unit = read_le<uint16_t>(in, *smdh + 0x08 + 0x200 + c * 2);
        if (!unit) break;
        // the titles are within the basic plane: one to three bytes each
        if (unit < 0x80) title += char(unit);
        else if (unit < 0x800) title += char(0xC0 | unit >> 6), title += char(0x80 | (unit & 0x3F));
        else title += char(0xE0 | unit >> 12), title += char(0x80 | (unit >> 6 & 0x3F)), title += char(0x80 | (unit & 0x3F));
    }
    return title;
}

bool read_icon(const fsys::path &dump, Icon &icon) {
    std::ifstream in(dump, std::ios::binary);
    std::optional<uint64_t> smdh = find_smdh(in, dump);
    // after the titles and settings, the small icon at 0x2040, the large one
    // at 0x24C0
    return smdh && read_tiled(in, *smdh + 0x2040, 24, icon.small) && read_tiled(in, *smdh + 0x24C0, 48, icon.large);
}

std::optional<RomFsImage> find_romfs(const fsys::path &dump, std::string &error) {
    std::ifstream in(dump, std::ios::binary);
    std::optional<uint64_t> found = find_ncch(in, dump, error);
    if (!found) return std::nullopt;
    uint64_t ncch = *found;
    char magic[4];
    uint64_t romfs = ncch + uint64_t(read_le<uint32_t>(in, ncch + 0x1B0)) * 0x200;
    uint64_t romfs_size = uint64_t(read_le<uint32_t>(in, ncch + 0x1B4)) * 0x200;
    in.seekg(static_cast<std::streamoff>(romfs));
    in.read(magic, 4);
    if (!romfs_size || std::memcmp(magic, "IVFC", 4) != 0) {
        error = dump.string() + ": no RomFS (is it decrypted?)";
        return std::nullopt;
    }
    // levels from 0x0C, 0x18 apart; +0x10 in a level is its block size, log2
    uint64_t master = read_le<uint32_t>(in, romfs + 0x08);
    uint32_t block = read_le<uint32_t>(in, romfs + 0x0C + 2 * 0x18 + 0x10);
    uint64_t level3 = align(0x60 + master, uint64_t(1) << block);
    return RomFsImage{dump, romfs + level3, romfs_size - level3};
}

namespace {

template <typename T> T read_be(std::ifstream &in, uint64_t at) {
    uint8_t bytes[sizeof(T)] = {};
    in.seekg(static_cast<std::streamoff>(at));
    in.read(reinterpret_cast<char *>(bytes), sizeof(T));
    T value = 0;
    for (uint8_t b : bytes) value = T(value << 8 | b);
    return value;
}

// a CIA's TMD: its title id, version and where its first content starts,
// and whether any content in the file is encrypted. Translated from
// zakuro-fs's cia.rs
struct Tmd {
    uint64_t title_id = 0, first = 0;
    uint16_t version = 0;
    bool encrypted = false;
};

std::optional<Tmd> read_tmd(std::ifstream &in) {
    const uint64_t certs = read_le<uint32_t>(in, 0x08), ticket = read_le<uint32_t>(in, 0x0C),
                   size = read_le<uint32_t>(in, 0x10);
    const uint64_t at = align(align(align(0x2020, 64) + certs, 64) + ticket, 64);
    // the signature's size goes by its type, padded to 64 bytes
    uint64_t signature;
    switch (read_be<uint32_t>(in, at) & 0xFFFF) {
    case 0: case 3: signature = 4 + 0x200 + 0x3C; break;
    case 1: case 4: signature = 4 + 0x100 + 0x3C; break;
    case 2: case 5: signature = 4 + 0x3C + 0x40; break;
    default: return std::nullopt;
    }
    const uint64_t body = at + signature;
    Tmd tmd;
    tmd.title_id = read_be<uint64_t>(in, body + 0x4C);
    tmd.version = read_be<uint16_t>(in, body + 0x9C);
    tmd.first = align(at + size, 64);
    // the contents' records, those the header's bitmap says are in the file
    const uint64_t records = body + 0xC4 + 64 * 0x24;
    for (uint32_t i = 0, count = read_be<uint16_t>(in, body + 0x9E); i < count && i < 64; i++) {
        const uint16_t index = read_be<uint16_t>(in, records + i * 0x30 + 4);
        const uint16_t flags = read_be<uint16_t>(in, records + i * 0x30 + 6);
        const uint8_t present = read_le<uint8_t>(in, 0x20 + index / 8);
        if (present & (0x80 >> (index % 8)) && flags & 1) tmd.encrypted = true;
    }
    if (!in) return std::nullopt;
    return tmd;
}

} // namespace

DumpProblem read_dump(const fsys::path &dump, DumpInfo &info, std::string &detail) {
    std::ifstream in(dump, std::ios::binary);
    if (!in) {
        detail = "it can't be opened";
        return DumpProblem::Unreadable;
    }
    info = DumpInfo{};
    if (read_le<uint32_t>(in, 0) == 0x2020) {
        auto tmd = read_tmd(in);
        if (!tmd) {
            detail = "its TMD can't be read";
            return DumpProblem::Unreadable;
        }
        info = DumpInfo{tmd->title_id, tmd->version, true};
        if (tmd->encrypted) return DumpProblem::Encrypted;
    }
    std::string error;
    std::optional<uint64_t> ncch = find_ncch(in, dump, error);
    if (!ncch) {
        if (info.cia) {
            detail = "its first content is no NCCH";
            return DumpProblem::Unreadable;
        }
        return DumpProblem::Unknown;
    }
    // the NCCH's flags: bit 2 of the last, stored in the clear
    if (!(read_le<uint8_t>(in, *ncch + 0x18F) & 4)) return DumpProblem::Encrypted;
    if (!info.cia) info.title_id = read_le<uint64_t>(in, *ncch + 0x118);
    return DumpProblem::None;
}

bool unpack_update(const fsys::path &cia, const fsys::path &dir, std::string &error) {
    std::vector<uint8_t> exheader, code;
    if (!read_program(cia, exheader, code, error)) return false;
    std::error_code ec;
    fsys::create_directories(dir, ec);
    auto write = [&](const fsys::path &path, const uint8_t *data, size_t size) {
        std::ofstream out(path, std::ios::binary);
        out.write(reinterpret_cast<const char *>(data), static_cast<std::streamsize>(size));
        if (!out) error = "could not write " + path.string();
        return bool(out);
    };
    if (!write(dir / "exheader.bin", exheader.data(), exheader.size()) || !write(dir / "code.bin", code.data(), code.size()))
        return false;
    auto romfs = find_romfs(cia, error);
    if (!romfs) return false;
    // the level 3 header: where the directories, the files and their data are
    std::ifstream in(cia, std::ios::binary);
    const uint64_t base = romfs->offset;
    if (read_le<uint32_t>(in, base) != 0x28) {
        error = cia.string() + ": its RomFS has no level 3 header";
        return false;
    }
    auto table = [&](uint32_t at) {
        std::vector<uint8_t> bytes(read_le<uint32_t>(in, base + at + 4));
        in.seekg(static_cast<std::streamoff>(base + read_le<uint32_t>(in, base + at)));
        in.read(reinterpret_cast<char *>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
        return bytes;
    };
    const std::vector<uint8_t> dirs = table(0x0C), files = table(0x1C);
    const uint64_t data = base + read_le<uint32_t>(in, base + 0x24);
    auto u32 = [](const std::vector<uint8_t> &t, uint32_t at) {
        return at + 4 <= t.size() ? uint32_t(t[at] | t[at + 1] << 8 | t[at + 2] << 16 | uint32_t(t[at + 3]) << 24) : 0xFFFFFFFFu;
    };
    auto u64 = [&](const std::vector<uint8_t> &t, uint32_t at) { return uint64_t(u32(t, at)) | uint64_t(u32(t, at + 4)) << 32; };
    // a name, UTF-16 of the basic plane, as UTF-8
    auto name = [](const std::vector<uint8_t> &t, uint32_t at, uint32_t size) {
        std::string s;
        for (uint32_t i = 0; i + 1 < size && at + i + 1 < t.size(); i += 2) {
            const uint32_t unit = t[at + i] | t[at + i + 1] << 8;
            if (unit < 0x80) s += char(unit);
            else if (unit < 0x800) s += char(0xC0 | unit >> 6), s += char(0x80 | (unit & 0x3F));
            else s += char(0xE0 | unit >> 12), s += char(0x80 | (unit >> 6 & 0x3F)), s += char(0x80 | (unit & 0x3F));
        }
        return s;
    };
    const uint32_t NONE = 0xFFFFFFFFu;
    auto as_path = [](const std::string &s) { return fsys::path(std::u8string(s.begin(), s.end())); };
    // the directories to go through, each with where its files go
    std::vector<std::pair<uint32_t, fsys::path>> pending{{0, dir / "romfs"}};
    for (size_t guard = 0; !pending.empty() && guard < 100000; guard++) {
        auto [at, out] = pending.back();
        pending.pop_back();
        for (uint32_t file = u32(dirs, at + 0x0C), n = 0; file != NONE && n < 100000; file = u32(files, file + 0x04), n++) {
            const std::string file_name = name(files, file + 0x20, u32(files, file + 0x1C));
            const bool module = file_name.ends_with(".cro") || file_name.ends_with(".crr") || file_name == "static.crs";
            if (!module) continue;
            std::vector<uint8_t> bytes(u64(files, file + 0x10));
            in.seekg(static_cast<std::streamoff>(data + u64(files, file + 0x08)));
            in.read(reinterpret_cast<char *>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
            if (!in) {
                error = cia.string() + ": " + file_name + " is cut short";
                return false;
            }
            fsys::create_directories(out, ec);
            if (!write(out / as_path(file_name), bytes.data(), bytes.size())) return false;
        }
        for (uint32_t sub = u32(dirs, at + 0x08), n = 0; sub != NONE && n < 100000; sub = u32(dirs, sub + 0x04), n++)
            pending.emplace_back(sub, out / as_path(name(dirs, sub + 0x18, u32(dirs, sub + 0x14))));
    }
    return true;
}

} // namespace runtime
