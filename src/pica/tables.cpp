// The fog's and the procedural texture's tables decoded for the shader.
// Translated from Zakuro's fog.rs and proctex.rs, Table::write and
// Tables::write (Rust, MIT, Copyright (c) 2026 fearkov; see LICENSE-zakuro).

#include "runtime/pica_tables.h"

namespace runtime::pica {

void decode_fog_table(const std::array<uint32_t, 128> &raw, FogTable &out) {
    for (size_t i = 0; i < out.size(); i++) {
        uint32_t value = raw[i];
        int32_t step = int32_t((value & 0x1FFF) << 19) >> 19;
        out[i] = {float((value >> 13) & 0x7FF) / 2048.0f, float(step) / 2048.0f};
    }
}

} // namespace runtime::pica
