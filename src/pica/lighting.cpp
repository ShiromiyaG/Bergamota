// The PICA's fragment lighting decoded for the shader. Translated from
// Zakuro's lighting.rs, Lighting::read and pack and Tables::write (Rust,
// MIT, Copyright (c) 2026 fearkov; see LICENSE-zakuro).

#include "runtime/pica_lighting.h"

#include <cmath>
#include <cstring>

namespace runtime::pica {

using namespace lighting;

float pica_float(uint32_t bits, uint32_t mantissa, uint32_t exponent) {
    uint32_t width = mantissa + exponent + 1;
    uint32_t sign = (bits >> (mantissa + exponent) & 1) << 31;
    if ((bits & ((1u << (width - 1)) - 1)) == 0) {
        float zero;
        std::memcpy(&zero, &sign, 4);
        return zero;
    }
    uint32_t biased = (bits >> mantissa) & ((1u << exponent) - 1);
    biased = biased == (1u << exponent) - 1 ? 255 : biased + 128 - (1u << (exponent - 1));
    uint32_t out = sign | ((bits & ((1u << mantissa) - 1)) << (23 - mantissa)) | (biased << 23);
    float f;
    std::memcpy(&f, &out, 4);
    return f;
}

void light_color(uint32_t value, float out[4]) {
    out[0] = float((value >> 20) & 0x3FF) / 255.0f;
    out[1] = float((value >> 10) & 0x3FF) / 255.0f;
    out[2] = float(value & 0x3FF) / 255.0f;
    out[3] = 0;
}

float fixed_1_11(uint32_t value) {
    int32_t v = int32_t((value & 0x1FFF) << 19) >> 19;
    return float(v) / 2047.0f;
}

bool table_supported(uint32_t config, int table) {
    switch (table) {
    case DISTRIBUTION0: return config != 1;
    case DISTRIBUTION1: return config != 0 && config != 1 && config != 5;
    case SPOTLIGHT: return config != 2 && config != 3;
    case FRESNEL: return config != 0 && config != 2 && config != 4;
    case REFLECT_RED: return config != 3;
    default: return config == 4 || config == 5 || config == 8;
    }
}

namespace {

uint32_t float_bits(float f) {
    uint32_t u;
    std::memcpy(&u, &f, 4);
    return u;
}

void set3(float out[4], float x, float y, float z) {
    out[0] = x, out[1] = y, out[2] = z, out[3] = 0;
}

} // namespace

bool LightingDecoder::decode_fresh(const uint32_t *regs, LightingBlock &b) {
    b = LightingBlock{};
    if (!(regs[ENABLE] & 1) || (regs[DISABLE] & 1)) return false;
    uint32_t config0 = regs[CONFIG0], config1 = regs[CONFIG1], config = (config0 >> 4) & 0xF;
    uint32_t absolute = regs[TABLE_ABSOLUTE], inputs = regs[TABLE_INPUT], scales = regs[TABLE_SCALE];

    // each table's input, abs and scale sit four bits apart, abs being a
    // disable bit one above
    auto lookup = [&](int slot, uint32_t field, bool on) {
        static const float SCALES[8] = {1, 2, 4, 8, 0, 0, 0.25f, 0.5f};
        uint32_t *l = b.lookups[slot];
        l[0] = on;
        l[1] = (inputs >> (field * 4)) & 7;
        l[2] = ((absolute >> (field * 4 + 1)) & 1) == 0;
        l[3] = float_bits(SCALES[(scales >> (field * 4)) & 7]);
    };
    auto table = [&](int slot, int which, uint32_t disabled, uint32_t field) {
        lookup(slot, field, !(config1 >> disabled & 1) && table_supported(config, which));
    };
    table(LOOKUP_D0, DISTRIBUTION0, 16, 0);
    table(LOOKUP_D1, DISTRIBUTION1, 17, 1);
    table(LOOKUP_FRESNEL, FRESNEL, 19, 3);
    table(LOOKUP_RED, REFLECT_RED, 20, 6);
    table(LOOKUP_GREEN, REFLECT_GREEN, 21, 5);
    table(LOOKUP_BLUE, REFLECT_BLUE, 22, 4);
    lookup(LOOKUP_SPOT, 2, true); // each light says whether it has one

    uint32_t count = (regs[LIGHT_COUNT] & 7) + 1;
    bool geometric = false, spotlight = false;
    for (uint32_t slot = 0; slot < count; slot++) {
        uint32_t number = (regs[LIGHT_SLOTS] >> (slot * 4)) & 7;
        const uint32_t *block = regs + LIGHTS + number * 0x10;
        LightSlot &light = b.lights[slot];
        light_color(block[0], light.specular0);
        light_color(block[1], light.specular1);
        light_color(block[2], light.diffuse);
        light_color(block[3], light.ambient);
        float x = pica_float(block[4] & 0xFFFF, 10, 5), y = pica_float(block[4] >> 16 & 0xFFFF, 10, 5),
              z = pica_float(block[5] & 0xFFFF, 10, 5);
        set3(light.position, x, y, z);
        float length = std::sqrt(x * x + y * y + z * z);
        set3(light.direction, x / length, y / length, z / length);
        set3(light.spot, fixed_1_11(block[6]), fixed_1_11(block[6] >> 16), fixed_1_11(block[7]));
        bool distance = !(config1 >> (24 + number) & 1);
        if (distance) {
            light.distance[0] = pica_float(block[10] & 0xFFFFF, 12, 7);
            light.distance[1] = pica_float(block[11] & 0xFFFFF, 12, 7);
        }
        bool spot = !(config1 >> (8 + number) & 1) && table_supported(config, SPOTLIGHT);
        uint32_t c = block[9];
        // directional, two-sided, geometric 0 and 1 are the config's own bits
        light.flags[0] = (c & 0xF) | uint32_t(distance) << 4 | uint32_t(spot) << 5 |
                         uint32_t(!(config1 >> number & 1)) << 6;
        light.flags[1] = number;
        geometric |= (c & 0xC) != 0;
        spotlight |= spot;
    }

    uint32_t bump = 0;
    switch ((config0 >> 28) & 3) {
    case 1: bump = 1 | ((config0 >> 22) & 3) << 4 | uint32_t(!(config0 >> 30 & 1)) << 8; break;
    case 2: bump = 2 | ((config0 >> 22) & 3) << 4; break;
    }
    uint32_t shadow = 0;
    if (config0 & 1) {
        shadow = 1 | ((config0 >> 24) & 3) << 4 | (config0 >> 18 & 1) << 8 | (config0 >> 16 & 1) << 9 |
                 (config0 >> 17 & 1) << 10 | (config0 >> 19 & 1) << 11;
    }

    // the dot products the tables that get read take
    bool half_input = false, view_input = false;
    for (int slot = 0; slot < LOOKUPS; slot++) {
        bool read = slot == LOOKUP_SPOT ? spotlight : b.lookups[slot][0] != 0;
        if (!read) continue;
        uint32_t input = b.lookups[slot][1];
        half_input |= input == 0 || input == 1 || (input == 5 && config == 8);
        view_input |= input == 1 || input == 2;
    }
    bool needs_half = geometric || half_input, needs_view = needs_half || view_input;

    b.head[0] = config;
    b.head[1] = count;
    b.head[2] = bump;
    b.head[3] = shadow;
    b.flags[0] = config0 >> 2 & 1;
    b.flags[1] = config0 >> 3 & 1;
    b.flags[2] = config0 >> 27 & 1;
    b.flags[3] = uint32_t(needs_half) | uint32_t(needs_view) << 1;
    light_color(regs[GLOBAL_AMBIENT], b.global_ambient);
    return true;
}

void LightingDecoder::registers_read(const uint32_t *regs, std::array<uint32_t, READ> &out) {
    constexpr size_t LIGHT_REGS = CONFIG1 + 1 - LIGHTS;
    out[0] = regs[ENABLE];
    std::memcpy(&out[1], regs + LIGHTS, LIGHT_REGS * 4);
    out[LIGHT_REGS + 1] = regs[DISABLE];
    std::memcpy(&out[LIGHT_REGS + 2], regs + TABLE_ABSOLUTE, 3 * 4);
    out[LIGHT_REGS + 5] = regs[LIGHT_SLOTS];
}

bool LightingDecoder::decode(const uint32_t *regs, LightingBlock &block) {
    std::array<uint32_t, READ> now;
    registers_read(regs, now);
    if (!valid_ || now != read_) {
        on_ = decode_fresh(regs, block_);
        read_ = now;
        valid_ = true;
    }
    block = block_;
    return on_;
}

void decode_light_tables(const std::array<std::array<uint32_t, 256>, 32> &raw, LightTables &out) {
    for (int t = 0; t < TABLES; t++) {
        for (int e = 0; e < 256; e++) {
            uint32_t value = raw[t][e];
            float step = float((value >> 12) & 0x7FF) / 2047.0f;
            out[t][e] = {float(value & 0xFFF) / 4095.0f, (value >> 23 & 1) ? -step : step};
        }
    }
}

} // namespace runtime::pica
