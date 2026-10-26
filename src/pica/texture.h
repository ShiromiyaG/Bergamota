// The PICA's texture formats (texture.cpp).
#ifndef RUNTIME_PICA_TEXTURE_H
#define RUNTIME_PICA_TEXTURE_H

#include <cstdint>

namespace runtime::pica {

// bits a texel of a format takes
uint32_t texture_bits(uint32_t format);
// a texture in memory (8x8 tiles) to RGBA8 rows, the first row the first
// in memory
void decode_texture(uint32_t format, const uint8_t *data, uint32_t width, uint32_t height, uint8_t *rgba);

} // namespace runtime::pica

#endif
