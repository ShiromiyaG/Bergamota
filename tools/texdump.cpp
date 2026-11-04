// Dumps a game's textures without playing it: every file of its RomFS (the
// update's in place of the game's), opened through the containers the
// Pokémon games keep them in (GARC, LZ11, Game Freak's two-letter packs,
// darc), and the textures in BCH, CLIM/FLIM, CTPK and Sun and Moon's
// GFTexture, decoded with the runtime's own decoder and written as PNG. Nothing of a game goes into
// this repository: the PNGs are for the user's own copy.
//
//   texdump <game .3ds> [update .cia] <out folder>
//
// Each is named as Azahar names its dumps, tex1_<width>x<height>_<hash>_
// <format>_mip0.png: the hash is its new one (CityHash64 over the
// texture's first level as the GPU reads it, 8x8 tiles, the bytes the
// runtime sees in memory; runtime/texture_pack.h), format the PICA's (0
// RGBA8 ... 13 ETC1A4), the PNG's first row the first in memory, and a
// pack.json beside them, so the folder is a pack (the runtime's and
// Azahar's) as it is. Textures made while the game runs (the font, what it
// draws into) are not in its files.

#include "runtime/dump.h"
#include "runtime/texture_pack.h"
#include "../src/pica/texture.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace {

using Bytes = std::vector<uint8_t>;

uint32_t u16(const uint8_t *p) { return p[0] | p[1] << 8; }
uint32_t u32(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | uint32_t(p[3]) << 24; }


// --- PNG, stored (no compression): small to write, any viewer reads it

uint32_t crc32(const uint8_t *p, size_t n, uint32_t c = 0) {
    static uint32_t table[256];
    if (!table[1]) {
        for (uint32_t i = 0; i < 256; i++) {
            uint32_t v = i;
            for (int k = 0; k < 8; k++) v = v & 1 ? 0xEDB88320 ^ v >> 1 : v >> 1;
            table[i] = v;
        }
    }
    c = ~c;
    for (size_t i = 0; i < n; i++) c = table[(c ^ p[i]) & 0xFF] ^ c >> 8;
    return ~c;
}

void be32(Bytes &o, uint32_t v) { o.insert(o.end(), {uint8_t(v >> 24), uint8_t(v >> 16), uint8_t(v >> 8), uint8_t(v)}); }

void chunk(Bytes &o, const char *type, const Bytes &body) {
    be32(o, uint32_t(body.size()));
    size_t at = o.size();
    o.insert(o.end(), type, type + 4);
    o.insert(o.end(), body.begin(), body.end());
    be32(o, crc32(&o[at], o.size() - at));
}

