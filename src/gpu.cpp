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

} // namespace runtime
