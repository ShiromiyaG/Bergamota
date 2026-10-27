// The fragment stage on the GPU against the CPU: a quad over a 64x64
// buffer drawn by the renderer, read back, and each pixel compared with
// what the CPU makes of the same registers. For what no game at hand draws
// (the procedural texture, unit 0's cube maps, shadow and projected
// textures). Skipped without a Vulkan 1.3 device.

#include "../src/pica/texture.h"
#include "runtime/memory.h"
#include "runtime/pica_tables.h"
#include "runtime/vulkan_renderer.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdio>
#include <functional>
#include <memory>

using namespace runtime;
using namespace runtime::pica;

namespace {

int failures = 0;

#define CHECK(cond)                                                                                                    \
    do {                                                                                                               \
        if (!(cond)) {                                                                                                 \
            std::printf("%s:%d: %s\n", __FILE__, __LINE__, #cond);                                                     \
            failures++;                                                                                                \
        }                                                                                                              \
    } while (0)

// a float as the PICA's 24 bits: a sign, 7 bits of exponent, 16 of mantissa
uint32_t to_f24(float v) {
    if (v == 0.0f) return 0;
    uint32_t bits = std::bit_cast<uint32_t>(v);
    uint32_t exponent = ((bits >> 23) & 0xFF) - 127 + 63;
    return (bits >> 31) << 23 | exponent << 16 | (bits & 0x7FFFFF) >> 7;
}

constexpr uint32_t mov(uint32_t dest, uint32_t source) { return (0x13u << 26) | (dest << 21) | (source << 12); }
constexpr uint32_t END = 0x22u << 26;
constexpr uint32_t PLAIN = 0xF | (0b00011011u << 5) | (0b00011011u << 14);
constexpr uint32_t TEV_STAGES[6] = {0x0C0, 0x0C8, 0x0D0, 0x0D8, 0x0F0, 0x0F8};

// a 64x64 RGBA8 buffer in VRAM, and a vertex shader that passes the
// position (o0), texture coordinates 0 (o1) and texture 0's w (o2)
struct Rig {
    static constexpr uint32_t W = 64, H = 64, AT = map::VRAM_PADDR;
    Memory memory{false, 64 << 20};
    Pica p{memory};
    VulkanRenderer vk{memory};
    // texture 0's w at the left and the right
    float w_left = 1, w_right = 1;
    // the position's z at the left and the right
    float z_left = -0.5f, z_right = -0.5f;

    Rig() {
        uint32_t vs[] = {mov(0, 0), mov(1, 1), mov(2, 2), END};
        std::copy(std::begin(vs), std::end(vs), p.vertex_shader.program.begin());
        p.vertex_shader.descriptors[0] = PLAIN;
        p.vertex_shader.version++;
        auto &r = p.regs;
        r[VS_ATTRIBUTE_COUNT] = 2;
        r[VS_BLOCK + INPUT_MAP_LOW] = 0x210;
        r[VS_BLOCK + OUTPUT_MASK] = 7;
        r[OUTPUT_TOTAL] = 3;
        r[OUTPUT_MAP] = 0x03020100;     // x y z w
        r[OUTPUT_MAP + 1] = 0x0F0E0D0C; // u0 v0 u1 v1
        r[OUTPUT_MAP + 2] = 0x1F1F1F10; // w0, the rest none
        r[FRAMEBUFFER_DIMENSIONS] = W | (H - 1) << 12;
        r[COLOR_BUFFER_ADDRESS] = AT >> 3;
        r[COLOR_BUFFER_FORMAT] = 0; // RGBA8
        r[DEPTH_COLOR_MASK] = 0xF00; // all of the color written, no depth test
        r[COLOR_BUFFER_WRITE] = 0xF;
        r[LOGIC_OP] = 3; // copy
        r[VIEWPORT_WIDTH] = to_f24(W / 2.0f);
        r[VIEWPORT_HEIGHT] = to_f24(H / 2.0f);
        // the first stage takes texture 3 (source 6) as it is, the rest
        // pass it on (source 15, the previous stage)
        for (int s = 0; s < 6; s++) r[TEV_STAGES[s]] = s ? 0xF000F : 0x60006;
    }

    // the quad from (u0, v0) at the bottom left to (u1, v1) at the top
    // right; the buffer back, row by row as memory has it
    std::vector<std::array<uint8_t, 4>> draw(float u0, float v0, float u1, float v1) {
        auto corner = [&](float x, float y) {
            std::array<Vec4, 16> a{};
            a[0] = {x, y, z_left + (z_right - z_left) * (x + 1) / 2, 1};
            float u = u0 + (u1 - u0) * (x + 1) / 2, v = v0 + (v1 - v0) * (y + 1) / 2;
            a[1] = {u, v, u, v};
            a[2] = {w_left + (w_right - w_left) * (x + 1) / 2, 0, 0, 0};
            return a;
        };
        std::vector<std::array<Vec4, 16>> quad = {corner(-1, -1), corner(1, -1), corner(1, 1),
                                                  corner(-1, -1), corner(1, 1), corner(-1, 1)};
        vk.draw_immediate(p, quad);
        vk.sync(AT, W * H * 4, false);
        std::vector<std::array<uint8_t, 4>> out(W * H);
        const uint8_t *bytes = memory.host(AT, W * H * 4);
        for (uint32_t y = 0; y < H; y++) {
            for (uint32_t x = 0; x < W; x++) out[y * W + x] = decode_color(0, bytes + tiled_offset(x, y, W, 4));
        }
        return out;
    }
};

// each pixel against what expect gives at its center's coordinates (rows
// up, as the PICA's: memory's last row is its first); a few may land the
// other side of a step (a nearest entry, a pulse, the noise's grid). Where
// unsure says the GPU and the CPU can round to either side of one (two
// faces of a cube all but tied, a coordinate all but on a texel's edge),
// the pixel is left out.
using Expect = std::function<std::array<float, 4>(float, float, float)>;
using Unsure = std::function<bool(float, float, float)>;
void compare(const char *name, Rig &rig, float u0, float v0, float u1, float v1, const Expect &expect,
             const Unsure &unsure = {}) {
    auto got = rig.draw(u0, v0, u1, v1);
    auto near = [](const std::array<uint8_t, 4> &have, const std::array<float, 4> &want) {
        bool same = true;
        for (int c = 0; c < 4; c++) same &= std::abs(float(have[c]) - want[c] * 255.0f) <= 2.5f;
        return same;
    };
    // that it shows something: many colors, and rows read the other way up
    // mostly wrong
    std::vector<uint32_t> colors;
    uint32_t off = 0, shown = 0, turned = 0, skipped = 0;
    for (uint32_t y = 0; y < Rig::H; y++) {
        for (uint32_t x = 0; x < Rig::W; x++) {
            float u = u0 + (u1 - u0) * (x + 0.5f) / Rig::W, v = v0 + (v1 - v0) * (Rig::H - 1 - y + 0.5f) / Rig::H;
            float w = rig.w_left + (rig.w_right - rig.w_left) * (x + 0.5f) / Rig::W;
            if (unsure && unsure(u, v, w)) {
                skipped++;
                continue;
            }
            auto want = expect(u, v, w);
            const auto &have = got[y * Rig::W + x];
            colors.push_back(std::bit_cast<uint32_t>(have));
            turned += !near(have, expect(u, v0 + (v1 - v0) * (y + 0.5f) / Rig::H, w));
            if (near(have, want)) continue;
            if (shown++ < 4)
                std::printf("  %s (%u, %u) u %.3f v %.3f: got %u %u %u %u, want %.0f %.0f %.0f %.0f\n", name, x, y, u, v,
                            have[0], have[1], have[2], have[3], want[0] * 255, want[1] * 255, want[2] * 255, want[3] * 255);
            off++;
        }
    }
    uint32_t total = Rig::W * Rig::H;
    std::sort(colors.begin(), colors.end());
    size_t distinct = std::unique(colors.begin(), colors.end()) - colors.begin();
    std::printf("%s: %u of %u pixels off, %u left out; %zu colors; turned over, %u off\n", name, off, total, skipped,
                distinct, turned);
    CHECK(off * 100 <= total); // 1%
    CHECK(skipped * 10 <= total); // 10%
    CHECK(distinct >= 16);
}

// the tables: rising maps (the alpha map falling) and a color table with
// each channel its own way
void fill_tables(Pica &p) {
    auto &t = p.proctex_tables;
    for (uint32_t i = 0; i < 128; i++) {
        t[0][i] = (i * 32) | 32u << 12;
        t[2][i] = (i * 31 + 40) | 31u << 12;
        t[3][i] = (4095 - i * 32) | ((-32) & 0xFFFu) << 12;
    }
    auto color = [](uint32_t i) {
        return std::array<int, 4>{int(i), int(255 - i), int((i * 7) & 0xFF), int((i * 3 + 20) & 0xFF)};
    };
    for (uint32_t i = 0; i < 256; i++) {
        auto c = color(i), n = color(std::min(i + 1, 255u));
        uint32_t word = 0, steps = 0;
        for (int ch = 0; ch < 4; ch++) {
            word |= uint32_t(c[ch]) << (ch * 8);
            int half = std::clamp((n[ch] - c[ch]) / 2, -128, 127);
            steps |= uint32_t(uint8_t(int8_t(half))) << (ch * 8);
        }
        t[4][i] = word;
        t[5][i] = steps;
    }
    p.tables_version++;
}

// Zakuro's a_flat_table_gives_its_color_everywhere, on the CPU
void flat_table() {
    std::array<std::array<uint32_t, 256>, 8> raw{};
    for (uint32_t i = 0; i < 128; i++) raw[2][i] = (i * 32) | 32u << 12;
    for (uint32_t i = 0; i < 256; i++) raw[4][i] = 0xFF808080;
    auto tables = std::make_unique<ProcTexTables>();
    decode_proctex_tables(raw, *tables);
    std::vector<uint32_t> regs(REGISTERS);
    regs[PROCTEX_CONFIG] = 2 | 2 << 3;
    regs[PROCTEX_CONFIG + 4] = 1 | 255 << 11;
    for (auto [u, v] : {std::pair{0.0f, 0.0f}, {0.3f, 0.7f}, {1.6f, 2.2f}}) {
        auto c = proctex_sample(regs.data(), *tables, u, v);
        CHECK(std::abs(c[0] - 128.0f / 255.0f) < 1e-3f && std::abs(c[3] - 1.0f) < 1e-3f);
    }
}

void procedural(Rig &rig) {
    Pica &p = rig.p;
    fill_tables(p);
    auto tables = std::make_unique<ProcTexTables>();
    decode_proctex_tables(p.proctex_tables, *tables);
    auto expect = [&](float u, float v, float) { return proctex_sample(p.regs.data(), *tables, u, v); };
    p.regs[TEXTURE_CONFIG] = 1 << 10; // texture 3, from coordinates 0
    for (int s = 0; s < 6; s++) p.regs[TEV_STAGES[s]] = s ? 0xF000F : 0x60006; // source 6
    struct Case {
        const char *name;
        uint32_t config, noise_u, noise_v, frequency, lut, offset;
        float u0, v0, u1, v1;
    } cases[] = {
        // a linear table, mirrored u and repeated v, the two made one by the
        // ninth function
        {"linear, mirrored, repeat", 3 | 2 << 3 | 9 << 6, 0, 0, 0, 1 | 255 << 11, 0, -0.3f, -0.2f, 2.3f, 1.8f},
        // the noise on, its own alpha; a nearest table of 128 from 16
        {"noise, alpha, nearest", 1 | 0 << 3 | 6 << 6 | 4 << 10 | 1 << 14 | 1 << 15, 0x0400u << 16 | 0x0800,
         0x0200u << 16 | 0xF800, 0x4000u << 16 | 0x3C00, 0 | 128 << 11, 16, 0.0f, 0.0f, 1.0f, 1.0f},
        // shifts both ways, a pulse, the fifth function
        {"shifts, pulse", 3 | 4 << 3 | 5 << 6 | 1 << 16 | 2 << 18, 0, 0, 0, 5 | 200 << 11, 30, 0.0f, 0.0f, 3.0f, 3.0f},
        // u squared, clamped to the edge
        {"edge, u squared", 1 | 1 << 3 | 1 << 6, 0, 0, 0, 3 | 255 << 11, 0, 0.0f, 0.0f, 1.4f, 1.0f},
    };
    for (const Case &c : cases) {
        p.regs[PROCTEX_CONFIG] = c.config;
        p.regs[PROCTEX_CONFIG + 1] = c.noise_u;
        p.regs[PROCTEX_CONFIG + 2] = c.noise_v;
        p.regs[PROCTEX_CONFIG + 3] = c.frequency;
        p.regs[PROCTEX_CONFIG + 4] = c.lut;
        p.regs[PROCTEX_CONFIG + 5] = c.offset;
        compare(c.name, rig, c.u0, c.v0, c.u1, c.v1, expect);
    }
}

// unit 0's textures: 32x32 RGBA8 in VRAM after the buffer, a face every
// 64 KiB; bytes from a counter, so every texel differs
constexpr uint32_t TEXTURE_AT = Rig::AT + 0x100000, SIDE = 32;

struct Faces {
    std::vector<uint8_t> rgba[6]; // decoded, rows as decode_texture gives them
};

Faces fill_faces(Rig &rig, uint32_t seed) {
    Faces f;
    for (uint32_t face = 0; face < 6; face++) {
        uint8_t *bytes = rig.memory.host(TEXTURE_AT + face * 0x10000, SIDE * SIDE * 4);
        uint32_t x = seed * 2654435761u + face * 40503u + 1;
        for (uint32_t i = 0; i < SIDE * SIDE * 4; i++) {
            x ^= x << 13, x ^= x >> 17, x ^= x << 5;
            bytes[i] = uint8_t(x);
        }
        f.rgba[face].resize(SIDE * SIDE * 4);
        decode_texture(0, bytes, SIDE, SIDE, f.rgba[face].data());
    }
    return f;
}

std::array<float, 4> texel_of(const std::vector<uint8_t> &rgba, uint32_t x, uint32_t row) {
    const uint8_t *t = &rgba[(row * SIDE + x) * 4];
    return {t[0] / 255.0f, t[1] / 255.0f, t[2] / 255.0f, t[3] / 255.0f};
}

// nearest, held to the edges: an image's row 0 is decode_texture's first,
// sampled at 1 - v (as pica.frag's texel)
std::array<float, 4> flat(const Faces &f, float u, float v) {
    auto at = [](float c) { return uint32_t(std::clamp(int(std::floor(c * SIDE)), 0, int(SIDE) - 1)); };
    return texel_of(f.rgba[0], at(u), at(1.0f - v));
}

// the cube by Vulkan's table of faces (its spec, "Cube Map Face
// Selection"): the face, and where on it from 0 to 1
struct CubeAt {
    int face;
    float s, t;
};
CubeAt cube_at(float x, float y, float z) {
    float ax = std::abs(x), ay = std::abs(y), az = std::abs(z), sc, tc, ma;
    int face;
    if (ax >= ay && ax >= az) face = x >= 0 ? 0 : 1, sc = x >= 0 ? -z : z, tc = -y, ma = ax;
    else if (ay >= az) face = y >= 0 ? 2 : 3, sc = x, tc = y >= 0 ? z : -z, ma = ay;
    else face = z >= 0 ? 4 : 5, sc = z >= 0 ? x : -x, tc = -y, ma = az;
    return {face, 0.5f * sc / ma + 0.5f, 0.5f * tc / ma + 0.5f};
}

// the faces go in turned over, so the face's row t is decode_texture's
// SIDE - 1 - t
std::array<float, 4> cubic(const Faces &f, float x, float y, float z) {
    auto at = [](float c) { return uint32_t(std::clamp(int(std::floor(c * SIDE)), 0, int(SIDE) - 1)); };
    CubeAt c = cube_at(x, y, z);
    return texel_of(f.rgba[c.face], at(c.s), SIDE - 1 - at(c.t));
}

// the two largest of a direction all but tied: either face
bool face_tie(float x, float y, float z) {
    float a[3] = {std::abs(x), std::abs(y), std::abs(z)};
    std::sort(std::begin(a), std::end(a));
    return a[2] - a[1] <= 1e-3f * a[2];
}

// a coordinate from 0 to 1 within a fiftieth of a texel of a texel's edge
bool on_edge(float c) {
    float at = c * SIDE, part = at - std::floor(at);
    return part < 0.02f || part > 0.98f;
}

bool cube_unsure(float x, float y, float z) {
    if (face_tie(x, y, z)) return true;
    CubeAt c = cube_at(x, y, z);
    return on_edge(c.s) || on_edge(c.t);
}

// a shadow texel against z: 0 when its depth (red, green, blue) is z or
// nearer, its penumbra (alpha) elsewhere
float shadow_compare(const std::vector<uint8_t> &rgba, uint32_t x, uint32_t row, uint32_t z) {
    const uint8_t *t = &rgba[(row * SIDE + x) * 4];
    uint32_t depth = uint32_t(t[0]) << 16 | uint32_t(t[1]) << 8 | t[2];
    return depth <= z ? 0.0f : t[3] / 255.0f;
}

float mix4(float a, float b, float c, float d, float fx, float fy) {
    float lo = a + (c - a) * fy, hi = b + (d - b) * fy;
    return lo + (hi - lo) * fx;
}

uint32_t shadow_z(float w, uint32_t reg) {
    int32_t bias = int32_t((reg >> 1 & 0x7FFFFF) << 1);
    return uint32_t(std::max(0, int32_t(std::min(std::abs(w), 1.0f) * float(0xFFFFFF)) - bias));
}

// the shadow texture, Citra's reading: rows its way up (its row i is ours
// SIDE - 1 - i), 1 past the edges
std::array<float, 4> shadow_flat(const Faces &f, uint32_t reg, float u, float v, float w) {
    if (!(reg & 1)) u /= w, v /= w;
    uint32_t z = shadow_z(w, reg);
    float ax = u * SIDE - 0.5f, ay = v * SIDE - 0.5f, lx = std::floor(ax), ly = std::floor(ay);
    auto at = [&](int x, int y) {
        if (x < 0 || y < 0 || x >= int(SIDE) || y >= int(SIDE)) return 1.0f;
        return shadow_compare(f.rgba[0], uint32_t(x), SIDE - 1 - uint32_t(y), z);
    };
    int i = int(lx), j = int(ly);
    float s = mix4(at(i, j), at(i + 1, j), at(i, j + 1), at(i + 1, j + 1), ax - lx, ay - ly);
    return {s, s, s, s};
}

// the shadow cube: the face by the largest, coordinates on it as Citra
// works them out, four texels held to its edges; the face's row i is, as it
// went in turned over, decode_texture's SIDE - 1 - i
std::array<float, 4> shadow_cubic(const Faces &f, uint32_t reg, float u, float v, float w) {
    float cx = u, cy = v, cz = w, ax = std::abs(cx), ay = std::abs(cy), az = std::abs(cz), su, sv;
    int face;
    if (ax > ay && ax > az) {
        w = ax, su = -cz, sv = -cy, face = cx > 0 ? 0 : 1;
        if (cx < 0) su = -su;
    } else if (ay > az) {
        w = ay, su = cx, sv = cz, face = cy > 0 ? 2 : 3;
        if (cy < 0) sv = -sv;
    } else {
        w = az, su = -cx, sv = -cy, face = cz > 0 ? 4 : 5;
        if (cz > 0) su = -su;
    }
    int32_t bias = int32_t((reg >> 1 & 0x7FFFFF) << 1);
    uint32_t z = uint32_t(std::max(0, int32_t(std::min(w, 1.0f) * float(0xFFFFFF)) - bias));
    float px = SIDE * (su / w * 0.5f + 0.5f) - 0.5f, py = SIDE * (sv / w * 0.5f + 0.5f) - 0.5f;
    float lx = std::floor(px), ly = std::floor(py);
    auto at = [&](int x, int y) {
        x = std::clamp(x, 0, int(SIDE) - 1), y = std::clamp(y, 0, int(SIDE) - 1);
        return shadow_compare(f.rgba[face], uint32_t(x), SIDE - 1 - uint32_t(y), z);
    };
    int i = int(lx), j = int(ly);
    float s = mix4(at(i, j), at(i + 1, j), at(i, j + 1), at(i + 1, j + 1), px - lx, py - ly);
    return {s, s, s, s};
}

void unit0_types(Rig &rig) {
    Pica &p = rig.p;
    auto &r = p.regs;
    r[TEXTURE_CONFIG] = 1; // unit 0 alone
    for (int s = 0; s < 6; s++) r[TEV_STAGES[s]] = s ? 0xF000F : 0x30003; // source 3, texture 0
    r[0x082] = SIDE | SIDE << 16;
    r[0x084] = 0;
    r[0x085] = TEXTURE_AT >> 3;
    for (uint32_t f = 1; f < 6; f++) r[0x086 + f - 1] = ((TEXTURE_AT + f * 0x10000) >> 3) & 0x3FFFFF;
    r[0x08E] = 0; // RGBA8
    Faces faces = fill_faces(rig, 1);
    // nearest, clamped to the edge; the type in bits 28-30
    auto type = [&](uint32_t t) { r[0x083] = t << 28; };

    type(0);
    rig.w_left = rig.w_right = 1;
    compare("2D", rig, -0.1f, -0.1f, 1.1f, 1.1f, [&](float u, float v, float) { return flat(faces, u, v); });

    type(3);
    rig.w_left = 0.6f, rig.w_right = 1.4f;
    compare("projected", rig, 0.0f, 0.0f, 1.2f, 1.0f, [&](float u, float v, float w) { return flat(faces, u / w, v / w); },
            [](float u, float v, float w) { return on_edge(u / w) || on_edge(v / w); });

    type(1);
    rig.w_left = rig.w_right = 0.3f;
    auto cube = [&](float u, float v, float w) { return cubic(faces, u, v, w); };
    compare("cube, x and y faces", rig, -1.0f, -1.0f, 1.0f, 1.0f, cube, cube_unsure);
    rig.w_left = -1.5f, rig.w_right = 1.5f;
    compare("cube, z faces", rig, -0.9f, -0.8f, 0.9f, 0.8f, cube, cube_unsure);

    type(2);
    r[0x08B] = 0x1000 << 1 | 1; // orthographic, a bias
    rig.w_left = 0.1f, rig.w_right = 0.9f;
    compare("shadow, orthographic", rig, -0.05f, -0.05f, 1.05f, 1.05f,
            [&](float u, float v, float w) { return shadow_flat(faces, r[0x08B], u, v, w); });
    r[0x08B] = 0x40000 << 1; // perspective
    rig.w_left = 0.5f, rig.w_right = 1.0f;
    compare("shadow, perspective", rig, 0.0f, 0.0f, 0.9f, 0.95f,
            [&](float u, float v, float w) { return shadow_flat(faces, r[0x08B], u, v, w); });

    type(4);
    r[0x08B] = 0x2000 << 1;
    rig.w_left = 0.4f, rig.w_right = 0.4f;
    auto shadow_cube = [&](float u, float v, float w) { return shadow_cubic(faces, r[0x08B], u, v, w); };
    compare("shadow cube, x and y faces", rig, -1.0f, -1.0f, 1.0f, 1.0f, shadow_cube, face_tie);
    rig.w_left = -1.2f, rig.w_right = 1.2f;
    compare("shadow cube, z faces", rig, -0.7f, -0.7f, 0.7f, 0.7f, shadow_cube, face_tie);
    r[TEXTURE_CONFIG] = 0;
}

// a 16-bit float (1.5.10)
float half(uint32_t h) {
    float sign = h & 0x8000 ? -1.0f : 1.0f, mantissa = float(h & 0x3FF) / 1024.0f;
    int32_t exponent = int32_t((h >> 10) & 0x1F);
    return exponent ? sign * (1 + mantissa) * std::ldexp(1.0f, exponent - 15) : sign * mantissa * std::ldexp(1.0f, -14);
}

// the shadow mode, as Citra worked it out: a pixel's word in memory is its
// depth (24 bits) above its penumbra (8); a fragment nearer than the depth
// with no penumbra of its own puts its depth, with one puts that penumbra
// scaled by the bias, when less
uint32_t shadow_update(uint32_t word, uint32_t d, uint32_t s, uint32_t bias) {
    uint32_t depth = word >> 8, penumbra = word & 0xFF;
    if (d < depth) {
        if (!s) depth = d;
        else {
            float scaled = float(s) / (half(bias & 0xFFFF) + half(bias >> 16) * float(d) / float(depth));
            penumbra = std::min(uint32_t(std::clamp(scaled, 0.0f, 255.0f)), penumbra);
        }
    }
    return depth << 8 | penumbra;
}

// a buffer of known words, then a quad over it nearer at its left twice:
// with no penumbra (a constant color), then texture 0's green as one
void shadow_mode(Rig &rig) {
    Pica &p = rig.p;
    auto &r = p.regs;
    constexpr uint32_t W = Rig::W, H = Rig::H;
    uint8_t *fb = rig.memory.host(Rig::AT, W * H * 4);
    std::vector<uint32_t> want(W * H);
    uint32_t seed = 7;
    for (uint32_t y = 0; y < H; y++) {
        for (uint32_t x = 0; x < W; x++) {
            seed ^= seed << 13, seed ^= seed >> 17, seed ^= seed << 5;
            uint32_t word = seed, at = tiled_offset(x, y, W, 4);
            for (int b = 0; b < 4; b++) fb[at + b] = uint8_t(word >> (b * 8));
            want[y * W + x] = word;
        }
    }
    rig.vk.sync(Rig::AT, W * H * 4, true); // memory the newer: loaded again
    std::vector<uint32_t> first = want;
    r[COLOR_OPERATION] = 3; // the shadow mode
    r[0x130] = 0x3C00 | 0x3800u << 16; // the penumbra's bias: 1 + 0.5 d / depth
    r[DEPTH_RANGE] = to_f24(-1.0f), r[DEPTH_NEAR] = 0;
    r[DEPTHMAP_ENABLE] = 1; // the depth z / w, no w buffer
    rig.w_left = rig.w_right = 1;
    // the same texture as unit0_types' (the renderer looks at a texture's
    // memory again a frame on, and these draws are all in one)
    Faces faces = fill_faces(rig, 1);
    r[TEXTURE_CONFIG] = 1;
    r[0x083] = 0; // 2D, nearest
    uint64_t refused = rig.vk.refused;
    uint32_t unsure = 0;
    std::vector<bool> skip(W * H);
    for (int pass = 0; pass < 2; pass++) {
        // the first stage: a constant color with no green, or texture 0, a
        // little nearer than the first (over the depths it put)
        r[TEV_STAGES[0]] = pass ? 0x30003 : 0xE000E;
        rig.z_left = pass ? -0.05f : -0.1f, rig.z_right = pass ? -0.85f : -0.9f;
        r[TEV_STAGES[0] + 3] = 0xFF0000FF;
        auto got = rig.draw(0, 0, 1, 1);
        for (uint32_t y = 0; y < H; y++) {
            for (uint32_t x = 0; x < W; x++) {
                float depth = -(rig.z_left + (rig.z_right - rig.z_left) * (x + 0.5f) / W);
                uint32_t d = uint32_t(depth * float(0xFFFFFF)), &w = want[y * W + x];
                // a depth all but the one there: either side
                if (uint32_t(std::abs(int64_t(d) - int64_t(w >> 8))) < 256) skip[y * W + x] = true;
                uint32_t s = 0;
                if (pass) {
                    auto texel = flat(faces, (x + 0.5f) / W, (H - 1 - y + 0.5f) / H);
                    s = uint32_t(texel[1] * 255.0f);
                }
                w = shadow_update(w, d, s, r[0x130]);
            }
        }
        (void)got;
    }
    // the words back, against the CPU's: depths within a few steps (the
    // fragment's interpolated), penumbras within one
    const uint8_t *bytes = rig.memory.host(Rig::AT, W * H * 4);
    uint32_t off = 0, changed = 0;
    for (uint32_t y = 0; y < H; y++) {
        for (uint32_t x = 0; x < W; x++) {
            if (skip[y * W + x]) {
                unsure++;
                continue;
            }
            uint32_t at = tiled_offset(x, y, W, 4), have = 0;
            for (int b = 0; b < 4; b++) have |= uint32_t(bytes[at + b]) << (b * 8);
            uint32_t w = want[y * W + x];
            bool same = std::abs(int64_t(have >> 8) - int64_t(w >> 8)) <= 4 && std::abs(int(have & 0xFF) - int(w & 0xFF)) <= 1;
            changed += have != first[y * W + x];
            if (!same && off++ < 4)
                std::printf("  shadow mode (%u, %u): got %06X/%02X, want %06X/%02X\n", x, y, have >> 8, have & 0xFF, w >> 8, w & 0xFF);
        }
    }
    uint32_t total = W * H;
    std::printf("shadow mode: %u of %u pixels off, %u left out; %u changed by the draws\n", off, total, unsure, changed);
    CHECK(rig.vk.refused == refused);
    CHECK(changed * 4 >= total);
    CHECK(off * 100 <= total);
    CHECK(unsure * 10 <= total);
    r[COLOR_OPERATION] = 0;
    r[TEXTURE_CONFIG] = 0;
    rig.z_left = rig.z_right = -0.5f;
}

} // namespace

int main() {
    flat_table();
    auto rig = std::make_unique<Rig>();
    if (!rig->vk.ready()) {
        std::printf("render: no Vulkan 1.3 device, the GPU half skipped\n");
    } else {
        procedural(*rig);
        unit0_types(*rig);
        shadow_mode(*rig);
    }
    if (failures) {
        std::printf("%d failed\n", failures);
        return 1;
    }
    std::printf("render: all passed\n");
    return 0;
}