bool write_png(const std::filesystem::path &path, const uint8_t *rgba, uint32_t w, uint32_t h) {
    Bytes raw;
    raw.reserve(size_t(w * 4 + 1) * h);
    for (uint32_t y = 0; y < h; y++) {
        raw.push_back(0);
        raw.insert(raw.end(), rgba + size_t(y) * w * 4, rgba + size_t(y + 1) * w * 4);
    }
    Bytes z{0x78, 0x01};
    uint32_t a = 1, b = 0;
    for (uint8_t c : raw) a = (a + c) % 65521, b = (b + a) % 65521;
    for (size_t at = 0; at < raw.size() || at == 0;) {
        size_t n = std::min<size_t>(65535, raw.size() - at);
        z.push_back(at + n == raw.size());
        z.insert(z.end(), {uint8_t(n), uint8_t(n >> 8), uint8_t(~n), uint8_t(~n >> 8)});
        z.insert(z.end(), raw.begin() + at, raw.begin() + at + n);
        at += n;
        if (!n) break;
    }
    be32(z, b << 16 | a);
    Bytes o{0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'}, ihdr;
    be32(ihdr, w), be32(ihdr, h);
    ihdr.insert(ihdr.end(), {8, 6, 0, 0, 0});
    chunk(o, "IHDR", ihdr);
    chunk(o, "IDAT", z);
    chunk(o, "IEND", {});
    std::ofstream f(path, std::ios::binary);
    return bool(f.write(reinterpret_cast<const char *>(o.data()), std::streamsize(o.size())));
}

// --- textures

struct Dumper {
    std::filesystem::path out;
    std::set<uint64_t> seen;
    size_t written = 0, repeated = 0;
    std::map<std::string, size_t> by_kind;

    // one texture: its format (the PICA's), size (powers of two) and data
    void texture(uint32_t format, uint32_t w, uint32_t h, const uint8_t *data, size_t avail, const char *kind) {
        if (format > 13 || w < 8 || h < 8 || w > 1024 || h > 1024 || (w & (w - 1)) || (h & (h - 1))) return;
        const size_t size = size_t(w) * h * runtime::pica::texture_bits(format) / 8;
        if (size > avail) return;
        const uint64_t hash = runtime::raw_texture_hash(data, size);
        if (!seen.insert(hash).second) {
            repeated++;
            return;
        }
        std::vector<uint8_t> rgba(size_t(w) * h * 4);
        runtime::pica::decode_texture(format, data, w, h, rgba.data());
        if (write_png(out / runtime::dump_name(w, h, hash, format), rgba.data(), w, h)) written++, by_kind[kind]++;
    }

    // BCH: its textures are the GPU's commands for texture unit 0 (size,
    // address, format), the address an offset into the data section
    void bch(const uint8_t *p, size_t n) {
        if (n < 0x44) return;
        const uint8_t back = p[4];
        size_t at = 8;
        auto word = [&] { uint32_t v = at + 4 <= n ? u32(p + at) : 0; at += 4; return v; };
        word(); // main header
        word(); // strings
        const uint32_t commands = word(), data = word();
        const uint32_t extended = back > 0x20 ? word() : 0;
        word(); // relocations
        word(), word();
        const uint32_t commands_size = word();
        if (commands >= n || data >= n) return;
        const size_t end = std::min<size_t>(n, size_t(commands) + commands_size);
        uint32_t dims = 0, address = 0;
        bool have_dims = false, have_address = false;
        for (size_t c = commands; c + 8 <= end;) {
            const uint32_t value = u32(p + c), header = u32(p + c + 4);
            const uint32_t id = header & 0xFFFF, extra = (header >> 20) & 0xFF;
            const bool consecutive = header >> 31;
            c += 8;
            auto set = [&](uint32_t reg, uint32_t v) {
                if (reg == 0x82) dims = v, have_dims = true;
                else if (reg == 0x85) address = v, have_address = true;
                else if (reg == 0x8E && have_dims && have_address) {
                    const uint32_t w = dims >> 16, h = dims & 0xFFFF;
                    for (uint32_t base : {data, extended}) {
                        if (base && size_t(base) + address < n) {
                            texture(v & 15, w, h, p + base + address, n - base - address, "BCH");
                            break;
                        }
                    }
                    have_dims = have_address = false;
                }
            };
            set(id, value);
            for (uint32_t k = 0; k < extra && c + 4 <= end; k++, c += 4) set(consecutive ? id + 1 + k : id, u32(p + c));
            if (extra & 1) c += 4; // padding to 8 bytes
        }
    }

    // CLIM / FLIM: the image first, a footer (header, then imag) at the end
    void lim(const uint8_t *p, size_t n) {
        if (n < 0x28) return;
        const uint8_t *imag = p + n - 0x14;
        if (std::memcmp(imag, "imag", 4)) return;
        static constexpr uint8_t PICA[14] = {7, 8, 9, 5, 6, 3, 1, 2, 4, 0, 12, 13, 10, 11};
        const uint32_t width = u16(imag + 8), height = u16(imag + 10);
        const uint32_t format = std::memcmp(p + n - 0x28, "FLIM", 4) ? u32(imag + 12) : imag[14];
        if (format > 13) return;
        auto pow2 = [](uint32_t v) { uint32_t r = 8; while (r < v) r <<= 1; return r; };
        // a FLIM may be laid out on its side (its swizzle)
        texture(PICA[format], pow2(width), pow2(height), p, n - 0x28, "CLIM/FLIM");
    }

    // CTPK: entries of 0x20 at 0x20
    void ctpk(const uint8_t *p, size_t n) {
        if (n < 0x20) return;
        const uint32_t count = u16(p + 6), section = u32(p + 8);
        for (uint32_t i = 0; i < count && 0x20 + (i + 1) * 0x20 <= n; i++) {
            const uint8_t *e = p + 0x20 + i * 0x20;
            const uint32_t size = u32(e + 4), offset = section + u32(e + 8);
            if (offset + size_t(size) <= n) texture(u32(e + 12), u16(e + 16), u16(e + 18), p + offset, size, "CTPK");
        }
    }

    // --- containers

    void scan(const uint8_t *p, size_t n, int depth) {
        if (n < 8 || depth > 8) return;
        if (!std::memcmp(p, "CRAG", 4)) return garc(p, n, depth);
        if (!std::memcmp(p, "darc", 4)) return darc(p, n, depth);
        if (!std::memcmp(p, "BCH", 4)) return bch(p, n);
        if (!std::memcmp(p, "CTPK", 4)) return ctpk(p, n);
        if (n >= 0x28 && (!std::memcmp(p + n - 0x28, "CLIM", 4) || !std::memcmp(p + n - 0x28, "FLIM", 4))) return lim(p, n);
        if (p[0] == 0x11) {
            Bytes d;
            if (lz11(p, n, d)) return scan(d.data(), d.size(), depth + 1);
        }
        if (pack(p, n, depth)) return;
        // anything else (Sun and Moon's model packs, a container not known
        // here): its GFTextures and BCHs wherever they start
        for (size_t at = 0; at + 0x80 <= n; at += 4) {
            if (u32(p + at) == 0x15041213) gftexture(p + at, n - at);
            else if (at && !std::memcmp(p + at, "BCH", 4)) bch(p + at, n - at);
        }
    }

    // Sun and Moon's GFTexture: a header of 0x80, then the data
    void gftexture(const uint8_t *p, size_t n) {
        const uint32_t length = u32(p + 0x18), width = u16(p + 0x68), height = u16(p + 0x6A), format = u16(p + 0x6C);
        uint32_t pica;
        switch (format) {
        case 0x02: pica = 3; break;
        case 0x03: pica = 1; break;
        case 0x04: pica = 0; break;
        case 0x16: pica = 4; break;
        case 0x17: pica = 2; break;
        case 0x23: pica = 5; break;
        case 0x24: pica = 6; break;
        case 0x25: pica = 7; break;
        case 0x26: pica = 8; break;
        case 0x27: pica = 9; break;
        case 0x28: pica = 10; break;
        case 0x29: pica = 11; break;
        case 0x2A: pica = 12; break;
        case 0x2B: pica = 13; break;
        default: return;
        }
        texture(pica, width, height, p + 0x80, std::min<size_t>(length, n - 0x80), "GFTexture");
    }

    // Game Freak's packs: two capital letters, a count, its offsets and the end
    bool pack(const uint8_t *p, size_t n, int depth) {
        if (!(p[0] >= 'A' && p[0] <= 'Z' && p[1] >= 'A' && p[1] <= 'Z')) return false;
        const uint32_t count = u16(p + 2);
        if (!count || 4 + size_t(count + 1) * 4 > n) return false;
        const uint32_t last = u32(p + 4 + count * 4);
        if (last > n || last + 0x80 < n) return false;
        for (uint32_t i = 0; i < count; i++) {
            uint32_t a = u32(p + 4 + i * 4), b = u32(p + 8 + i * 4);
            if (a > b || b > n) return false;
        }
        for (uint32_t i = 0; i < count; i++) {
            uint32_t a = u32(p + 4 + i * 4), b = u32(p + 8 + i * 4);
            scan(p + a, b - a, depth + 1);
        }
        return true;
    }

    void garc(const uint8_t *p, size_t n, int depth) {
        const uint32_t header = u32(p + 4), data = u32(p + 0x10);
        if (header + 12 > n) return;
        const uint8_t *fato = p + header;
        if (std::memcmp(fato, "OTAF", 4)) return;
        const uint32_t count = u16(fato + 8);
        const size_t fatb = header + u32(fato + 4);
        if (fatb + 12 > n || std::memcmp(p + fatb, "BTAF", 4)) return;
        for (uint32_t i = 0; i < count; i++) {
            if (header + 12 + (i + 1) * 4 > n) return;
            size_t at = fatb + 12 + u32(fato + 12 + i * 4);
            if (at + 4 > n) continue;
            const uint32_t mask = u32(p + at);
            at += 4;
            for (int bit = 0; bit < 32; bit++) {
                if (!(mask >> bit & 1)) continue;
                if (at + 12 > n) break;
                const uint32_t start = u32(p + at), end = u32(p + at + 4);
                at += 12;
                if (start <= end && size_t(data) + end <= n) scan(p + data + start, end - start, depth + 1);
            }
        }
    }

    void darc(const uint8_t *p, size_t n, int depth) {
        const uint32_t table = u32(p + 0x10);
        if (table + 12 > n) return;
        const uint32_t entries = u32(p + table + 8);
        for (uint32_t i = 1; i < entries && table + (i + 1) * 12 <= n; i++) {
            const uint8_t *e = p + table + i * 12;
            if (u32(e) >> 24) continue; // a folder
            const uint32_t offset = u32(e + 4), size = u32(e + 8);
            if (offset + size_t(size) <= n) scan(p + offset, size, depth + 1);
        }
    }

    static bool lz11(const uint8_t *p, size_t n, Bytes &o) {
        size_t size = p[1] | p[2] << 8 | p[3] << 16, at = 4;
        if (!size) {
            if (n < 8) return false;
            size = u32(p + 4), at = 8;
        }
        if (size > 64u << 20 || size < n) return false;
        o.clear();
        o.reserve(size);
        while (o.size() < size) {
            if (at >= n) return false;
            const uint8_t flags = p[at++];
            for (int k = 7; k >= 0 && o.size() < size; k--) {
                if (!(flags >> k & 1)) {
                    if (at >= n) return false;
                    o.push_back(p[at++]);
                    continue;
                }
                if (at + 2 > n) return false;
                uint32_t b0 = p[at], len, dist;
                if (b0 >> 4 == 0) {
                    if (at + 3 > n) return false;
                    len = ((b0 & 0xF) << 4 | p[at + 1] >> 4) + 0x11;
                    dist = ((p[at + 1] & 0xF) << 8 | p[at + 2]) + 1;
                    at += 3;
                } else if (b0 >> 4 == 1) {
                    if (at + 4 > n) return false;
                    len = ((b0 & 0xF) << 12 | p[at + 1] << 4 | p[at + 2] >> 4) + 0x111;
                    dist = ((p[at + 2] & 0xF) << 8 | p[at + 3]) + 1;
                    at += 4;
                } else {
                    len = (b0 >> 4) + 1;
                    dist = ((b0 & 0xF) << 8 | p[at + 1]) + 1;
                    at += 2;
                }
                if (dist > o.size()) return false;
                for (uint32_t i = 0; i < len && o.size() < size; i++) o.push_back(o[o.size() - dist]);
            }
        }
        return true;
    }
};

// --- RomFS (level 3): its files by path

struct RomFs {
    std::ifstream in;
    uint64_t base = 0;
    uint32_t dir_meta = 0, file_meta = 0, file_data = 0;
    Bytes dirs, files;

    bool open(const runtime::RomFsImage &image) {
        in.open(image.file, std::ios::binary);
        base = image.offset;
        Bytes h = read(0, 0x28);
        if (h.size() < 0x28) return false;
        dir_meta = u32(&h[12]), file_meta = u32(&h[28]), file_data = u32(&h[36]);
        dirs = read(dir_meta, u32(&h[16]));
        files = read(file_meta, u32(&h[32]));
        return !dirs.empty();
    }
    Bytes read(uint64_t at, size_t n) {
        Bytes b(n);
        in.clear();
        in.seekg(std::streamoff(base + at));
        in.read(reinterpret_cast<char *>(b.data()), std::streamsize(n));
        b.resize(size_t(in.gcount()));
        return b;
    }
    static std::string name(const uint8_t *p, uint32_t bytes) {
        std::string s;
        for (uint32_t i = 0; i + 1 < bytes; i += 2) s += char(p[i] < 0x80 && !p[i + 1] ? p[i] : '_');
        return s;
    }
    // path -> (offset, size)
    void list(uint32_t dir, const std::string &path, std::map<std::string, std::pair<uint64_t, uint64_t>> &out) {
        if (dir + 0x18 > dirs.size()) return;
        const uint8_t *d = &dirs[dir];
        for (uint32_t f = u32(d + 12); f != 0xFFFFFFFF && f + 0x20 <= files.size();) {
            const uint8_t *e = &files[f];
            uint64_t offset = u32(e + 8) | uint64_t(u32(e + 12)) << 32, size = u32(e + 16) | uint64_t(u32(e + 20)) << 32;
            out[path + name(e + 0x20, u32(e + 0x1C))] = {file_data + offset, size};
            f = u32(e + 4);
        }
        for (uint32_t c = u32(d + 8); c != 0xFFFFFFFF && c + 0x18 <= dirs.size();) {
            const uint8_t *e = &dirs[c];
            list(c, path + name(e + 0x18, u32(e + 0x14)) + "/", out);
            c = u32(e + 4);
        }
    }
};

} // namespace

