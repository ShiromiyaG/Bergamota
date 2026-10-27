// The PICA's texture formats (texture.cpp).
#ifndef RUNTIME_PICA_TEXTURE_H
#define RUNTIME_PICA_TEXTURE_H

#include <cstdint>
#include <vector>

namespace runtime::pica {

// bits a texel of a format takes
uint32_t texture_bits(uint32_t format);
// a texture in memory (8x8 tiles) to RGBA8 rows, the first row the first
// in memory
void decode_texture(uint32_t format, const uint8_t *data, uint32_t width, uint32_t height, uint8_t *rgba);

// a texture's mipmap levels, one after the other in memory from the first's
// address, each half the size of the last: up to max_level (the level
// register's bits 16-19), and none narrower than a tile
struct Level {
    uint32_t offset, width, height;
};
std::vector<Level> texture_levels(uint32_t format, uint32_t width, uint32_t height, uint32_t max_level);
// the bytes they take together
uint32_t texture_bytes(uint32_t format, const std::vector<Level> &levels);

} // namespace runtime::pica

#endif
