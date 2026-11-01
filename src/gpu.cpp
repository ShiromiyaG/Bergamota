// A GPU that draws nothing: the memory work only.

#include "runtime/gpu.h"

#include <algorithm>
#include <cstring>

namespace runtime {

void NullGpu::write_register(uint32_t offset, uint32_t value) {
    if (offset / 4 < 0x1000) registers_[offset / 4] = value;
}

uint32_t NullGpu::read_register(uint32_t offset) { return offset / 4 < 0x1000 ? registers_[offset / 4] : 0; }

void NullGpu::memory_fill(uint32_t start, uint32_t end, uint32_t value, uint32_t width) {
    if (end <= start) return;
    uint8_t *at = memory_.host(start, end - start);
    if (!at) return;
    for (uint32_t i = 0; i + width <= end - start; i += width) std::memcpy(at + i, &value, width);
}

// copies size bytes in runs, skipping a gap after each on either side (the
// low half of a gap word is the run, the high half the gap, both in 16s)
void NullGpu::texture_copy(uint32_t input, uint32_t output, uint32_t size, uint32_t input_gap, uint32_t output_gap) {
    uint32_t in_run = (input_gap & 0xFFFF) * 16, in_skip = (input_gap >> 16) * 16;
    uint32_t out_run = (output_gap & 0xFFFF) * 16, out_skip = (output_gap >> 16) * 16;
    if (!in_run) in_run = size;
    if (!out_run) out_run = size;
    uint32_t in = input, out = output, in_left = in_run, out_left = out_run;
    for (uint32_t done = 0; done < size;) {
        uint32_t n = std::min({in_left, out_left, size - done});
        uint8_t *src = memory_.host(in, n), *dst = memory_.host(out, n);
        if (src && dst) std::memmove(dst, src, n);
        in += n, out += n, done += n, in_left -= n, out_left -= n;
        if (!in_left) in += in_skip, in_left = in_run;
        if (!out_left) out += out_skip, out_left = out_run;
    }
}

Picture read_screen(Memory &memory, uint32_t screen, uint32_t address, uint32_t stride, uint32_t format) {
    Picture p;
    p.width = screen == 0 ? 400 : 320;
    p.height = 240;
    p.rgba.assign(p.width * p.height * 4, 0);
    static const uint32_t sizes[8] = {4, 3, 2, 2, 2, 4, 4, 4};
    uint32_t bpp = sizes[format & 7];
    if (!stride) stride = 240 * bpp;
    const uint8_t *base = memory.host(address, stride * p.width);
    if (!base) return p;
    for (uint32_t x = 0; x < p.width; x++) {
        // a column of the screen is a row in memory, bottom to top
        const uint8_t *row = base + x * stride;
        for (uint32_t y = 0; y < p.height; y++) {
            const uint8_t *s = row + (p.height - 1 - y) * bpp;
            uint8_t *d = &p.rgba[(y * p.width + x) * 4];
            uint16_t v = uint16_t(s[0] | s[1] << 8);
            switch (format & 7) {
            case 0: d[0] = s[3], d[1] = s[2], d[2] = s[1]; break;          // RGBA8: A B G R in memory
            case 1: d[0] = s[2], d[1] = s[1], d[2] = s[0]; break;          // RGB8: B G R
            case 2: d[0] = uint8_t((v >> 11) << 3), d[1] = uint8_t(((v >> 5) & 63) << 2), d[2] = uint8_t((v & 31) << 3); break;
            case 3: d[0] = uint8_t((v >> 11) << 3), d[1] = uint8_t(((v >> 6) & 31) << 3), d[2] = uint8_t(((v >> 1) & 31) << 3); break;
            default: d[0] = uint8_t((v >> 12) << 4), d[1] = uint8_t(((v >> 8) & 15) << 4), d[2] = uint8_t(((v >> 4) & 15) << 4); break;
            }
            d[3] = 255;
        }
    }
    return p;
}

} // namespace runtime
