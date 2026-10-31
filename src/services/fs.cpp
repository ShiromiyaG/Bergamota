// fs:USER. Translated from Zakuro's services/fs.rs and host_archive.rs,
// and zakuro-fs (Rust, MIT, Copyright (c) 2026 fearkov; see LICENSE-zakuro).

#include "runtime/fs.h"

#include <algorithm>
#include <cstring>
#include <fstream>

namespace runtime {

namespace fsys = std::filesystem;

namespace {

constexpr uint32_t ARCHIVE_SELF_NCCH = 3, ARCHIVE_SAVEDATA = 4, ARCHIVE_EXTDATA = 6, ARCHIVE_SHARED_EXTDATA = 7,
                   ARCHIVE_SYSTEM_SAVEDATA = 8, ARCHIVE_SDMC = 9, ARCHIVE_NCCH = 0x2345678A;
constexpr uint32_t PATH_EMPTY = 1, PATH_BINARY = 2, PATH_ASCII = 3, PATH_UTF16 = 4;
constexpr uint32_t SELF_NCCH_ROMFS = 0, SELF_NCCH_UPDATE_ROMFS = 5;
constexpr uint32_t OPEN_CREATE = 1u << 2;
constexpr uint32_t ENTRY_SIZE = 0x228;
constexpr uint32_t PLAY_COINS = 0xF000000B;

namespace code {
constexpr uint32_t NOT_FOUND = 0xC8804478, ARCHIVE_NOT_MOUNTED = 0xC8A0445A, FILE_NOT_FOUND = 0xC8804470,
                   PATH_NOT_FOUND = 0xC8804471, NOT_FOUND_INVALID_STATE = 0xC8A04478, NOT_FORMATTED = 0xC8A04554,
                   FILE_ALREADY_EXISTS = 0xC82044B4, DIRECTORY_ALREADY_EXISTS = 0xC82044B9,
                   DIRECTORY_NOT_EMPTY = 0xC92044F0, UNEXPECTED_FILE_OR_DIRECTORY = 0xE0C04702,
                   INVALID_PATH = 0xE0E046BE, NOT_IMPLEMENTED = 0xD960442C;
}

template <typename T> T read_le(std::ifstream &in, uint64_t at) {
    T v{};
    in.seekg(static_cast<std::streamoff>(at));
    in.read(reinterpret_cast<char *>(&v), sizeof v);
    return v;
}

uint64_t align(uint64_t v, uint64_t a) { return (v + a - 1) & ~(a - 1); }

uint64_t u64(const CommandBuffer &b, uint32_t i) { return uint64_t(b.get(i)) | uint64_t(b.get(i + 1)) << 32; }

std::vector<uint8_t> path_bytes(Memory &m, uint32_t size, uint32_t pointer) {
    std::vector<uint8_t> bytes(std::min(size, 0x400u));
    if (!bytes.empty()) m.read_bytes(pointer, bytes.data(), static_cast<uint32_t>(bytes.size()));
    return bytes;
}

std::string decode_path(Memory &m, uint32_t type, uint32_t size, uint32_t pointer) {
    if (type == PATH_ASCII) return m.read_cstring(pointer, size);
    if (type == PATH_UTF16) {
        std::string out;
        for (uint32_t i = 0; i + 1 < size; i += 2) {
            uint16_t unit = m.read16(pointer + i);
            if (!unit) break;
            // the games' save paths are ASCII; anything else as UTF-8
            if (unit < 0x80) out.push_back(char(unit));
            else if (unit < 0x800) {
                out.push_back(char(0xC0 | unit >> 6));
                out.push_back(char(0x80 | (unit & 0x3F)));
            } else {
                out.push_back(char(0xE0 | unit >> 12));
                out.push_back(char(0x80 | ((unit >> 6) & 0x3F)));
                out.push_back(char(0x80 | (unit & 0x3F)));
            }
        }
        return out;
    }
    return {};
}

// a path inside an archive on the host, refusing anything that steps out
std::optional<fsys::path> resolve(const fsys::path &root, const std::string &path) {
    fsys::path host = root;
    size_t start = 0;
    while (start <= path.size()) {
        size_t end = path.find_first_of("/\\", start);
        if (end == std::string::npos) end = path.size();
        std::string part = path.substr(start, end - start);
        if (part == "..") return std::nullopt;
        if (!part.empty() && part != ".") host /= fsys::u8path(part);
        start = end + 1;
    }
    return host;
}

fsys::path format_path(const fsys::path &root) {
    fsys::path p = root;
    p += ".format";
    return p;
}

void format(const fsys::path &root, uint32_t total, uint32_t dirs, uint32_t files, bool duplicate) {
    std::error_code e;
    fsys::remove_all(root, e);
    fsys::create_directories(root, e);
    uint32_t info[4] = {total, dirs, files, duplicate};
    std::ofstream(format_path(root), std::ios::binary).write(reinterpret_cast<char *>(info), 16);
}

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
static bool blz_decompress(std::vector<uint8_t> &data, std::string &error) {
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
        // the exheader's flags, bit 0: the code is compressed
        return !(exheader[0x0D] & 1) || blz_decompress(code, error);
    }
    error = dump.string() + ": no code in its ExeFS";
    return false;
}

std::string read_title(const fsys::path &dump) {
    std::ifstream in(dump, std::ios::binary);
    std::string error;
    std::optional<uint64_t> ncch = find_ncch(in, dump, error);
    if (!ncch) return {};
    uint64_t exefs = *ncch + uint64_t(read_le<uint32_t>(in, *ncch + 0x1A0)) * 0x200;
    for (int i = 0; i < 10; i++) {
        char name[8];
        in.seekg(static_cast<std::streamoff>(exefs + i * 0x10));
        in.read(name, 8);
        if (std::strncmp(name, "icon", 8) != 0) continue;
        // the SMDH: "SMDH", then per language a short title (0x80 bytes of
        // UTF-16), a long one and the publisher; English is the second
        uint64_t smdh = exefs + 0x200 + read_le<uint32_t>(in, exefs + i * 0x10 + 8);
        char magic[4];
        in.seekg(static_cast<std::streamoff>(smdh));
        in.read(magic, 4);
        if (std::memcmp(magic, "SMDH", 4) != 0) return {};
        std::string title;
        for (int c = 0; c < 0x40; c++) {
            uint16_t unit = read_le<uint16_t>(in, smdh + 0x08 + 0x200 + c * 2);
            if (!unit) break;
            // the titles are within the basic plane: one to three bytes each
            if (unit < 0x80) title += char(unit);
            else if (unit < 0x800) title += char(0xC0 | unit >> 6), title += char(0x80 | (unit & 0x3F));
            else title += char(0xE0 | unit >> 12), title += char(0x80 | (unit >> 6 & 0x3F)), title += char(0x80 | (unit & 0x3F));
        }
        return title;
    }
    return {};
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

uint64_t Fs::File::size() const {
    switch (backing) {
    case Backing::Rom: return rom.size;
    case Backing::Host: {
        std::error_code e;
        auto s = fsys::file_size(host, e);
        return e ? 0 : s;
    }
    default: return bytes.size();
    }
}

Handle Fs::session(System &system, const char *service, uint32_t id) {
    return system.kernel.create_handle(system.kernel.insert(ClientSession{service, id}));
}

void Fs::reply_file(System &system, CommandBuffer &b, uint32_t command, uint32_t code, uint32_t id) {
    if (code) b.reply_error(command, code);
    else b.reply_with_handle(command, session(system, "FSFile", id));
}

std::optional<fsys::path> Fs::host_archive(uint32_t id, const std::vector<uint8_t> &path) const {
    auto word = [&](size_t i) {
        uint32_t v = 0;
        if (i + 4 <= path.size()) std::memcpy(&v, path.data() + i, 4);
        return v;
    };
    char name[17];
    switch (id) {
    case ARCHIVE_SAVEDATA:
        std::snprintf(name, sizeof name, "%016llX", (unsigned long long)program_id_);
        return user_ / "savedata" / name;
    case ARCHIVE_EXTDATA:
    case ARCHIVE_SHARED_EXTDATA: // path: media type, then the 64-bit id
        std::snprintf(name, sizeof name, "%016llX", (unsigned long long)(word(4) | uint64_t(word(8)) << 32));
        return (id == ARCHIVE_SHARED_EXTDATA ? user_ / "extdata" / "shared" : user_ / "extdata") / name;
    case ARCHIVE_SYSTEM_SAVEDATA:
        std::snprintf(name, sizeof name, "%016llX", (unsigned long long)(word(4) | uint64_t(word(0)) << 32));
        return user_ / "nand" / "system-save" / name;
    case ARCHIVE_SDMC: return user_ / "sdmc";
    default: return std::nullopt;
    }
}

// a writable archive, failing as a console does when it was never made:
// the games tell "create it" from "it is broken" by the code
uint32_t Fs::open_host_archive(uint32_t id, const std::vector<uint8_t> &path, fsys::path &out) const {
    auto root = host_archive(id, path);
    if (!root) return code::NOT_FOUND;
    std::error_code e;
    if (id == ARCHIVE_SDMC) {
        fsys::create_directories(*root, e);
        out = *root;
        return 0;
    }
    uint32_t extdata_id = 0;
    if (path.size() >= 8) std::memcpy(&extdata_id, path.data() + 4, 4);
    if (id == ARCHIVE_SHARED_EXTDATA && extdata_id == PLAY_COINS && !fsys::is_directory(*root)) {
        // the play coins every console has: 42 of them, no steps
        format(*root, 0, 0, 0, false);
        uint8_t coins[20] = {0x00, 0x4F, 0, 0, 42, 0};
        coins[16] = 2014 & 0xFF;
        coins[17] = 2014 >> 8;
        coins[18] = 12;
        coins[19] = 29;
        std::ofstream(*root / "gamecoin.dat", std::ios::binary).write(reinterpret_cast<char *>(coins), 20);
    }
    if (fsys::is_directory(*root)) {
        out = *root;
        return 0;
    }
    return id == ARCHIVE_EXTDATA ? code::NOT_FOUND_INVALID_STATE : code::NOT_FORMATTED;
}

uint32_t Fs::open_host_file(System &system, const fsys::path &root, uint32_t type, uint32_t size, uint32_t pointer,
                            uint32_t flags, uint32_t &id) {
    auto host = resolve(root, decode_path(system.memory, type, size, pointer));
    if (!host) return code::INVALID_PATH;
    if (fsys::is_directory(*host)) return code::UNEXPECTED_FILE_OR_DIRECTORY;
    if (!fsys::is_regular_file(*host)) {
        if (!fsys::is_directory(host->parent_path())) return code::PATH_NOT_FOUND;
        if (!(flags & OPEN_CREATE)) return code::FILE_NOT_FOUND;
        std::ofstream(*host, std::ios::binary);
    }
    id = ++next_file_;
    files_[id] = File{Backing::Host, {}, *host, {}};
    return 0;
}

std::optional<uint32_t> Fs::open_ncch(uint32_t kind) {
    // the update's RomFS when asked for it, the game's own otherwise
    const std::optional<RomFsImage> &image =
        kind == SELF_NCCH_UPDATE_ROMFS && update_romfs ? update_romfs : romfs;
    if ((kind != SELF_NCCH_ROMFS && kind != SELF_NCCH_UPDATE_ROMFS) || !image) return std::nullopt;
    uint32_t id = ++next_file_;
    files_[id] = File{Backing::Rom, *image, {}, {}};
    return id;
}

std::optional<uint32_t> Fs::open_rom_file(System &system, uint32_t archive, uint32_t type, uint32_t size,
                                          uint32_t pointer) {
    if (archive == ARCHIVE_SELF_NCCH) {
        // a binary path picks a part of the NCCH; an empty one is the RomFS
        uint32_t kind = type == PATH_BINARY && size >= 4 ? system.memory.read32(pointer) : SELF_NCCH_ROMFS;
        return open_ncch(kind);
    }
    if (archive == ARCHIVE_NCCH && type == PATH_BINARY && size >= 12) {
        // the system's shared data (fonts, bad words, Mii data): not there
        return std::nullopt;
    }
    return std::nullopt;
}

bool Fs::handle(System &system, Thread &thread, CommandBuffer &b, const std::string &service, uint32_t subhandle) {
    if (service == "FSFile") return file(system, thread, b, subhandle);
    if (service == "FSDirectory") return directory(system, b, subhandle);
    return user(system, thread, b);
}

bool Fs::user(System &system, Thread &, CommandBuffer &b) {
    Memory &m = system.memory;
    uint32_t command = b.command();
    switch (command) {
    case 0x0801: case 0x0861: // Initialize, InitializeWithSdkVersion
    case 0x0840: case 0x085A: case 0x085D: // AbnegateAccessRight, SetArchivePriority, SetFsCompatibilityInfo
    case 0x086E: // SetThisSaveDataSecureValue: nothing checks it
        b.reply(command);
        return true;
    case 0x0802: { // OpenFile(transaction, archive, path type, size, flags, attributes, <path>)
        auto it = archives_.find(u64(b, 2));
        uint32_t id = 0, result;
        if (it == archives_.end()) result = code::ARCHIVE_NOT_MOUNTED;
        else if (it->second.host) result = open_host_file(system, *it->second.host, b.get(4), b.get(5), b.get(9), b.get(6), id);
        else {
            auto opened = open_rom_file(system, it->second.id, b.get(4), b.get(5), b.get(9));
            result = opened ? 0 : code::NOT_FOUND;
            id = opened.value_or(0);
        }
        reply_file(system, b, command, result, id);
        return true;
    }
    case 0x0803: { // OpenFileDirectly(transaction, archive id, archive path type, size, file path type, size, flags, attributes, <archive path>, <file path>)
        uint32_t archive = b.get(2);
        std::vector<uint8_t> archive_path = path_bytes(m, b.get(4), b.get(10));
        uint32_t id = 0, result;
        if (host_archive(archive, archive_path)) {
            fsys::path root;
            result = open_host_archive(archive, archive_path, root);
            if (!result) result = open_host_file(system, root, b.get(5), b.get(6), b.get(12), b.get(7), id);
        } else {
            auto opened = open_rom_file(system, archive, b.get(5), b.get(6), b.get(12));
            result = opened ? 0 : code::NOT_FOUND;
            id = opened.value_or(0);
        }
        reply_file(system, b, command, result, id);
        return true;
    }
    case 0x0804: case 0x0806: case 0x0807: { // DeleteFile, DeleteDirectory(Recursively)
        auto it = archives_.find(u64(b, 2));
        uint32_t result = 0;
        if (it == archives_.end() || !it->second.host) {
            result = it == archives_.end() ? code::ARCHIVE_NOT_MOUNTED : code::UNEXPECTED_FILE_OR_DIRECTORY;
        } else if (auto host = resolve(*it->second.host, decode_path(m, b.get(4), b.get(5), b.get(7)))) {
            std::error_code e;
            if (command == 0x0804) {
                if (!fsys::is_regular_file(*host)) result = code::FILE_NOT_FOUND;
                else fsys::remove(*host, e);
            } else if (!fsys::is_directory(*host) || *host == *it->second.host) {
                result = code::PATH_NOT_FOUND;
            } else if (command == 0x0807) {
                fsys::remove_all(*host, e);
            } else if (!fsys::is_empty(*host, e)) {
                result = code::DIRECTORY_NOT_EMPTY;
            } else {
                fsys::remove(*host, e);
            }
        } else {
            result = code::INVALID_PATH;
        }
        if (result) b.reply_error(command, result);
        else b.reply(command);
        return true;
    }
    case 0x0805: case 0x080A: { // RenameFile, RenameDirectory (within one archive)
        auto it = archives_.find(u64(b, 2));
        uint32_t result = 0;
        if (it == archives_.end() || !it->second.host) {
            result = code::ARCHIVE_NOT_MOUNTED;
        } else {
            auto from = resolve(*it->second.host, decode_path(m, b.get(4), b.get(5), b.get(11)));
            auto to = resolve(*it->second.host, decode_path(m, b.get(8), b.get(9), b.get(13)));
            bool is_file = command == 0x0805;
            if (!from || !to) result = code::INVALID_PATH;
            else if (is_file ? !fsys::is_regular_file(*from) : !fsys::is_directory(*from))
                result = is_file ? code::FILE_NOT_FOUND : code::PATH_NOT_FOUND;
            else if (fsys::exists(*to)) result = is_file ? code::FILE_ALREADY_EXISTS : code::DIRECTORY_ALREADY_EXISTS;
            else {
                std::error_code e;
                fsys::rename(*from, *to, e);
            }
        }
        if (result) b.reply_error(command, result);
        else b.reply(command);
        return true;
    }
    case 0x0808: case 0x0809: { // CreateFile(…, size u64, <path>), CreateDirectory(…, <path>)
        auto it = archives_.find(u64(b, 2));
        uint32_t result = 0;
        auto host = it != archives_.end() && it->second.host
                        ? resolve(*it->second.host, decode_path(m, b.get(4), b.get(5), b.get(command == 0x0808 ? 10 : 8)))
                        : std::nullopt;
        if (it == archives_.end() || !it->second.host) result = code::ARCHIVE_NOT_MOUNTED;
        else if (!host) result = code::INVALID_PATH;
        else if (fsys::exists(*host)) result = command == 0x0808 ? code::FILE_ALREADY_EXISTS : code::DIRECTORY_ALREADY_EXISTS;
        else if (!fsys::is_directory(host->parent_path())) result = code::PATH_NOT_FOUND;
        else if (command == 0x0808) {
            std::ofstream(*host, std::ios::binary);
            std::error_code e;
            fsys::resize_file(*host, u64(b, 7), e);
        } else {
            std::error_code e;
            fsys::create_directory(*host, e);
        }
        if (result) b.reply_error(command, result);
        else b.reply(command);
        return true;
    }
    case 0x080B: { // OpenDirectory(archive, path type, size, <path>)
        auto it = archives_.find(u64(b, 1));
        auto host = it != archives_.end() && it->second.host
                        ? resolve(*it->second.host, decode_path(m, b.get(3), b.get(4), b.get(6)))
                        : std::nullopt;
        if (!host || !fsys::is_directory(*host)) {
            b.reply_error(command, code::PATH_NOT_FOUND);
            return true;
        }
        Directory d;
        for (auto &entry : fsys::directory_iterator(*host)) {
            bool dir = entry.is_directory();
            d.entries.push_back(Entry{entry.path().filename().string(), dir, dir ? 0 : entry.file_size()});
        }
        std::sort(d.entries.begin(), d.entries.end(), [](auto &a, auto &c) { return a.name < c.name; });
        uint32_t id = ++next_directory_;
        directories_[id] = std::move(d);
        b.reply_with_handle(command, session(system, "FSDirectory", id));
        return true;
    }
    case 0x080C: { // OpenArchive(archive id, path type, size, <path>)
        uint32_t id = b.get(1);
        std::vector<uint8_t> path = path_bytes(m, b.get(3), b.get(5));
        Archive archive{id, std::nullopt};
        if (host_archive(id, path)) {
            fsys::path root;
            if (uint32_t result = open_host_archive(id, path, root)) {
                b.reply_error(command, result);
                return true;
            }
            archive.host = root;
        }
        uint64_t handle = ++next_archive_;
        archives_[handle] = archive;
        b.set(0, header(command, 3, 0));
        b.set(1, result::SUCCESS);
        b.set(2, uint32_t(handle));
        b.set(3, uint32_t(handle >> 32));
        return true;
    }
    case 0x080D: { // ControlArchive: commits, which the host files need not
        uint32_t words[4] = {b.get(6), b.get(7), b.get(8), b.get(9)};
        b.set(0, header(command, 1, 4));
        b.set(1, result::SUCCESS);
        for (uint32_t i = 0; i < 4; i++) b.set(2 + i, words[i]);
        return true;
    }
    case 0x080E: // CloseArchive
        archives_.erase(u64(b, 1));
        b.reply(command);
        return true;
    case 0x080F: { // FormatThisUserSaveData(block size, directories, files, …, duplicate)
        std::vector<uint8_t> none;
        format(*host_archive(ARCHIVE_SAVEDATA, none), b.get(1) * 512, b.get(2), b.get(3), b.get(6) & 0xFF);
        b.reply(command);
        return true;
    }
    case 0x084C: { // FormatSaveData(archive id, path type, size, block size, dirs, files, …, duplicate, <path>)
        if (b.get(1) != ARCHIVE_SAVEDATA) {
            b.reply_error(command, code::INVALID_PATH);
            return true;
        }
        std::vector<uint8_t> none;
        format(*host_archive(ARCHIVE_SAVEDATA, none), b.get(4) * 512, b.get(5), b.get(6), b.get(9) & 0xFF);
        b.reply(command);
        return true;
    }
    case 0x0812: { // GetFreeBytes: plenty
        uint64_t free = 512ull << 20;
        b.reply(command, {uint32_t(free), uint32_t(free >> 32)});
        return true;
    }
    case 0x0813: // GetCardType: a CTR card
        b.reply(command, {0});
        return true;
    case 0x0814: case 0x0815: case 0x0849: // the archive resources
        b.reply(command, {0x200, 0x4000, 0x80000, 0x80000});
        return true;
    case 0x0817: case 0x0818: case 0x0821: case 0x083D: // SD there and writable, card inserted, ext data allowed
        b.reply(command, {1});
        return true;
    case 0x0845: { // GetFormatInfo(archive id, path type, size, <path>)
        auto root = host_archive(b.get(1), path_bytes(m, b.get(3), b.get(5)));
        if (!root || !fsys::is_directory(*root)) {
            b.reply_error(command, code::NOT_FORMATTED);
            return true;
        }
        uint32_t info[4] = {};
        std::ifstream(format_path(*root), std::ios::binary).read(reinterpret_cast<char *>(info), 16);
        b.reply(command, {info[0], info[1], info[2], info[3] & 0xFF});
        return true;
    }
    case 0x0851: { // CreateExtSaveData(media type, id low, id high, …, dirs, files, …, <icon>)
        std::vector<uint8_t> path(12);
        for (uint32_t i = 0; i < 3; i++) {
            uint32_t w = b.get(1 + i);
            std::memcpy(path.data() + i * 4, &w, 4);
        }
        auto root = host_archive(ARCHIVE_EXTDATA, path);
        if (!fsys::is_directory(*root)) format(*root, 0, b.get(5), b.get(6), false);
        uint32_t icon_descriptor = b.get(10), icon = b.get(11);
        b.set(0, header(command, 1, 2));
        b.set(1, result::SUCCESS);
        b.set(2, icon_descriptor);
        b.set(3, icon);
        return true;
    }
    case 0x0852: { // DeleteExtSaveData
        std::vector<uint8_t> path(12);
        for (uint32_t i = 0; i < 3; i++) {
            uint32_t w = b.get(1 + i);
            std::memcpy(path.data() + i * 4, &w, 4);
        }
        auto root = host_archive(ARCHIVE_EXTDATA, path);
        std::error_code e;
        if (!fsys::is_directory(*root)) b.reply_error(command, code::NOT_FOUND_INVALID_STATE);
        else {
            fsys::remove_all(*root, e);
            fsys::remove(format_path(*root), e);
            b.reply(command);
        }
        return true;
    }
    case 0x0862: // SetPriority
        priority_ = b.get(1);
        b.reply(command);
        return true;
    case 0x0863: // GetPriority
        b.reply(command, {priority_});
        return true;
    case 0x086F: // GetThisSaveDataSecureValue: none, so the save is taken as it is
        b.reply(command, {0, 0, 0, 0});
        return true;
    default:
        return false;
    }
}

bool Fs::file(System &system, Thread &thread, CommandBuffer &b, uint32_t id) {
    Memory &m = system.memory;
    uint32_t command = b.command();
    auto it = files_.find(id);
    File *f = it == files_.end() ? nullptr : &it->second;
    switch (command) {
    case 0x0802: { // Read(offset u64, size, <mapped buffer>)
        uint64_t offset = u64(b, 1);
        uint32_t size = b.get(3), dest = b.get(5), read = 0;
        if (f && offset < f->size()) {
            uint32_t count = static_cast<uint32_t>(std::min<uint64_t>(size, f->size() - offset));
            std::vector<uint8_t> data(count);
            if (f->backing == Backing::Rom) {
                std::ifstream in(f->rom.file, std::ios::binary);
                in.seekg(static_cast<std::streamoff>(f->rom.offset + offset));
                in.read(reinterpret_cast<char *>(data.data()), count);
            } else if (f->backing == Backing::Host) {
                std::ifstream in(f->host, std::ios::binary);
                in.seekg(static_cast<std::streamoff>(offset));
                in.read(reinterpret_cast<char *>(data.data()), count);
                count = static_cast<uint32_t>(in.gcount());
            } else {
                std::memcpy(data.data(), f->bytes.data() + offset, count);
            }
            m.write_bytes(dest, data.data(), count);
            read = count;
        }
        b.set(0, header(command, 2, 2));
        b.set(1, result::SUCCESS);
        b.set(2, read);
        b.set(3, size << 4 | 0xC);
        b.set(4, dest);
        // the thread waits as long as the console's card would take, after
        // the reads it is still busy with: games that load on a worker
        // count on the others getting ahead meanwhile
        int medium = f && f->backing == Backing::Host ? 1 : 0;
        uint64_t now = system.ticks();
        uint64_t delay = nanos_to_ticks(std::max<uint64_t>(uint64_t(read) * 94 + 582778, 663124));
        busy_until_[medium] = std::max(busy_until_[medium], now) + delay;
        system.kernel.sleep(thread, busy_until_[medium] - now);
        return true;
    }
    case 0x0803: { // Write(offset u64, size, flags, <mapped buffer>)
        uint64_t offset = u64(b, 1);
        uint32_t size = b.get(3), source = b.get(6), written = 0;
        if (f && f->backing == Backing::Host) {
            std::vector<uint8_t> data(size);
            m.read_bytes(source, data.data(), size);
            std::fstream out(f->host, std::ios::binary | std::ios::in | std::ios::out);
            out.seekp(static_cast<std::streamoff>(offset));
            out.write(reinterpret_cast<char *>(data.data()), size);
            if (out) written = size;
        }
        b.set(0, header(command, 2, 2));
        b.set(1, result::SUCCESS);
        b.set(2, written);
        b.set(3, size << 4 | 0xA);
        b.set(4, source);
        return true;
    }
    case 0x0804: { // GetSize
        uint64_t size = f ? f->size() : 0;
        b.reply(command, {uint32_t(size), uint32_t(size >> 32)});
        return true;
    }
    case 0x0805: // SetSize
        if (f && f->backing == Backing::Host) {
            std::error_code e;
            fsys::resize_file(f->host, u64(b, 1), e);
        }
        b.reply(command);
        return true;
    case 0x0806: case 0x080B: // GetAttributes, GetPriority
        b.reply(command, {0});
        return true;
    case 0x0808: // Close
        files_.erase(id);
        b.reply(command);
        return true;
    case 0x0807: case 0x0809: case 0x080A: // SetAttributes, Flush, SetPriority
        b.reply(command);
        return true;
    case 0x080C: // OpenLinkFile: another session on the same file
        b.reply_with_handle(command, session(system, "FSFile", id));
        return true;
    default:
        return false;
    }
}

bool Fs::directory(System &system, CommandBuffer &b, uint32_t id) {
    uint32_t command = b.command();
    if (command == 0x0802) { // Close
        directories_.erase(id);
        b.reply(command);
        return true;
    }
    if (command != 0x0801) return false;
    // Read(count, <mapped buffer>): the UTF-16 name, an 8.3 name, flags, size
    uint32_t count = b.get(1), desc = b.get(2), dest = b.get(3), done = 0;
    auto it = directories_.find(id);
    if (it != directories_.end()) {
        Directory &d = it->second;
        for (; done < count && d.cursor < d.entries.size(); done++, d.cursor++) {
            const Entry &e = d.entries[d.cursor];
            uint8_t raw[ENTRY_SIZE] = {};
            for (size_t i = 0; i < e.name.size() && i < 0x105; i++) raw[i * 2] = uint8_t(e.name[i]);
            size_t dot = e.directory ? std::string::npos : e.name.rfind('.');
            std::string stem = e.name.substr(0, dot), ext = dot == std::string::npos ? "" : e.name.substr(dot + 1);
            for (size_t i = 0; i < stem.size() && i < 8; i++) raw[0x20C + i] = uint8_t(std::toupper(stem[i]));
            for (size_t i = 0; i < ext.size() && i < 3; i++) raw[0x216 + i] = uint8_t(std::toupper(ext[i]));
            raw[0x21A] = 1;
            raw[0x21C] = e.directory;
            raw[0x21E] = !e.directory;
            std::memcpy(raw + 0x220, &e.size, 8);
            system.memory.write_bytes(dest + done * ENTRY_SIZE, raw, ENTRY_SIZE);
        }
    }
    b.set(0, header(command, 2, 2));
    b.set(1, result::SUCCESS);
    b.set(2, done);
    b.set(3, desc);
    b.set(4, dest);
    return true;
}

} // namespace runtime
