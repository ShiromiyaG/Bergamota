// The PICA's fragment lighting, decoded on the CPU into the block the
// fragment shader reads (pica.frag), and its tables into what the shader
// looks up. The shading itself is in the shader. After Zakuro's lighting.rs
// (Rust, MIT, Copyright (c) 2026 fearkov; see LICENSE-zakuro).
#ifndef RUNTIME_PICA_LIGHTING_H
#define RUNTIME_PICA_LIGHTING_H

#include <array>
#include <cstddef>
#include <cstdint>

namespace runtime::pica {

namespace lighting {
constexpr uint32_t ENABLE = 0x08F, LIGHTS = 0x140, GLOBAL_AMBIENT = 0x1C0, LIGHT_COUNT = 0x1C2, CONFIG0 = 0x1C3,
                   CONFIG1 = 0x1C4, DISABLE = 0x1C6, TABLE_ABSOLUTE = 0x1D0, TABLE_INPUT = 0x1D1,
                   TABLE_SCALE = 0x1D2, LIGHT_SLOTS = 0x1D9;
// the tables, by number: two distributions, fresnel, the three reflections,
// then a spotlight and a distance table for each light
constexpr int DISTRIBUTION0 = 0, DISTRIBUTION1 = 1, FRESNEL = 3, REFLECT_BLUE = 4, REFLECT_GREEN = 5,
              REFLECT_RED = 6, SPOTLIGHT = 8, DISTANCE = 16, TABLES = 24;
// the lookups in the block, in the order the shader reads them
constexpr int LOOKUP_D0 = 0, LOOKUP_D1 = 1, LOOKUP_FRESNEL = 2, LOOKUP_RED = 3, LOOKUP_GREEN = 4, LOOKUP_BLUE = 5,
              LOOKUP_SPOT = 6, LOOKUPS = 7;
} // namespace lighting

// one light, std140: colors, where it is, its spotlight direction, the
// bias and scale into its distance table, and its switches
struct LightSlot {
    float specular0[4], specular1[4], diffuse[4], ambient[4];
    float position[4];
    float direction[4]; // the unit vector toward a directional light
    float spot[4];      // the inverse spotlight direction
    float distance[4];  // bias, scale
    // directional, two-sided, geometric 0 and 1, distance table, spotlight,
    // shadowed (bits 0-6); the light's number (its tables)
    uint32_t flags[4];
};

// what the shader reads, std140 (uniform Lighting in pica.frag)
struct LightingBlock {
    // configuration, lights on, bump (0 none, 1 normal, 2 tangent; unit in
    // bits 4-5, z rebuilt in bit 8), shadow (bit 0 on; unit in bits 4-5,
    // inverted, primary, secondary, alpha in bits 8-11)
    uint32_t head[4];
    // fresnel on the primary alpha, on the secondary, highlights clamped,
    // the half vector needed (bit 0) and the view vector (bit 1)
    uint32_t flags[4];
    // each table read: on, its input, absolute, the scale's float bits
    uint32_t lookups[lighting::LOOKUPS][4];
    float global_ambient[4];
    LightSlot lights[8];
};
static_assert(sizeof(LightSlot) == 9 * 16, "std140: nine vec4");
static_assert(sizeof(LightingBlock) == (2 + lighting::LOOKUPS + 1) * 16 + 8 * sizeof(LightSlot), "std140");

// a PICA float with mantissa and exponent bits, where a zero exponent is
// the smallest one rather than denormal
float pica_float(uint32_t bits, uint32_t mantissa, uint32_t exponent);
// a light color: three 10-bit fields where 255 stands for one
void light_color(uint32_t value, float out[4]);
// a spotlight direction component, a signed 1.11 fixed point number
float fixed_1_11(uint32_t value);
// which tables a lighting configuration has room for
bool table_supported(uint32_t config, int table);

// the lighting the registers set up into block; false (and a block with no
// lights) when it is off. keeps what it last decoded and the registers it
// came from, which draws mostly leave as they were
class LightingDecoder {
public:
    bool decode(const uint32_t *regs, LightingBlock &block);
    // decodes no matter what it kept (tests)
    static bool decode_fresh(const uint32_t *regs, LightingBlock &block);

private:
    static constexpr size_t READ = 1 + (lighting::CONFIG1 + 1 - lighting::LIGHTS) + 1 + 3 + 1;
    static void registers_read(const uint32_t *regs, std::array<uint32_t, READ> &out);
    bool valid_ = false, on_ = false;
    std::array<uint32_t, READ> read_{};
    LightingBlock block_{};
};

// the 24 tables as the shader looks them up: each entry a value and the
// step to the next, from the raw entries the registers wrote (a 0.12
// value, an 11-bit step with its sign above it)
using LightTables = std::array<std::array<std::array<float, 2>, 256>, lighting::TABLES>;
void decode_light_tables(const std::array<std::array<uint32_t, 256>, 32> &raw, LightTables &out);

} // namespace runtime::pica

#endif
