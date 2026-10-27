// The fragment lighting's decoding, without a GPU: the PICA's number
// formats, the tables, and the block the shader reads, as Zakuro's
// lighting.rs tests have them.

#include "runtime/pica_lighting.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

using namespace runtime::pica;
using namespace runtime::pica::lighting;

namespace {

int failures = 0;

#define CHECK(cond)                                                                                                    \
    do {                                                                                                               \
        if (!(cond)) {                                                                                                 \
            std::printf("%s:%d: %s\n", __FILE__, __LINE__, #cond);                                                     \
            failures++;                                                                                                \
        }                                                                                                              \
    } while (0)

float as_float(uint32_t bits) {
    float f;
    std::memcpy(&f, &bits, 4);
    return f;
}

void number_formats() {
    CHECK(pica_float(0x3C00, 10, 5) == 1.0f);
    CHECK(pica_float(0xC000, 10, 5) == -2.0f);
    CHECK(pica_float(0x0000, 10, 5) == 0.0f);
    float c[4];
    light_color(255u << 20 | 510u << 10 | 0, c);
    CHECK(c[0] == 1.0f && c[1] == 2.0f && c[2] == 0.0f);
    CHECK(fixed_1_11(2047) == 1.0f);
    CHECK(fixed_1_11(0x1FFF) == -1.0f / 2047.0f); // the sign is bit 12
}

void tables_decode() {
    std::array<std::array<uint32_t, 256>, 32> raw{};
    raw[3][0] = 4095;
    // half of the way down, the sign above the step
    raw[3][1] = 2048 | ((0x800 | 0x400) << 12);
    LightTables t;
    decode_light_tables(raw, t);
    CHECK(t[3][0][0] == 1.0f && t[3][0][1] == 0.0f);
    CHECK(std::fabs(t[3][1][0] - 2048.0f / 4095.0f) < 1e-6f);
    CHECK(t[3][1][1] == -1024.0f / 2047.0f);
}

void supported_tables() {
    CHECK(!table_supported(1, DISTRIBUTION0) && table_supported(0, DISTRIBUTION0));
    CHECK(!table_supported(5, DISTRIBUTION1) && table_supported(8, DISTRIBUTION1));
    CHECK(!table_supported(2, SPOTLIGHT) && table_supported(1, SPOTLIGHT));
    CHECK(!table_supported(4, FRESNEL) && table_supported(1, FRESNEL));
    CHECK(!table_supported(3, REFLECT_RED) && table_supported(0, REFLECT_RED));
    CHECK(table_supported(4, REFLECT_GREEN) && !table_supported(0, REFLECT_BLUE));
}

void lights_decode() {
    std::vector<uint32_t> regs(0x300, 0);
    LightingBlock b;
    CHECK(!LightingDecoder::decode_fresh(regs.data(), b) && b.head[1] == 0);
    regs[ENABLE] = 1;
    regs[DISABLE] = 1;
    CHECK(!LightingDecoder::decode_fresh(regs.data(), b));
    regs[DISABLE] = 0;
    // two lights, slot 0 holding light 5 and slot 1 light 2
    regs[LIGHT_COUNT] = 1;
    regs[LIGHT_SLOTS] = 5 | 2 << 4;
    regs[LIGHTS + 5 * 0x10 + 2] = 255u << 20;            // light 5's diffuse: red
    regs[LIGHTS + 5 * 0x10 + 4] = 0x3C00 << 16;          // at (0, 1, 0)
    regs[LIGHTS + 5 * 0x10 + 9] = 1;                     // directional
    regs[CONFIG1] = 1u << (24 + 5) | 1u << (8 + 2);      // light 5 no distance, light 2 no spotlight
    regs[TABLE_SCALE] = 6;                               // D0 at a quarter
    CHECK(LightingDecoder::decode_fresh(regs.data(), b));
    CHECK(b.head[1] == 2);
    CHECK(b.lights[0].flags[1] == 5 && b.lights[1].flags[1] == 2);
    CHECK(b.lights[0].diffuse[0] == 1.0f && b.lights[0].position[1] == 1.0f && b.lights[0].direction[1] == 1.0f);
    CHECK((b.lights[0].flags[0] & 1) && !(b.lights[0].flags[0] & 16)); // directional, no distance table
    CHECK((b.lights[0].flags[0] & 32) && !(b.lights[1].flags[0] & 32)); // light 2 has no spotlight
    CHECK(b.lookups[LOOKUP_D0][0] == 1 && as_float(b.lookups[LOOKUP_D0][3]) == 0.25f);
    // configuration 0 has no room for D1 and fresnel
    CHECK(b.lookups[LOOKUP_D1][0] == 0 && b.lookups[LOOKUP_FRESNEL][0] == 0);
    // D0 reads the normal and the half vector (input 0): the half vector is needed
    CHECK((b.flags[3] & 3) == 3);
}

// what is kept is what the registers set up, through changes to any
// register, read or not
void kept_follows_registers() {
    uint32_t seed = 3;
    auto random = [&] {
        seed = seed * 1664525u + 1013904223u;
        return seed >> 8;
    };
    std::vector<uint32_t> regs(0x300, 0);
    regs[ENABLE] = 1;
    regs[LIGHT_COUNT] = 7;
    regs[LIGHT_SLOTS] = 0x76543210;
    LightingDecoder kept;
    int lit = 0;
    for (int step = 0; step < 4000; step++) {
        uint32_t reg;
        switch (random() % 8) {
        case 0: reg = random() % 0x300; break;
        case 1: {
            const uint32_t switches[] = {ENABLE, DISABLE, LIGHT_COUNT, LIGHT_SLOTS};
            reg = switches[random() % 4];
            break;
        }
        default: reg = LIGHTS + random() % (CONFIG1 + 1 - LIGHTS); break;
        }
        // the two switches flip their own bit, so lighting goes on and off
        // as often as anything else changes
        bool toggle = (reg == ENABLE || reg == DISABLE) && random() % 2;
        regs[reg] ^= toggle ? 1u : 1u << (random() % 32);
        LightingBlock a, b;
        bool on = kept.decode(regs.data(), a), fresh = LightingDecoder::decode_fresh(regs.data(), b);
        lit += fresh;
        if (on != fresh || std::memcmp(&a, &b, sizeof a) != 0) {
            std::printf("step %d, register 0x%X\n", step, reg);
            failures++;
            break;
        }
    }
    if (lit <= 400) std::printf("lit %d times\n", lit);
    CHECK(lit > 400);
}

} // namespace

int main() {
    number_formats();
    tables_decode();
    supported_tables();
    lights_decode();
    kept_follows_registers();
    if (failures) std::printf("%d failed\n", failures);
    return failures ? 1 : 0;
}
