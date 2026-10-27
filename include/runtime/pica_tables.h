// The PICA's tables as the fragment shader looks them up (binding 3 in
// pica.frag): lighting's, fog's, and the procedural texture's, decoded on
// the CPU from what the registers wrote, sent when one changes. After
// Zakuro's fog.rs and proctex.rs (Rust, MIT, Copyright (c) 2026 fearkov;
// see LICENSE-zakuro).
#ifndef RUNTIME_PICA_TABLES_H
#define RUNTIME_PICA_TABLES_H

#include <array>
#include <cstddef>
#include <cstdint>

#include "runtime/pica_lighting.h"

namespace runtime::pica {

// the fog's table: 128 entries, each the factor and the step to the next
using FogTable = std::array<std::array<float, 2>, 128>;

// the procedural texture's (texture 3): the noise, the color map and the
// alpha map, 128 entries of a value and the step to the next each; then
// the colors, 0 to 255 a channel, and the steps between them
struct ProcTexTables {
    std::array<std::array<float, 2>, 128> maps[3];
    std::array<std::array<float, 4>, 256> colors, steps;
};

// everything the shader's buffer holds, in its order; the offsets in vec2s
// are what pica.frag reads them at
struct ShaderTables {
    LightTables light;
    FogTable fog;
    ProcTexTables proctex;
};
constexpr int FOG_TABLE_AT = lighting::TABLES * 256;
constexpr int PROCTEX_TABLES_AT = FOG_TABLE_AT + 128;
static_assert(sizeof(LightTables) == FOG_TABLE_AT * 8, "the light tables, then the fog's");
static_assert(offsetof(ShaderTables, proctex) == PROCTEX_TABLES_AT * 8, "then the procedural texture's");

// the factor in 0.11 above the step, 13 bits with the sign on top
void decode_fog_table(const std::array<uint32_t, 128> &raw, FogTable &out);

// the procedural texture's registers and tables, as Pica keeps them: the
// table index register's bits 8-10 number them, noise 0, color map 2, alpha
// map 3, colors 4, color steps 5
constexpr uint32_t PROCTEX_CONFIG = 0x0A8; // to 0x0AD: noise u, noise v, frequency, lookup, offset
void decode_proctex_tables(const std::array<std::array<uint32_t, 256>, 8> &raw, ProcTexTables &out);
// texture 3 at a pair of coordinates, 0 to 1 a channel, from the registers
// (all of them, as Pica has them); what pica.frag's proctex() does
std::array<float, 4> proctex_sample(const uint32_t *regs, const ProcTexTables &tables, float u, float v);

} // namespace runtime::pica

#endif
