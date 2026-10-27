// The fog's and the procedural texture's tables decoded for the shader,
// and the procedural texture itself as the shader makes it, to check it by.
// Translated from Zakuro's fog.rs and proctex.rs, Table::write,
// Tables::write and ProcTex::sample (Rust, MIT, Copyright (c) 2026 fearkov;
// see LICENSE-zakuro; Zakuro follows what Citra worked out against the
// hardware).

#include "runtime/pica_tables.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>

namespace runtime::pica {

void decode_fog_table(const std::array<uint32_t, 128> &raw, FogTable &out) {
    for (size_t i = 0; i < out.size(); i++) {
        uint32_t value = raw[i];
        int32_t step = int32_t((value & 0x1FFF) << 19) >> 19;
        out[i] = {float((value >> 13) & 0x7FF) / 2048.0f, float(step) / 2048.0f};
    }
}

void decode_proctex_tables(const std::array<std::array<uint32_t, 256>, 8> &raw, ProcTexTables &out) {
    // a 0.12 value and the step to the next one, 12 bits with the sign on top
    static constexpr int MAPS[3] = {0, 2, 3};
    for (int m = 0; m < 3; m++) {
        for (size_t i = 0; i < 128; i++) {
            uint32_t value = raw[MAPS[m]][i];
            int32_t step = int32_t(((value >> 12) & 0xFFF) << 20) >> 20;
            out.maps[m][i] = {float(value & 0xFFF) / 4095.0f, float(step) / 4095.0f};
        }
    }
    // the steps come halved, signed bytes
    for (size_t i = 0; i < 256; i++) {
        for (int ch = 0; ch < 4; ch++) {
            out.colors[i][ch] = float((raw[4][i] >> (ch * 8)) & 0xFF);
            out.steps[i][ch] = float(int8_t(raw[5][i] >> (ch * 8))) * 2.0f;
        }
    }
}

namespace {

// a map table read at a coordinate from 0 to 1, between its entries
float lookup(const ProcTexTables &t, int map, float coordinate) {
    float at = coordinate * 128.0f;
    int entry = std::min(int(std::max(at, 0.0f)), 127);
    return t.maps[map][entry][0] + (at - float(entry)) * t.maps[map][entry][1];
}

// a pseudo-random value from -1 to 1 for a point of the noise's grid
float random(uint32_t x, uint32_t y) {
    static constexpr uint32_t ROWS[16] = {0, 4, 10, 8, 4, 9, 7, 12, 5, 15, 13, 14, 11, 15, 2, 11};
    static constexpr uint32_t MIX[16] = {10, 2, 15, 8, 0, 7, 4, 5, 5, 13, 2, 6, 13, 9, 3, 14};
    auto one = [](uint32_t v) { return (((v % 9 + 2) * 3) & 0xF) ^ ROWS[(v / 9) & 0xF]; };
    uint32_t u = one(x), v = one(y);
    if ((u & 3) == 1) v += 4;
    v ^= (u & 1) * 6;
    v += 10 + u;
    v &= 0xF;
    v ^= MIX[u];
    return -1.0f + float(v) * 2.0f / 15.0f;
}

// how far the shift moves one coordinate, by the other: odd or even rows
float shift(float other, uint32_t mode, uint32_t clamp) {
    float amount = clamp == 3 ? 1.0f : 0.5f;
    int32_t o = int32_t(other);
    switch (mode) {
    case 1: return amount * float((o / 2) % 2);
    case 2: return amount * float(((o + 1) / 2) % 2);
    default: return 0.0f;
    }
}

// a coordinate brought back to 0 to 1: to zero, to the edge, repeated,
// mirrored, a pulse
float wrap(float c, uint32_t mode) {
    switch (mode) {
    case 0: return c > 1.0f ? 0.0f : c;
    case 1: return std::min(c, 1.0f);
    case 2: return c - std::floor(c);
    case 3: {
        int32_t whole = int32_t(c);
        float part = c - float(whole);
        return whole % 2 == 0 ? part : 1.0f - part;
    }
    case 4: return c <= 0.5f ? 0.0f : 1.0f;
    default: return std::clamp(c, 0.0f, 1.0f);
    }
}

// the two coordinates made into one
float combine(float u, float v, uint32_t function) {
    float length = std::sqrt(u * u + v * v);
    switch (function) {
    case 0: return u;
    case 1: return u * u;
    case 2: return v;
    case 3: return v * v;
    case 4: return (u + v) * 0.5f;
    case 5: return (u * u + v * v) * 0.5f;
    case 6: return std::min(length, 1.0f);
    case 7: return std::min(u, v);
    case 8: return std::max(u, v);
    case 9: return std::min(((u + v) * 0.5f + length) * 0.5f, 1.0f);
    default: return 0.0f;
    }
}

// a 16-bit float: a sign, 5 bits of exponent and 10 of mantissa
float half(uint32_t h) {
    float sign = h & 0x8000 ? -1.0f : 1.0f;
    int32_t exponent = int32_t((h >> 10) & 0x1F);
    float mantissa = float(h & 0x3FF) / 1024.0f;
    return exponent ? sign * (1.0f + mantissa) * std::ldexp(1.0f, exponent - 15) : sign * mantissa * std::ldexp(1.0f, -14);
}

} // namespace

std::array<float, 4> proctex_sample(const uint32_t *regs, const ProcTexTables &t, float u, float v) {
    uint32_t config = regs[PROCTEX_CONFIG], noise_u = regs[PROCTEX_CONFIG + 1], noise_v = regs[PROCTEX_CONFIG + 2];
    uint32_t frequency = regs[PROCTEX_CONFIG + 3], lut = regs[PROCTEX_CONFIG + 4], offset = regs[PROCTEX_CONFIG + 5];
    uint32_t clamp_u = config & 7, clamp_v = (config >> 3) & 7;
    u = std::abs(u), v = std::abs(v);
    // the shifts come from the coordinates before the noise moves them
    float shift_u = shift(v, (config >> 16) & 3, clamp_u), shift_v = shift(u, (config >> 18) & 3, clamp_v);
    if (config >> 15 & 1) {
        float x = 9.0f * half(frequency & 0xFFFF) * std::abs(u + float(noise_u >> 16) / 4096.0f);
        float y = 9.0f * half(frequency >> 16) * std::abs(v + float(noise_v >> 16) / 4096.0f);
        // as Rust's casts: below zero is zero
        uint32_t xi = uint32_t(std::max(x, 0.0f)), yi = uint32_t(std::max(y, 0.0f));
        float xf = x - float(xi), yf = y - float(yi);
        float g0 = random(xi, yi) * (xf + yf), g1 = random(xi + 1, yi) * (xf + yf - 1.0f);
        float g2 = random(xi, yi + 1) * (xf + yf - 1.0f), g3 = random(xi + 1, yi + 1) * (xf + yf - 2.0f);
        float s = lookup(t, 0, xf), tt = lookup(t, 0, yf);
        float bottom = g0 * (1.0f - s) + g1 * s, top = g2 * (1.0f - s) + g3 * s;
        float noise = bottom * (1.0f - tt) + top * tt;
        u = std::abs(u + noise * float(int16_t(noise_u)) / 4095.0f);
        v = std::abs(v + noise * float(int16_t(noise_v)) / 4095.0f);
    }
    u = wrap(u + shift_u, clamp_u);
    v = wrap(v + shift_v, clamp_v);

    float coordinate = lookup(t, 1, combine(u, v, (config >> 6) & 0xF));
    float width = float((lut >> 11) & 0xFF);
    float at = float(offset & 0xFF) + coordinate * std::max(width - 1.0f, 0.0f);
    std::array<float, 4> color;
    switch (lut & 7) {
    case 1: case 3: case 5: { // linear, and linear with either kind of mipmap
        int entry = std::min(int(std::max(at, 0.0f)), 255);
        float part = at - float(entry);
        for (int ch = 0; ch < 4; ch++) color[ch] = t.colors[entry][ch] + part * t.steps[entry][ch];
        break;
    }
    default: {
        int entry = std::min(int(std::max(std::round(at), 0.0f)), 255);
        color = t.colors[entry];
    }
    }
    for (float &ch : color) ch = std::clamp(ch / 255.0f, 0.0f, 1.0f);
    // alpha of its own skips the color table, the map gives it
    if (config >> 14 & 1) color[3] = std::clamp(lookup(t, 2, combine(u, v, (config >> 10) & 0xF)), 0.0f, 1.0f);
    return color;
}

} // namespace runtime::pica
