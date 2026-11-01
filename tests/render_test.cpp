// the renderer drawing command lists made by hand, read back from memory:
// what the games reach late, checked without them. the procedural texture
// (as Zakuro's proctex.rs has it): a flat color table gives its color
// everywhere, a ramp a ramp. skipped without a Vulkan device
#include <bit>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

#include "runtime/memory.h"
#include "runtime/pica.h"
#include "runtime/vulkan_renderer.h"

using namespace runtime;
using namespace runtime::pica;

namespace {

constexpr uint32_t LIST = 0x20000000, COLOR = 0x20100000, SIZE = 64;

// a float as the PICA's 24-bit one (1.7.16)
uint32_t to_f24(float f) {
    if (f == 0) return 0;
    uint32_t bits = std::bit_cast<uint32_t>(f);
    uint32_t sign = bits >> 31, exponent = (bits >> 23) & 0xFF, mantissa = bits & 0x7FFFFF;
    return sign << 23 | (exponent - 127 + 63) << 16 | mantissa >> 7;
}

struct List {
    std::vector<uint32_t> words;
    void put(uint32_t reg, uint32_t value) { words.insert(words.end(), {value, reg | 0xF << 16}); }
    // a vector as three words of 24-bit floats, w first
    void put4(uint32_t reg, float x, float y, float z, float w) {
        uint32_t X = to_f24(x), Y = to_f24(y), Z = to_f24(z), W = to_f24(w);
        put(reg, Z >> 16 | W << 8);
        put(reg + 1, Y >> 8 | (Z & 0xFFFF) << 16);
        put(reg + 2, X | (Y & 0xFF) << 24);
    }
};

// the state for a draw over the whole SIZE x SIZE buffer, its color the
// procedural texture's: "mov o0, v0; mov o1, v1; end", o0 the position, o1
// texture coordinates 0
void setup(List &l) {
    l.put(FRAMEBUFFER_DIMENSIONS, SIZE | (SIZE - 1) << 12 | 1 << 24);
    l.put(COLOR_BUFFER_FORMAT, 2); // RGBA8
    l.put(COLOR_BUFFER_ADDRESS, COLOR >> 3);
    l.put(DEPTH_BUFFER_ADDRESS, 0);
    l.put(VIEWPORT_WIDTH, to_f24(SIZE / 2.0f));
    l.put(VIEWPORT_HEIGHT, to_f24(SIZE / 2.0f));
    l.put(VIEWPORT_XY, 0);
    l.put(FACE_CULLING, 0);
    l.put(DEPTHMAP_ENABLE, 1);
    l.put(DEPTH_COLOR_MASK, 0xF00);
    l.put(COLOR_BUFFER_WRITE, 0xF);
    l.put(COLOR_OPERATION, 0);
    l.put(LOGIC_OP, 3); // copy
    // stage 0 takes texture 3 as it is, the rest pass it on
    l.put(TEV_STAGE0, 6 | 6 << 16);
    for (uint32_t stage : {0x0C8u, 0x0D0u, 0x0D8u, 0x0F0u, 0x0F8u}) l.put(stage, 0xF | 0xF << 16);
    l.put(TEXTURE_CONFIG, 1 << 10); // the procedural texture, on coordinates 0
    l.put(VS_BLOCK + PROGRAM_INDEX, 0);
    l.put(VS_BLOCK + PROGRAM_DATA, 0x13u << 26 | 0 << 21 | 0 << 12);
    l.put(VS_BLOCK + PROGRAM_DATA, 0x13u << 26 | 1 << 21 | 1 << 12);
    l.put(VS_BLOCK + PROGRAM_DATA, 0x22u << 26);
    l.put(VS_BLOCK + DESCRIPTOR_INDEX, 0);
    l.put(VS_BLOCK + DESCRIPTOR_DATA, 0xF | 0x1B << 5);
    l.put(VS_BLOCK + ENTRY_POINT, 0);
    l.put(VS_BLOCK + INPUT_MAP_LOW, 0x76543210);
    l.put(VS_BLOCK + OUTPUT_MASK, 3);
    l.put(VS_ATTRIBUTE_COUNT, 1);
    l.put(OUTPUT_TOTAL, 2);
    l.put(OUTPUT_MAP, 0x03020100);
    l.put(OUTPUT_MAP + 1, 0x1F1F0D0C);
    l.put(PRIMITIVE_CONFIG, 0);
    l.put(GEOSTAGE_CONFIG, 0);
}

// two triangles over the buffer, u and v from 0 to 1 across it
void quad(List &l) {
    l.put(FIXED_ATTRIBUTE_INDEX, 0xF);
    const float corners[6][2] = {{-1, -1}, {1, -1}, {1, 1}, {-1, -1}, {1, 1}, {-1, 1}};
    for (auto &c : corners) {
        l.put4(FIXED_ATTRIBUTE_DATA, c[0], c[1], 0, 1);
        l.put4(FIXED_ATTRIBUTE_DATA, (c[0] + 1) / 2, (c[1] + 1) / 2, 0, 0);
    }
    l.put(FIXED_ATTRIBUTE_INDEX, 0); // ends the vertices
}

void table(List &l, uint32_t which, const std::vector<uint32_t> &values) {
    l.put(PROCTEX_TABLE_INDEX, which << 8);
    for (uint32_t v : values) l.put(PROCTEX_TABLE_DATA, v);
}

struct Bench {
    Memory memory{false, 64u << 20};
    Pica pica{memory};
    VulkanRenderer renderer{memory};

