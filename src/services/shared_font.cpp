// The console's shared font: the user's dump, or a stand-in. Translated
// from Zakuro's services/shared_font.rs and apt.rs (Rust, MIT, Copyright (c)
// 2026 fearkov; see LICENSE-zakuro); the host font is drawn with stb_truetype
// rather than ab_glyph, and the block is always laid out at 0 and moved,
// dump or stand-in alike.

#include "runtime/shared_font.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <thread>

#ifdef RUNTIME_HOST_FONT
#define STB_TRUETYPE_IMPLEMENTATION
#define STBTT_STATIC
#include "imstb_truetype.h"
#endif

namespace runtime {

namespace {

#include "shared_font_data.inc"

// where the BCFNT starts, past the status word and the rest of the header
constexpr uint32_t FONT_OFFSET = 0x80;
// a section's pointer is to its body, past its magic and size
constexpr uint32_t SECTION_BODY = 8;
// every glyph's cell; cells sit a pixel apart on the 256x256 sheets
constexpr uint32_t CELL = 24, STRIDE = CELL + 1, SHEET = 256, COLUMNS = SHEET / STRIDE, ROWS = SHEET / STRIDE;
constexpr uint32_t PER_SHEET = COLUMNS * ROWS, SHEET_SIZE = SHEET * SHEET / 2; // 4-bit alpha
constexpr uint16_t FORMAT_A4 = 11;
// the GPU takes a texture's address in 8-byte steps, the console's font
// has its sheets on 128
constexpr uint32_t SHEET_ALIGN = 0x80;
// the console's own block, which a title may map no more of
constexpr uint32_t BLOCK_LIMIT = 0x332000;
// characters in a row that get a map of their own
constexpr size_t DIRECT_RUN = 16;
// the bitmap font: each pixel three, 21 pixels on
constexpr uint32_t SCALE = 3;
constexpr uint8_t BITMAP_ADVANCE = 21;
// the host's font at 22 pixels an em, its baseline on row 20
constexpr float EM = 22.0f;
constexpr int BASELINE = 20;

struct Glyph {
    uint16_t code;
    uint8_t alpha[CELL * CELL]; // 0 to 15
    int8_t left;
    uint8_t width, advance;
};

struct Face {
    std::vector<Glyph> glyphs; // in the order of their characters
    uint8_t baseline, advance;
};

uint32_t align_up(uint32_t v, uint32_t a) { return (v + a - 1) / a * a; }
void put16(std::vector<uint8_t> &o, uint32_t at, uint32_t v) { o[at] = uint8_t(v), o[at + 1] = uint8_t(v >> 8); }
void put32(std::vector<uint8_t> &o, uint32_t at, uint32_t v) { put16(o, at, v), put16(o, at + 2, v >> 16); }
uint32_t get16(const std::vector<uint8_t> &o, uint32_t at) { return o[at] | o[at + 1] << 8; }
uint32_t get32(const std::vector<uint8_t> &o, uint32_t at) { return get16(o, at) | get16(o, at + 2) << 16; }
void magic(std::vector<uint8_t> &o, uint32_t at, const char *m) { std::memcpy(&o[at], m, 4); }

// a texel's index in a PICA-tiled image: 8x8 tiles, Z-order inside
uint32_t morton(uint32_t x, uint32_t y) {
    uint32_t inside = 0;
    for (uint32_t bit = 0; bit < 3; bit++) inside |= (x >> bit & 1) << 2 * bit | (y >> bit & 1) << (2 * bit + 1);
    return ((y / 8) * (SHEET / 8) + x / 8) * 64 + inside;
}

// a map: characters begin..end from glyph first on (direct), or a sorted
// list of character and glyph (scan)
struct Map {
    uint16_t begin = 0, end = 0, first = 0;
    std::vector<std::pair<uint16_t, uint16_t>> pairs;
    bool scan = false;
    uint32_t payload() const { return scan ? 2 + uint32_t(pairs.size()) * 4 : 2; }
};

std::vector<Map> maps_of(const std::vector<Glyph> &glyphs) {
    std::vector<Map> maps;
    Map rest;
    rest.scan = true;
    for (size_t start = 0, end; start < glyphs.size(); start = end) {
        for (end = start + 1; end < glyphs.size() && glyphs[end].code == glyphs[end - 1].code + 1;) end++;
        if (end - start >= DIRECT_RUN) {
            maps.push_back({glyphs[start].code, glyphs[end - 1].code, uint16_t(start), {}, false});
        } else {
            for (size_t i = start; i < end; i++) rest.pairs.emplace_back(glyphs[i].code, uint16_t(i));
        }
    }
    if (!rest.pairs.empty() || maps.empty()) {
        if (!rest.pairs.empty()) rest.begin = rest.pairs.front().first, rest.end = rest.pairs.back().first;
        maps.push_back(std::move(rest));
    }
    return maps;
}

// the block for face, laid out at 0
std::vector<uint8_t> write(const Face &face) {
    const auto &glyphs = face.glyphs;
    const uint32_t count = uint32_t(glyphs.size());
    const uint32_t sheets = std::max<uint32_t>(1, (count + PER_SHEET - 1) / PER_SHEET);
    const auto maps = maps_of(glyphs);

    const uint32_t finf = FONT_OFFSET + 0x14, tglp = finf + 0x20;
    const uint32_t sheet_at = align_up(tglp + 0x20, SHEET_ALIGN);
    const uint32_t cwdh = sheet_at + sheets * SHEET_SIZE, cwdh_size = 0x10 + align_up(count * 3, 4);
    std::vector<uint32_t> cmap;
    uint32_t total = cwdh + cwdh_size;
    for (const Map &m : maps) cmap.push_back(total), total += 0x14 + align_up(m.payload(), 4);
    if (total > BLOCK_LIMIT) std::fprintf(stderr, "runtime: the shared font's stand-in takes 0x%X bytes, more than the console's\n", total);

    std::vector<uint8_t> o(total);
    // status (loaded), the region's font (the standard one), its size
    put32(o, 0x00, 2);
    put32(o, 0x04, 1);
    put32(o, 0x08, total - FONT_OFFSET);

    // CFNU, as the console keeps it: pointers already absolute
    magic(o, FONT_OFFSET, "CFNU");
    put16(o, FONT_OFFSET + 4, 0xFEFF);
    put16(o, FONT_OFFSET + 6, 0x14);
    put32(o, FONT_OFFSET + 8, 0x03000000);
    put32(o, FONT_OFFSET + 12, total - FONT_OFFSET);
    put32(o, FONT_OFFSET + 16, 3 + uint32_t(maps.size()));

    // FINF: what a missing character shows (its question mark), the metrics
    auto question = std::find_if(glyphs.begin(), glyphs.end(), [](const Glyph &g) { return g.code == '?'; });
    magic(o, finf, "FINF");
    put32(o, finf + 4, 0x20);
    o[finf + 8] = 1; // glyph sheets
    o[finf + 9] = CELL + 1; // line feed
    put16(o, finf + 10, question == glyphs.end() ? 0 : uint32_t(question - glyphs.begin()));
    o[finf + 13] = CELL;
    o[finf + 14] = face.advance;
    o[finf + 15] = 1; // UTF-16
    put32(o, finf + 16, tglp + SECTION_BODY);
    put32(o, finf + 20, cwdh + SECTION_BODY);
    put32(o, finf + 24, cmap[0] + SECTION_BODY);
    o[finf + 28] = CELL;
    o[finf + 29] = CELL;
    o[finf + 30] = face.baseline; // ascent

    // TGLP and the sheets, glyphs as the GPU reads them, low nibble first
    uint8_t widest = CELL;
    for (const Glyph &g : glyphs) widest = std::max(widest, g.advance);
    magic(o, tglp, "TGLP");
    put32(o, tglp + 4, cwdh - tglp);
    o[tglp + 8] = CELL;
    o[tglp + 9] = CELL;
    o[tglp + 10] = face.baseline;
    o[tglp + 11] = widest;
    put32(o, tglp + 12, SHEET_SIZE);
    put16(o, tglp + 16, sheets);
    put16(o, tglp + 18, FORMAT_A4);
    put16(o, tglp + 20, COLUMNS);
    put16(o, tglp + 22, ROWS);
    put16(o, tglp + 24, SHEET);
    put16(o, tglp + 26, SHEET);
    put32(o, tglp + 28, sheet_at);
    for (uint32_t i = 0; i < count; i++) {
        uint8_t *sheet = &o[sheet_at + i / PER_SHEET * SHEET_SIZE];
        const uint32_t cell = i % PER_SHEET, x0 = cell % COLUMNS * STRIDE + 1, y0 = cell / COLUMNS * STRIDE + 1;
        for (uint32_t y = 0; y < CELL; y++) {
            for (uint32_t x = 0; x < CELL; x++) {
                if (uint8_t a = glyphs[i].alpha[y * CELL + x] & 0xF) {
                    uint32_t t = morton(x0 + x, y0 + y);
                    sheet[t / 2] |= a << (t % 2 ? 4 : 0);
                }
            }
        }
    }

    // CWDH: each glyph's left, width and advance
    magic(o, cwdh, "CWDH");
    put32(o, cwdh + 4, cwdh_size);
    put16(o, cwdh + 10, count ? count - 1 : 0);
    for (uint32_t i = 0; i < count; i++) {
        o[cwdh + 0x10 + i * 3] = uint8_t(glyphs[i].left);
        o[cwdh + 0x11 + i * 3] = glyphs[i].width;
        o[cwdh + 0x12 + i * 3] = glyphs[i].advance;
    }

    // CMAPs, each pointing on to the next: the runs, then the list
    for (size_t i = 0; i < maps.size(); i++) {
        const Map &m = maps[i];
        const uint32_t at = cmap[i];
        magic(o, at, "CMAP");
        put32(o, at + 4, 0x14 + align_up(m.payload(), 4));
        put16(o, at + 8, m.begin);
        put16(o, at + 10, m.end);
        put16(o, at + 12, m.scan ? 2 : 0);
        put32(o, at + 16, i + 1 < maps.size() ? cmap[i + 1] + SECTION_BODY : 0);
        put16(o, at + 20, m.scan ? uint32_t(m.pairs.size()) : m.first);
        for (size_t n = 0; n < m.pairs.size(); n++) {
            put16(o, at + 22 + uint32_t(n) * 4, m.pairs[n].first);
            put16(o, at + 24 + uint32_t(n) * 4, m.pairs[n].second);
        }
    }
    return o;
}

Face bitmap_face() {
    Face face{{}, CELL - SCALE, BITMAP_ADVANCE};
    face.glyphs.resize(95);
    for (uint32_t i = 0; i < 95; i++) {
        Glyph &g = face.glyphs[i];
        g = Glyph{uint16_t(0x20 + i), {}, 0, CELL, BITMAP_ADVANCE};
        for (uint32_t y = 0; y < 8 * SCALE; y++)
            for (uint32_t x = 0; x < 8 * SCALE; x++)
                if (BITMAP[i][y / SCALE] >> (7 - x / SCALE) & 1) g.alpha[y * CELL + x] = 15;
    }
    return face;
}

// the characters the console's font has, near enough: JIS X 0208's, the
// Latin alphabets, punctuation, full and half width forms
std::vector<uint16_t> characters() {
    std::vector<uint16_t> c(std::begin(JIS0208), std::end(JIS0208));
    const std::pair<uint16_t, uint16_t> ranges[] = {
        {0x20, 0x7E}, {0xA0, 0x17F}, {0x2010, 0x2027}, {0x2030, 0x203B}, {0x20AC, 0x20AC}, {0x2122, 0x2122},
        {0x2190, 0x2199}, {0x2460, 0x2473}, {0x3000, 0x30FF}, {0xFF01, 0xFF9F}, {0xFFE0, 0xFFE6}};
    for (auto [a, b] : ranges)
        for (uint32_t x = a; x <= b; x++) c.push_back(uint16_t(x));
    std::sort(c.begin(), c.end());
    c.erase(std::unique(c.begin(), c.end()), c.end());
    return c;
}

#ifdef RUNTIME_HOST_FONT
// the glyphs a font file has of those characters, drawn on every core;
// none when it has no Japanese
bool host_face(const std::vector<uint8_t> &data, Face &face) {
    stbtt_fontinfo font;
    int offset = stbtt_GetFontOffsetForIndex(data.data(), 0);
    if (offset < 0 || !stbtt_InitFont(&font, data.data(), offset)) return false;
    if (!stbtt_FindGlyphIndex(&font, 0x3042)) return false; // no Japanese: the bitmap is as good
    const float scale = stbtt_ScaleForMappingEmToPixels(&font, EM);
    const auto codes = characters();
    std::vector<Glyph> drawn(codes.size());
    std::vector<uint8_t> have(codes.size());
    auto draw = [&](size_t from, size_t to) {
        for (size_t i = from; i < to; i++) {
            int id = stbtt_FindGlyphIndex(&font, codes[i]);
            if (!id) continue;
            Glyph &g = drawn[i];
            g = Glyph{codes[i], {}, 0, 0, 0};
            int advance, bearing, x0, y0, x1, y1;
            stbtt_GetGlyphHMetrics(&font, id, &advance, &bearing);
            g.advance = uint8_t(std::clamp(std::lround(advance * scale), 0L, 255L));
            stbtt_GetGlyphBitmapBox(&font, id, scale, scale, &x0, &y0, &x1, &y1);
            // the image at the cell's left, its baseline on BASELINE; left
            // says where it goes from the pen
            const int left = 0, top = BASELINE + y0;
            const int w = std::min<int>(x1 - x0, CELL), h = y1 - y0;
            g.left = int8_t(std::clamp(x0, -128, 127));
            g.width = uint8_t(std::clamp(x1 - x0, 0, int(CELL)));
            if (w > 0 && h > 0) {
                std::vector<uint8_t> pixels(size_t(w) * h);
                stbtt_MakeGlyphBitmap(&font, pixels.data(), w, h, w, scale, scale, id);
                for (int y = 0; y < h; y++) {
                    if (top + y < 0 || top + y >= int(CELL)) continue;
                    for (int x = 0; x < w; x++) g.alpha[(top + y) * CELL + left + x] = uint8_t((pixels[y * w + x] * 15 + 127) / 255);
                }
            }
            have[i] = 1;
        }
    };
    const size_t workers = std::clamp<size_t>(std::thread::hardware_concurrency(), 1, 16), per = (codes.size() + workers - 1) / workers;
    std::vector<std::thread> threads;
    for (size_t w = 0; w < workers; w++) threads.emplace_back(draw, w * per, std::min(codes.size(), (w + 1) * per));
    for (auto &t : threads) t.join();
    face.glyphs.clear();
    for (size_t i = 0; i < codes.size(); i++)
        if (have[i]) face.glyphs.push_back(drawn[i]);
    int advance, bearing;
    stbtt_GetCodepointHMetrics(&font, 0x3042, &advance, &bearing);
    face.baseline = BASELINE;
    face.advance = uint8_t(std::clamp(std::lround(advance * scale), 0L, 255L));
    return true;
}
#endif

std::vector<uint8_t> read_file(const std::filesystem::path &path) {
    std::ifstream f(path, std::ios::binary);
    return f ? std::vector<uint8_t>(std::istreambuf_iterator<char>(f), {}) : std::vector<uint8_t>();
}

// fonts for Japanese the host has, best first
std::vector<std::filesystem::path> host_fonts() {
#ifdef _WIN32
    const char *windir = std::getenv("WINDIR");
    std::filesystem::path fonts = std::filesystem::path(windir ? windir : "C:\\Windows") / "Fonts";
    return {fonts / "YuGothM.ttc", fonts / "meiryo.ttc", fonts / "msgothic.ttc"};
#elif defined(__APPLE__)
    return {"/System/Library/Fonts/ヒラギノ角ゴシック W4.ttc", "/System/Library/Fonts/ヒラギノ角ゴシック W3.ttc"};
#else // Linux distributions' Noto, and Android's
    std::vector<std::filesystem::path> paths;
    for (const char *folder : {"/usr/share/fonts/noto-cjk", "/usr/share/fonts/opentype/noto", "/usr/share/fonts/google-noto-cjk", "/usr/share/fonts/noto"})
        for (const char *name : {"NotoSansCJK-Medium.ttc", "NotoSansCJK-Regular.ttc"}) paths.push_back(std::filesystem::path(folder) / name);
    paths.push_back("/system/fonts/NotoSansCJK-Regular.ttc");
    return paths;
#endif
}

} // namespace

std::vector<uint8_t> shared_font_stand_in(const std::vector<uint8_t> &font, uint32_t base) {
    std::vector<uint8_t> block;
    if (font.empty()) {
        block = write(bitmap_face());
    } else {
#ifdef RUNTIME_HOST_FONT
        Face face;
        if (!host_face(font, face)) return {};
        block = write(face);
#else
        return {};
#endif
    }
    shared_font_relocate(block, base);
    return block;
}

bool shared_font_relocate(std::vector<uint8_t> &b, uint32_t base) {
    const uint32_t header = FONT_OFFSET;
    if (b.size() < header + 0x14 || (std::memcmp(&b[header], "CFNU", 4) && std::memcmp(&b[header], "CFNT", 4))) return false;
    // FINF and TGLP, walking the sections from the header
    uint32_t finf = 0, tglp = 0, at = header + get16(b, header + 6);
    for (uint32_t n = get32(b, header + 16), i = 0; i < std::min(n, 16u) && at + 8 <= b.size(); i++) {
        if (!finf && !std::memcmp(&b[at], "FINF", 4)) finf = at;
        if (!tglp && !std::memcmp(&b[at], "TGLP", 4)) tglp = at;
        uint32_t size = get32(b, at + 4);
        if (!size || (finf && tglp)) break;
        at += size;
    }
    if (!finf || !tglp) return false;
    // FINF's pointer to TGLP tells where the block was laid out for
    const uint32_t laid_out = get32(b, finf + 16) - (tglp + SECTION_BODY), delta = base - laid_out;
    if (!delta) return true;
    auto inside = [&](uint32_t pointer) { return pointer - laid_out + 4 <= b.size(); };
    auto move = [&](uint32_t field) {
        uint32_t p = get32(b, field);
        put32(b, field, p + delta);
        return p - laid_out; // where it points, in the block
    };
    // the widths and the maps first, each chained on to the next by the
    // pointer at +4 of its body (widths) or +8 (maps), then FINF's own
    for (auto [first, link] : {std::pair{finf + 20, 4u}, std::pair{finf + 24, 8u}}) {
        uint32_t pointer = get32(b, first);
        for (int i = 0; i < 256 && pointer && inside(pointer + link); i++) {
            const uint32_t field = pointer - laid_out + link;
            pointer = get32(b, field);
            if (pointer) move(field);
        }
    }
    for (uint32_t field : {finf + 16, finf + 20, finf + 24, tglp + 28}) move(field);
    return true;
}

std::vector<uint8_t> shared_font(const std::filesystem::path &user, uint32_t base, std::string &source) {
    for (const char *name : {"sysdata/shared_font.bin", "shared_font.bin"}) {
        auto dump = read_file(user / name);
        if (dump.empty()) continue;
        // a dump may be taken before the system finished loading it
        if (dump.size() >= 4) put32(dump, 0, 2);
        if (shared_font_relocate(dump, base)) {
            source = (user / name).string();
            return dump;
        }
        std::fprintf(stderr, "runtime: %s holds no font, passed over\n", (user / name).string().c_str());
    }
    for (const auto &path : host_fonts()) {
        auto data = read_file(path);
        if (data.empty()) continue;
        auto block = shared_font_stand_in(data, base);
        if (!block.empty()) {
            source = "a stand-in drawn from " + path.string();
            return block;
        }
    }
    source = "a stand-in of ASCII from a bitmap";
    return shared_font_stand_in({}, base);
}

} // namespace runtime
