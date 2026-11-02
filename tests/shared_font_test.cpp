// The shared font's block, read the way a title's font library reads it, as
// Zakuro's shared_font.rs tests have it: the sections where the pointers
// say, every character finding its glyph, the sheets aligned and inside,
// and the block moved whole to another address.

#include "runtime/shared_font.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <vector>

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

constexpr uint32_t BASE = 0x18000000, FINF = 0x80 + 0x14, BODY = 8;

uint32_t r16(const std::vector<uint8_t> &f, uint32_t at) { return f[at] | f[at + 1] << 8; }
uint32_t r32(const std::vector<uint8_t> &f, uint32_t at) { return r16(f, at) | r16(f, at + 2) << 16; }

// the glyph of a character, down the maps from FINF's first
int glyph_of(const std::vector<uint8_t> &f, uint32_t base, uint16_t code) {
    for (uint32_t body = r32(f, FINF + 24); body;) {
        const uint32_t at = body - base - BODY, begin = r16(f, at + 8), end = r16(f, at + 10), method = r16(f, at + 12);
        if (code >= begin && code <= end) {
            if (method == 0) return int(r16(f, at + 20) + code - begin);
            for (uint32_t n = 0, count = r16(f, at + 20); n < count; n++)
                if (r16(f, at + 22 + n * 4) == code) return int(r16(f, at + 24 + n * 4));
        }
        body = r32(f, at + 16);
    }
    return -1;
}

uint32_t morton(uint32_t x, uint32_t y) {
    uint32_t inside = 0;
    for (uint32_t bit = 0; bit < 3; bit++) inside |= (x >> bit & 1) << 2 * bit | (y >> bit & 1) << (2 * bit + 1);
    return ((y / 8) * 32 + x / 8) * 64 + inside;
}

// the pixels a glyph has drawn
int lit(const std::vector<uint8_t> &f, uint32_t base, int glyph) {
    const uint32_t tglp = r32(f, FINF + 16) - base - BODY, sheet_size = r32(f, tglp + 12), sheets = r32(f, tglp + 28) - base;
    const uint32_t per = 100, sheet = glyph / per, cell = glyph % per;
    int n = 0;
    for (uint32_t y = 0; y < 24; y++)
        for (uint32_t x = 0; x < 24; x++) {
            uint32_t t = morton(cell % 10 * 25 + 1 + x, cell / 10 * 25 + 1 + y);
            n += (f[sheets + sheet * sheet_size + t / 2] >> (t % 2 ? 4 : 0) & 0xF) != 0;
        }
    return n;
}

void parses(const std::vector<uint8_t> &f, uint32_t base) {
    CHECK(r32(f, 0) == 2);
    CHECK(!std::memcmp(&f[0x80], "CFNU", 4));
    CHECK(r32(f, 0x80 + 12) == f.size() - 0x80);
    CHECK(!std::memcmp(&f[FINF], "FINF", 4));
    const char *magics[] = {"TGLP", "CWDH", "CMAP"};
    for (int i = 0; i < 3; i++) CHECK(!std::memcmp(&f[r32(f, FINF + 16 + i * 4) - base - BODY], magics[i], 4));
    const uint32_t tglp = r32(f, FINF + 16) - base - BODY;
    CHECK(r32(f, tglp + 28) % 0x80 == 0);
    CHECK(r32(f, tglp + 28) - base + r16(f, tglp + 16) * r32(f, tglp + 12) <= r32(f, FINF + 20) - base - BODY);
}

} // namespace

int main() {
    // the bitmap one: printable ASCII and nothing more
    auto bitmap = shared_font_stand_in({}, BASE);
    parses(bitmap, BASE);
    CHECK(glyph_of(bitmap, BASE, 'A') == 'A' - 0x20);
    CHECK(glyph_of(bitmap, BASE, 0x3042) < 0);
    CHECK(lit(bitmap, BASE, glyph_of(bitmap, BASE, 'A')) > 4);

    // moved whole: every pointer follows
    auto moved = bitmap;
    CHECK(shared_font_relocate(moved, 0x14100000));
    parses(moved, 0x14100000);
    CHECK(glyph_of(moved, 0x14100000, '~') == '~' - 0x20);
    CHECK(shared_font_relocate(moved, BASE) && moved == bitmap);
    std::vector<uint8_t> junk(0x200);
    CHECK(!shared_font_relocate(junk, BASE));

    // the host's font for Japanese, where there is one
#ifdef _WIN32
    const char *windir = std::getenv("WINDIR");
    std::ifstream file(std::string(windir ? windir : "C:\\Windows") + "\\Fonts\\msgothic.ttc", std::ios::binary);
    std::vector<uint8_t> data(std::istreambuf_iterator<char>(file), {});
    auto host = data.empty() ? std::vector<uint8_t>() : shared_font_stand_in(data, BASE);
    if (!host.empty()) {
        parses(host, BASE);
        CHECK(host.size() <= 0x332000);
        CHECK(r32(host, 0x80 + 16) > 4); // runs and a list of the rest
        for (uint16_t c : {u'あ', u'ア', u'漢', u'字', u'A', u'z', u'é', u'！', u'・'}) {
            int g = glyph_of(host, BASE, c);
            CHECK(g >= 0);
            if (g >= 0) CHECK(lit(host, BASE, g) > 4);
        }
        CHECK(glyph_of(host, BASE, ' ') >= 0);
    } else {
        std::printf("no font for Japanese on this host\n");
    }
#endif

    if (failures) std::printf("%d failures\n", failures);
    else std::printf("shared font: ok\n");
    return failures != 0;
}