    // the list run, the buffer read back: each pixel's RGBA
    std::vector<std::array<uint8_t, 4>> draw(const List &l) {
        std::memcpy(memory.host(LIST, uint32_t(l.words.size() * 4)), l.words.data(), l.words.size() * 4);
        pica.command_list(LIST, uint32_t(l.words.size() * 4));
        renderer.sync(COLOR, SIZE * SIZE * 4, false);
        const uint8_t *bytes = memory.host(COLOR, SIZE * SIZE * 4);
        std::vector<std::array<uint8_t, 4>> out(SIZE * SIZE);
        for (uint32_t y = 0; y < SIZE; y++) {
            for (uint32_t x = 0; x < SIZE; x++) {
                const uint8_t *p = bytes + tiled_offset(x, y, SIZE, 4);
                out[y * SIZE + x] = {p[3], p[2], p[1], p[0]};
            }
        }
        return out;
    }
};

int failed = 0;
void check(const char *what, bool ok) {
    std::printf("%s: %s\n", what, ok ? "ok" : "WRONG");
    failed += !ok;
}

} // namespace

int main() {
    Bench b;
    if (!b.renderer.ready()) {
        std::printf("skipped: no Vulkan device (%s)\n", b.renderer.error.c_str());
        return 0;
    }
    b.pica.backend = &b.renderer;

    // the color map straight through, u picks the color (repeat both ways),
    // a 256 entry table read without filtering
    List base;
    setup(base);
    std::vector<uint32_t> map;
    for (uint32_t e = 0; e < 128; e++) map.push_back((e * 32) | (32 << 12));
    table(base, 2, map);
    base.put(0x0A8, 2 | 2 << 3);
    base.put(0x0AC, 0 | 255 << 11);
    base.put(0x0AD, 0);

    List flat = base;
    table(flat, 4, std::vector<uint32_t>(256, 0xFF808080));
    quad(flat);
    auto pixels = b.draw(flat);
    bool grey = true;
    for (auto &p : pixels) grey &= std::abs(p[0] - 128) <= 1 && std::abs(p[2] - 128) <= 1 && p[3] == 255;
    std::printf("  a pixel: %d %d %d %d\n", pixels[100][0], pixels[100][1], pixels[100][2], pixels[100][3]);
    check("a flat table gives its color everywhere", grey);

    List ramp = base;
    std::vector<uint32_t> colors;
    for (uint32_t e = 0; e < 256; e++) colors.push_back(e | e << 8 | e << 16 | 0xFFu << 24);
    table(ramp, 4, colors);
    quad(ramp);
    pixels = b.draw(ramp);
    // along a row and a column of the middle: one of them climbs from dark
    // to bright through the middle, as u does
    auto at = [&](uint32_t x, uint32_t y) { return int(pixels[y * SIZE + x][0]); };
    int row[3] = {at(SIZE / 4, SIZE / 2), at(SIZE / 2, SIZE / 2), at(3 * SIZE / 4, SIZE / 2)};
    int column[3] = {at(SIZE / 2, SIZE / 4), at(SIZE / 2, SIZE / 2), at(SIZE / 2, 3 * SIZE / 4)};
    std::printf("  row %d %d %d, column %d %d %d\n", row[0], row[1], row[2], column[0], column[1], column[2]);
    auto climbs = [](const int (&v)[3]) {
        int lo = std::min(v[0], v[2]), hi = std::max(v[0], v[2]);
        return std::abs(lo - 64) <= 6 && std::abs(v[1] - 128) <= 6 && std::abs(hi - 192) <= 6;
    };
    check("a ramp table gives a ramp along u", climbs(row) != climbs(column));
    return failed ? 1 : 0;
}
