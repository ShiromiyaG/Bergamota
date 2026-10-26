// The PICA's texture formats, decoded to RGBA8. After Zakuro's zakuro-gpu
// texture.rs (Rust, MIT, Copyright (c) 2026 fearkov; see LICENSE-zakuro).

#include "texture.h"

#include <algorithm>
#include <cstring>

namespace runtime::pica {

uint32_t texture_bits(uint32_t format) {
    static constexpr uint32_t BITS[16] = {32, 24, 16, 16, 16, 16, 16, 8, 8, 8, 4, 4, 4, 8, 32, 32};
    return BITS[format & 15];
}

namespace {

uint8_t x4(uint32_t v) { return uint8_t(v << 4 | v); }
uint8_t x5(uint32_t v) { return uint8_t(v << 3 | v >> 2); }
uint8_t x6(uint32_t v) { return uint8_t(v << 2 | v >> 4); }

uint32_t z_order(uint32_t x, uint32_t y) {
    return (x & 1) | (y & 1) << 1 | (x & 2) << 1 | (y & 2) << 2 | (x & 4) << 2 | (y & 4) << 3;
}

void texel(uint32_t format, const uint8_t *b, uint8_t *o) {
    uint16_t v = uint16_t(b[0] | b[1] << 8);
    switch (format) {
    case 0: o[0] = b[3], o[1] = b[2], o[2] = b[1], o[3] = b[0]; break;
    case 1: o[0] = b[2], o[1] = b[1], o[2] = b[0], o[3] = 255; break;
    case 2: o[0] = x5(v >> 11), o[1] = x5(v >> 6 & 31), o[2] = x5(v >> 1 & 31), o[3] = v & 1 ? 255 : 0; break;
    case 3: o[0] = x5(v >> 11), o[1] = x6(v >> 5 & 63), o[2] = x5(v & 31), o[3] = 255; break;
    case 4: o[0] = x4(v >> 12), o[1] = x4(v >> 8 & 15), o[2] = x4(v >> 4 & 15), o[3] = x4(v & 15); break;
    case 5: o[0] = o[1] = o[2] = b[1], o[3] = b[0]; break;           // LA8
    case 6: o[0] = b[1], o[1] = b[0], o[2] = 0, o[3] = 255; break;   // HILO8
    case 7: o[0] = o[1] = o[2] = b[0], o[3] = 255; break;            // L8
    case 8: o[0] = o[1] = o[2] = 0, o[3] = b[0]; break;              // A8
    case 9: o[0] = o[1] = o[2] = x4(b[0] >> 4), o[3] = x4(b[0] & 15); break; // LA4
    default: o[0] = o[1] = o[2] = o[3] = 0;
    }
}

// one ETC1 block of 4x4, into rgba (16 texels, row by row)
void etc1(uint64_t word, uint8_t out[16][4]) {
    static constexpr int MOD[8][4] = {{2, 8, -2, -8}, {5, 17, -5, -17}, {9, 29, -9, -29}, {13, 42, -13, -42},
                                      {18, 60, -18, -60}, {24, 80, -24, -80}, {33, 106, -33, -106}, {47, 183, -47, -183}};
    uint32_t low = uint32_t(word), high = uint32_t(word >> 32);
    bool flip = high & 1, diff = high & 2;
    int base[2][3];
    if (diff) {
        auto sx3 = [](uint32_t v) { return int(v & 4 ? v - 8 : v); };
        int r = high >> 27 & 31, g = high >> 19 & 31, b = high >> 11 & 31;
        int c0[3] = {r, g, b}, d[3] = {sx3(high >> 24 & 7), sx3(high >> 16 & 7), sx3(high >> 8 & 7)};
        for (int i = 0; i < 3; i++) base[0][i] = x5(c0[i]), base[1][i] = x5(std::clamp(c0[i] + d[i], 0, 31));
    } else {
        for (int i = 0; i < 3; i++) base[0][i] = x4(high >> (28 - i * 8) & 15), base[1][i] = x4(high >> (24 - i * 8) & 15);
    }
    int table[2] = {int(high >> 5 & 7), int(high >> 2 & 7)};
    for (uint32_t y = 0; y < 4; y++) {
        for (uint32_t x = 0; x < 4; x++) {
            int half = (flip ? y : x) < 2 ? 0 : 1;
            // the index planes go column by column
            uint32_t i = x * 4 + y;
            int delta = MOD[table[half]][((low >> (i + 16)) & 1) << 1 | ((low >> i) & 1)];
            for (int c = 0; c < 3; c++) out[y * 4 + x][c] = uint8_t(std::clamp(base[half][c] + delta, 0, 255));
            out[y * 4 + x][3] = 255;
        }
    }
}

} // namespace

void decode_texture(uint32_t format, const uint8_t *data, uint32_t width, uint32_t height, uint8_t *rgba) {
    format &= 15;
    if (format == 12 || format == 13) { // ETC1, ETC1A4: 8x8 tiles of four 4x4 blocks
        uint32_t block = format == 13 ? 16 : 8;
        for (uint32_t ty = 0; ty < height / 8; ty++) {
            for (uint32_t tx = 0; tx < width / 8; tx++) {
                for (uint32_t sub = 0; sub < 4; sub++) {
                    const uint8_t *b = data + ((ty * (width / 8) + tx) * 4 + sub) * block;
                    uint64_t color, alpha = ~0ull;
                    if (format == 13) std::memcpy(&alpha, b, 8), std::memcpy(&color, b + 8, 8);
                    else std::memcpy(&color, b, 8);
                    uint8_t px[16][4];
                    etc1(color, px);
                    for (uint32_t y = 0; y < 4; y++) {
                        for (uint32_t x = 0; x < 4; x++) {
                            uint32_t ox = tx * 8 + (sub & 1) * 4 + x, oy = ty * 8 + (sub >> 1) * 4 + y;
                            uint8_t *o = rgba + (size_t(oy) * width + ox) * 4;
                            std::memcpy(o, px[y * 4 + x], 4);
                            if (format == 13) o[3] = x4(uint32_t(alpha >> ((x * 4 + y) * 4)) & 15);
                        }
                    }
                }
            }
        }
        return;
    }
    uint32_t bits = texture_bits(format);
    for (uint32_t y = 0; y < height; y++) {
        for (uint32_t x = 0; x < width; x++) {
            uint32_t n = (y / 8 * (width / 8) + x / 8) * 64 + z_order(x, y);
            uint8_t *o = rgba + (size_t(y) * width + x) * 4;
            if (bits == 4) { // L4, A4: two a byte, the low nibble first
                uint32_t v = data[n / 2] >> (n & 1 ? 4 : 0) & 15;
                if (format == 10) o[0] = o[1] = o[2] = x4(v), o[3] = 255;
                else o[0] = o[1] = o[2] = 0, o[3] = x4(v);
            } else {
                texel(format, data + n * (bits / 8), o);
            }
        }
    }
}

} // namespace runtime::pica