int main(int argc, char **argv) {
    if (argc < 3) {
        std::fprintf(stderr, "texdump <game .3ds> [update .cia] <out folder>\n");
        return 2;
    }
    std::vector<std::string> dumps(argv + 1, argv + argc - 1);
    Dumper dumper{argv[argc - 1]};
    std::filesystem::create_directories(dumper.out);
    runtime::write_pack_template(dumper.out);
    // each path from the last dump that has it: the update over the game
    struct Source {
        RomFs fs;
        std::map<std::string, std::pair<uint64_t, uint64_t>> files;
    };
    std::vector<std::unique_ptr<Source>> sources;
    std::map<std::string, size_t> owner;
    for (const auto &path : dumps) {
        std::string error;
        auto image = runtime::find_romfs(path, error);
        auto s = std::make_unique<Source>();
        if (!image || !s->fs.open(*image)) {
            std::fprintf(stderr, "%s: no RomFS (%s)\n", path.c_str(), error.c_str());
            return 1;
        }
        s->fs.list(0, "", s->files);
        for (auto &[name, _] : s->files) owner[name] = sources.size();
        std::printf("%s: %zu files\n", path.c_str(), s->files.size());
        sources.push_back(std::move(s));
    }
    size_t done = 0;
    for (auto &[name, which] : owner) {
        Source &s = *sources[which];
        auto [offset, size] = s.files[name];
        if (size > (256u << 20)) continue;
        Bytes data = s.fs.read(offset, size_t(size));
        dumper.scan(data.data(), data.size(), 0);
        if (++done % 200 == 0) std::printf("%zu/%zu files, %zu textures\n", done, owner.size(), dumper.written);
    }
    std::printf("%zu textures written to %s (%zu repeated)\n", dumper.written, dumper.out.string().c_str(), dumper.repeated);
    for (auto &[kind, count] : dumper.by_kind) std::printf("  %s: %zu\n", kind.c_str(), count);
    return 0;
}
