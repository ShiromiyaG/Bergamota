// The fragment stage on the GPU against the CPU: a quad over a 64x64
// buffer drawn by the renderer, read back, and each pixel compared with
// what the CPU makes of the same registers. For what no game at hand draws
// (the procedural texture). Skipped without a Vulkan 1.3 device.

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
// position (o0) and texture coordinates 0 (o1)
struct Rig {
    static constexpr uint32_t W = 64, H = 64, AT = map::VRAM_PADDR;
    Memory memory{false, 64 << 20};
    Pica p{memory};
    VulkanRenderer vk{memory};

    Rig() {
        uint32_t vs[] = {mov(0, 0), mov(1, 1), END};
        std::copy(std::begin(vs), std::end(vs), p.vertex_shader.program.begin());
        p.vertex_shader.descriptors[0] = PLAIN;
        p.vertex_shader.version++;
        auto &r = p.regs;
        r[VS_ATTRIBUTE_COUNT] = 1;
        r[VS_BLOCK + INPUT_MAP_LOW] = 0x10;
        r[VS_BLOCK + OUTPUT_MASK] = 3;
        r[OUTPUT_TOTAL] = 2;
        r[OUTPUT_MAP] = 0x03020100;     // x y z w
        r[OUTPUT_MAP + 1] = 0x0F0E0D0C; // u0 v0 u1 v1
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
            a[0] = {x, y, -0.5f, 1};
            float u = u0 + (u1 - u0) * (x + 1) / 2, v = v0 + (v1 - v0) * (y + 1) / 2;
            a[1] = {u, v, u, v};
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
// other side of a step (a nearest entry, a pulse, the noise's grid)
void compare(const char *name, Rig &rig, float u0, float v0, float u1, float v1,
             const std::function<std::array<float, 4>(float, float)> &expect) {
    auto got = rig.draw(u0, v0, u1, v1);
    auto near = [](const std::array<uint8_t, 4> &have, const std::array<float, 4> &want) {
        bool same = true;
        for (int c = 0; c < 4; c++) same &= std::abs(float(have[c]) - want[c] * 255.0f) <= 2.5f;
        return same;
    };
    // that it shows something: many colors, and rows read the other way up
    // mostly wrong
    std::vector<uint32_t> colors;
    uint32_t off = 0, shown = 0, turned = 0;
    for (uint32_t y = 0; y < Rig::H; y++) {
        for (uint32_t x = 0; x < Rig::W; x++) {
            float u = u0 + (u1 - u0) * (x + 0.5f) / Rig::W, v = v0 + (v1 - v0) * (Rig::H - 1 - y + 0.5f) / Rig::H;
            auto want = expect(u, v);
            const auto &have = got[y * Rig::W + x];
            colors.push_back(std::bit_cast<uint32_t>(have));
            turned += !near(have, expect(u, v0 + (v1 - v0) * (y + 0.5f) / Rig::H));
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
    std::printf("%s: %u of %u pixels off; %zu colors; turned over, %u off\n", name, off, total, distinct, turned);
    CHECK(off * 100 <= total); // 1%
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
    auto expect = [&](float u, float v) { return proctex_sample(p.regs.data(), *tables, u, v); };
    p.regs[TEXTURE_CONFIG] = 1 << 10; // texture 3, from coordinates 0
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

} // namespace

int main() {
    flat_table();
    auto rig = std::make_unique<Rig>();
    if (!rig->vk.ready()) {
        std::printf("render: no Vulkan 1.3 device, the GPU half skipped\n");
    } else {
        procedural(*rig);
    }
    if (failures) {
        std::printf("%d failed\n", failures);
        return 1;
    }
    std::printf("render: all passed\n");
    return 0;
}
