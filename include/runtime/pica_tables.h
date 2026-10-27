// The PICA's tables as the fragment shader looks them up (binding 3 in
// pica.frag): lighting's, fog's, and the procedural texture's, decoded on
// the CPU from what the registers wrote, sent when one changes. After
// Zakuro's fog.rs and proctex.rs (Rust, MIT, Copyright (c) 2026 fearkov;
// see LICENSE-zakuro).
#ifndef RUNTIME_PICA_TABLES_H
#define RUNTIME_PICA_TABLES_H

#include <array>
#include <cstdint>

#include "runtime/pica_lighting.h"

namespace runtime::pica {

// the fog's table: 128 entries, each the factor and the step to the next
using FogTable = std::array<std::array<float, 2>, 128>;

// everything the shader's buffer holds, in its order; the offsets in vec2s
// are what pica.frag reads them at
struct ShaderTables {
    LightTables light;
    FogTable fog;
};
constexpr int FOG_TABLE_AT = lighting::TABLES * 256;
static_assert(sizeof(LightTables) == FOG_TABLE_AT * 8, "the light tables, then the fog's");

// the factor in 0.11 above the step, 13 bits with the sign on top
void decode_fog_table(const std::array<uint32_t, 128> &raw, FogTable &out);

} // namespace runtime::pica

#endif
