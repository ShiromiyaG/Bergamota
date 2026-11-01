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

constexpr uint32_t LIST = 0x20000000, COLOR = 0x20100000, DEPTH = 0x20200000, TEXTURE = 0x20300000, VERTICES = 0x20400000, CUBE = 0x20500000, SIZE = 64;

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

    // a geometry shader (as Zakuro's lib.rs tests it): one vertex in, the
    // shader emits a triangle over the whole buffer from its uniforms c0-c2,
    // the flat grey on it, over a buffer cleared to black
    b.pica.memory_fill(COLOR, COLOR + SIZE * SIZE * 4, 0, 4);
    List geometry = base;
    table(geometry, 4, std::vector<uint32_t>(256, 0xFF808080));
    geometry.put(VS_COM_MODE, 1); // its own program from here
    geometry.put(GEOSTAGE_CONFIG, 2);
    geometry.put(GS_CONFIG, 0);
    geometry.put(VS_OUTPUT_TOTAL, 1);              // o0 and o1 a vertex
    geometry.put(GS_BLOCK + INPUT_CONFIG, 1);      // both, one vertex an invocation
    geometry.put(GS_BLOCK + INPUT_MAP_LOW, 0x76543210);
    geometry.put(GS_BLOCK + OUTPUT_MASK, 3);
    geometry.put(GS_BLOCK + ENTRY_POINT, 0);
    auto setemit = [](uint32_t slot, bool primitive) { return 0x2Bu << 26 | slot << 24 | uint32_t(primitive) << 23; };
    auto mov = [](uint32_t dest, uint32_t src) { return 0x13u << 26 | dest << 21 | src << 12; };
    const uint32_t program[] = {mov(1, 1),      setemit(0, false), mov(0, 0x20), 0x2Au << 26, setemit(1, false), mov(0, 0x21),
                                0x2Au << 26,    setemit(2, true),  mov(0, 0x22), 0x2Au << 26, 0x22u << 26};
    geometry.put(GS_BLOCK + PROGRAM_INDEX, 0);
    for (uint32_t word : program) geometry.put(GS_BLOCK + PROGRAM_DATA, word);
    geometry.put(GS_BLOCK + UNIFORM_INDEX, 0);
    geometry.put4(GS_BLOCK + UNIFORM_DATA, -1, -1, 0, 1);
    geometry.put4(GS_BLOCK + UNIFORM_DATA, 3, -1, 0, 1);
    geometry.put4(GS_BLOCK + UNIFORM_DATA, -1, 3, 0, 1);
    geometry.put(FIXED_ATTRIBUTE_INDEX, 0xF);
    geometry.put4(FIXED_ATTRIBUTE_DATA, 0, 0, 0, 1);
    geometry.put4(FIXED_ATTRIBUTE_DATA, 0.5f, 0.5f, 0, 0);
    geometry.put(FIXED_ATTRIBUTE_INDEX, 0);
    pixels = b.draw(geometry);
    int covered = 0;
    for (auto &p : pixels) covered += std::abs(p[0] - 128) <= 1;
    std::printf("  %d of %d pixels grey\n", covered, SIZE * SIZE);
    check("a geometry shader's triangle is drawn", covered == SIZE * SIZE);

    // the w buffer: a quad at z/w 0.2 with w 2 keeps 0.2 in a depth buffer
    // and 0.4 (the depth times w) in a w buffer
    auto depth_of = [&](bool w_buffer) {
        List l = base;
        table(l, 4, std::vector<uint32_t>(256, 0xFF808080));
        l.put(DEPTH_BUFFER_FORMAT, 3); // 24 bits of depth, 8 of stencil
        l.put(DEPTH_BUFFER_ADDRESS, DEPTH >> 3);
        l.put(DEPTH_COLOR_MASK, 1 | 1 << 4 | 0xF00 | 1 << 12); // tested always, written
        l.put(DEPTH_STENCIL_WRITE, 3);
        l.put(DEPTH_RANGE, to_f24(1));
        l.put(DEPTH_NEAR, 0);
        l.put(DEPTHMAP_ENABLE, w_buffer ? 0 : 1);
        l.put(FIXED_ATTRIBUTE_INDEX, 0xF);
        const float corners[6][2] = {{-1, -1}, {1, -1}, {1, 1}, {-1, -1}, {1, 1}, {-1, 1}};
        for (auto &c : corners) {
            l.put4(FIXED_ATTRIBUTE_DATA, c[0] * 2, c[1] * 2, 0.4f, 2);
            l.put4(FIXED_ATTRIBUTE_DATA, 0.5f, 0.5f, 0, 0);
        }
        l.put(FIXED_ATTRIBUTE_INDEX, 0);
        b.draw(l);
        b.renderer.sync(DEPTH, SIZE * SIZE * 4, false);
        const uint8_t *p = b.memory.host(DEPTH, SIZE * SIZE * 4) + tiled_offset(SIZE / 2, SIZE / 2, SIZE, 4);
        return float(p[0] | p[1] << 8 | p[2] << 16) / 0xFFFFFF;
    };
    float z = depth_of(false), w = depth_of(true);
    std::printf("  depth %.3f, w buffer %.3f\n", z, w);
    check("the w buffer keeps the depth times w", std::abs(z - 0.2f) < 0.01f && std::abs(w - 0.4f) < 0.01f);

    // a projected texture (type 3): texture 0, 8 texels wide and growing by
    // 32 a texel across u, read at its coordinates over w. the quad's
    // coordinates are twice what is read, its w 2: at a quarter of the way
    // across, texel 2 (64), where read as they are it would be texel 4 (128)
    auto projected = [&](uint32_t type) {
        uint8_t *texels = b.memory.host(TEXTURE, 8 * 8 * 4);
        for (uint32_t y = 0; y < 8; y++) {
            for (uint32_t x = 0; x < 8; x++) {
                uint8_t *p = texels + tiled_offset(x, y, 8, 4);
                p[0] = 255, p[1] = p[2] = p[3] = uint8_t(x * 32); // A, B, G, R
            }
        }
        List l = base;
        l.put(TEV_STAGE0, 3 | 3 << 16); // texture 0
        l.put(TEXTURE_CONFIG, 1);
        l.put(0x082, 8 | 8 << 16);      // 8 by 8
        l.put(0x083, type << 28);       // nearest, clamped
        l.put(0x085, TEXTURE >> 3);
        l.put(0x08E, 0);                // RGBA8
        l.put(OUTPUT_MAP + 1, 0x1F100D0C); // o1: u, v, the w of texture 0
        l.put(FIXED_ATTRIBUTE_INDEX, 0xF);
        const float corners[6][2] = {{-1, -1}, {1, -1}, {1, 1}, {-1, -1}, {1, 1}, {-1, 1}};
        for (auto &c : corners) {
            l.put4(FIXED_ATTRIBUTE_DATA, c[0], c[1], 0, 1);
            l.put4(FIXED_ATTRIBUTE_DATA, 0.5f, (c[1] + 1) / 2 * 2, 2, 0);
        }
        l.put(FIXED_ATTRIBUTE_INDEX, 0);
        return int(b.draw(l)[(SIZE / 2) * SIZE + SIZE / 2][0]);
    };
    int flat_read = projected(0), over_w = projected(3);
    std::printf("  read as they are %d, over w %d\n", flat_read, over_w);
    check("a projected texture reads its coordinates over w", flat_read == 128 && over_w == 64);

    // the geometry shader's other modes: the vertices go into its uniforms,
    // and a shader that copies them out draws a triangle over the buffer
    auto uniform_geometry = [&](uint32_t mode, const std::vector<uint32_t> &program_words, bool indexed) {
        b.pica.memory_fill(COLOR, COLOR + SIZE * SIZE * 4, 0, 4);
        List l = base;
        table(l, 4, std::vector<uint32_t>(256, 0xFF808080));
        l.put(VS_COM_MODE, 1);
        l.put(GEOSTAGE_CONFIG, 2);
        l.put(VS_OUTPUT_TOTAL, 1);
        l.put(GS_BLOCK + OUTPUT_MASK, 3);
        l.put(GS_BLOCK + ENTRY_POINT, 0);
        // mode 2: three vertices from uniform 0, two outputs each; mode 1:
        // the first vertex with all its outputs
        l.put(GS_CONFIG, mode | 2 << 8 | 1 << 12 | 0 << 16);
        l.put(GS_MAIN_VERTICES, 0);
        l.put(GS_BLOCK + PROGRAM_INDEX, 0);
        for (uint32_t word : program_words) l.put(GS_BLOCK + PROGRAM_DATA, word);
        const float corners[3][2] = {{-1, -1}, {3, -1}, {-1, 3}};
        if (!indexed) {
            l.put(FIXED_ATTRIBUTE_INDEX, 0xF);
            for (auto &c : corners) {
                l.put4(FIXED_ATTRIBUTE_DATA, c[0], c[1], 0, 1);
                l.put4(FIXED_ATTRIBUTE_DATA, 0.5f, 0.5f, 0, 0);
            }
            l.put(FIXED_ATTRIBUTE_INDEX, 0);
        } else {
            // an array of the three, two float vectors each, and the indices:
            // a count of 3, then the three
            float *v = reinterpret_cast<float *>(b.memory.host(VERTICES, 3 * 32));
            for (int i = 0; i < 3; i++) {
                const float one[8] = {corners[i][0], corners[i][1], 0, 1, 0.5f, 0.5f, 0, 0};
                std::memcpy(v + i * 8, one, sizeof one);
            }
            const uint16_t indices[4] = {3, 0, 1, 2};
            std::memcpy(b.memory.host(VERTICES + 0x100, sizeof indices), indices, sizeof indices);
            l.put(VS_BLOCK + INPUT_CONFIG, 1);
            l.put(ATTRIBUTE_BASE, VERTICES >> 3);
            l.put(ATTRIBUTE_FORMAT_LOW, 0xFF); // both four floats
            l.put(ATTRIBUTE_FORMAT_HIGH, 0);
            l.put(ATTRIBUTE_LOADER, 0);
            l.put(ATTRIBUTE_LOADER + 1, 0x10); // attributes 0 and 1
            l.put(ATTRIBUTE_LOADER + 2, 32 << 16 | 2u << 28);
            l.put(INDEX_ARRAY, 0x100 | 1u << 31);
            l.put(VERTEX_COUNT, 4);
            l.put(VERTEX_OFFSET, 0);
            l.put(DRAW_ELEMENTS, 1);
        }
        int grey = 0;
        for (auto &p : b.draw(l)) grey += std::abs(p[0] - 128) <= 1;
        return grey;
    };
    // fixed primitives: c0-c5 are the three vertices' position and coordinates
    const uint32_t fixed_program[] = {setemit(0, false), mov(0, 0x20), mov(1, 0x21), 0x2Au << 26,
                                      setemit(1, false), mov(0, 0x22), 0x2Au << 26,  setemit(2, true),
                                      mov(0, 0x24),      0x2Au << 26,  0x22u << 26};
    int fixed = uniform_geometry(2, {std::begin(fixed_program), std::end(fixed_program)}, false);
    // variable primitives: c0 the count, c1-c2 the first vertex whole, c3 and
    // c4 the others' positions
    const uint32_t variable_program[] = {setemit(0, false), mov(0, 0x21), mov(1, 0x22), 0x2Au << 26,
                                         setemit(1, false), mov(0, 0x23), 0x2Au << 26,  setemit(2, true),
                                         mov(0, 0x24),      0x2Au << 26,  0x22u << 26};
    int variable = uniform_geometry(1, {std::begin(variable_program), std::end(variable_program)}, true);
    std::printf("  fixed primitives %d, variable %d of %d pixels grey\n", fixed, variable, SIZE * SIZE);
    check("the geometry shader's fixed primitives are drawn", fixed == SIZE * SIZE);
    check("the geometry shader's variable primitives are drawn", variable == SIZE * SIZE);

    // a cube map (type 1): six faces of one red each (40 times the face's
    // number plus one: +X, -X, +Y, -Y, +Z, -Z), read by the direction (u, v,
    // w) of texture 0's coordinates
    for (uint32_t face = 0; face < 6; face++) {
        uint8_t *texels = b.memory.host(CUBE + face * 0x1000, 8 * 8 * 4);
        for (uint32_t i = 0; i < 64; i++) texels[i * 4] = 255, texels[i * 4 + 1] = texels[i * 4 + 2] = 0, texels[i * 4 + 3] = uint8_t(40 * (face + 1));
    }
    auto cube_read = [&](float x, float y, float z) {
        List l = base;
        l.put(TEV_STAGE0, 3 | 3 << 16);
        l.put(TEXTURE_CONFIG, 1);
        l.put(0x082, 8 | 8 << 16);
        l.put(0x083, 1u << 28);
        l.put(0x085, CUBE >> 3);
        for (uint32_t face = 1; face < 6; face++) l.put(0x085 + face, ((CUBE + face * 0x1000) >> 3) & 0x3FFFFF);
        l.put(0x08E, 0);
        l.put(OUTPUT_MAP + 1, 0x1F100D0C);
        l.put(FIXED_ATTRIBUTE_INDEX, 0xF);
        const float corners[6][2] = {{-1, -1}, {1, -1}, {1, 1}, {-1, -1}, {1, 1}, {-1, 1}};
        for (auto &c : corners) {
            l.put4(FIXED_ATTRIBUTE_DATA, c[0], c[1], 0, 1);
            l.put4(FIXED_ATTRIBUTE_DATA, x, y, z, 0);
        }
        l.put(FIXED_ATTRIBUTE_INDEX, 0);
        return int(b.draw(l)[(SIZE / 2) * SIZE + SIZE / 2][0]);
    };
    const float directions[6][3] = {{1, 0.2f, 0.1f}, {-1, 0.2f, 0.1f}, {0.2f, 1, 0.1f},
                                    {0.2f, -1, 0.1f}, {0.2f, 0.1f, 1}, {0.2f, 0.1f, -1}};
    bool faces_right = true;
    std::printf("  cube faces read:");
    for (int face = 0; face < 6; face++) {
        int red = cube_read(directions[face][0], directions[face][1], directions[face][2]);
        std::printf(" %d", red);
        faces_right &= red == 40 * (face + 1);
    }
    std::printf("\n");
    check("a cube map reads the face its direction points at", faces_right);

    // the shadow output (mode 3) into a map that starts far and full
    // (0xFFFFFFFF): a fragment with an intensity (green 128) lowers only the
    // intensity, one with none (green 0) only the depth (0.25 here)
    b.pica.memory_fill(COLOR, COLOR + SIZE * SIZE * 4, 0xFFFFFFFF, 4);
    auto shadow_draw = [&](uint32_t color) {
        List l = base;
        table(l, 4, std::vector<uint32_t>(256, color));
        l.put(COLOR_OPERATION, 3);
        l.put(0x130, 0x3C00); // constant 1, linear 0
        l.put(DEPTH_RANGE, to_f24(1));
        l.put(DEPTH_NEAR, 0);
        l.put(FIXED_ATTRIBUTE_INDEX, 0xF);
        const float corners[6][2] = {{-1, -1}, {1, -1}, {1, 1}, {-1, -1}, {1, 1}, {-1, 1}};
        for (auto &c : corners) {
            l.put4(FIXED_ATTRIBUTE_DATA, c[0], c[1], 0.25f, 1);
            l.put4(FIXED_ATTRIBUTE_DATA, 0.5f, 0.5f, 0, 0);
        }
        l.put(FIXED_ATTRIBUTE_INDEX, 0);
        b.draw(l);
    };
    shadow_draw(0xFF808080);
    shadow_draw(0xFF000000);
    const uint8_t *shadow_word = b.memory.host(COLOR, SIZE * SIZE * 4) + tiled_offset(SIZE / 2, SIZE / 2, SIZE, 4);
    uint32_t kept_depth = uint32_t(shadow_word[0]) << 16 | shadow_word[1] << 8 | shadow_word[2], kept_intensity = shadow_word[3];
    std::printf("  shadow map: depth %06X, intensity %u\n", kept_depth, kept_intensity);
    check("the shadow output keeps the nearest depth and the lowest intensity",
          kept_depth >= 0x3FFFF0 && kept_depth <= 0x400000 && kept_intensity == 128);

    // the map read as a shadow texture (type 2, orthographic): in front of
    // what it saw (0.2) its intensity, behind (0.3) none
    auto shadow_read = [&](float z) {
        List l = base;
        l.put(COLOR_BUFFER_ADDRESS, (COLOR + 0x10000) >> 3); // drawn elsewhere, the map read
        l.put(TEV_STAGE0, 3 | 3 << 16);
        l.put(TEXTURE_CONFIG, 1);
        l.put(0x082, SIZE | SIZE << 16);
        l.put(0x083, 2u << 28);
        l.put(0x085, COLOR >> 3);
        l.put(0x08B, 1); // orthographic, no bias
        l.put(0x08E, 0);
        l.put(OUTPUT_MAP + 1, 0x1F100D0C);
        l.put(FIXED_ATTRIBUTE_INDEX, 0xF);
        const float corners[6][2] = {{-1, -1}, {1, -1}, {1, 1}, {-1, -1}, {1, 1}, {-1, 1}};
        for (auto &c : corners) {
            l.put4(FIXED_ATTRIBUTE_DATA, c[0], c[1], 0, 1);
            l.put4(FIXED_ATTRIBUTE_DATA, (c[0] + 1) / 2, (c[1] + 1) / 2, z, 0);
        }
        l.put(FIXED_ATTRIBUTE_INDEX, 0);
        std::memcpy(b.memory.host(LIST, uint32_t(l.words.size() * 4)), l.words.data(), l.words.size() * 4);
        b.pica.command_list(LIST, uint32_t(l.words.size() * 4));
        b.renderer.sync(COLOR + 0x10000, SIZE * SIZE * 4, false);
        return int(b.memory.host(COLOR + 0x10000, SIZE * SIZE * 4)[tiled_offset(SIZE / 2, SIZE / 2, SIZE, 4) + 3]);
    };
    int in_front = shadow_read(0.2f), behind = shadow_read(0.3f);
    std::printf("  shadow texture: in front %d, behind %d\n", in_front, behind);
    check("a shadow texture gives its intensity in front of what it saw, none behind", in_front == 128 && behind == 0);
    return failed ? 1 : 0;
}
