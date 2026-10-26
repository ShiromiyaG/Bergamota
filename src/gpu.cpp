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

#ifdef RUNTIME_ZAKURO_GPU
#include "zakuro-gpu.h"

#include <chrono>

namespace runtime {

ZakuroGpu::ZakuroGpu(Memory &memory, bool use_hardware, uint64_t title) : memory_(memory) {
    gpu_ = zgpu_new(memory.host(map::FCRAM_PADDR, 1), memory.fcram_size(), memory.host(map::VRAM_PADDR, map::VRAM_SIZE),
                    map::VRAM_SIZE, memory.host(map::AXI_WRAM_PADDR, map::AXI_WRAM_SIZE), map::AXI_WRAM_SIZE);
    hardware = use_hardware && zgpu_enable_hardware(gpu_, 1, title);
    if (hardware) {
        // what the GPU draws stays on its side until the CPU gets there
        zgpu_set_guard(
            gpu_,
            [](void *user, uint32_t paddr, uint32_t len, int32_t write) {
                Memory &m = *static_cast<Memory *>(user);
                if (paddr >= map::FCRAM_PADDR) m.guard(m.linear_base + (paddr - map::FCRAM_PADDR), len, write);
                else if (paddr >= map::VRAM_PADDR && paddr < map::VRAM_PADDR + map::VRAM_SIZE)
                    m.guard(map::VRAM_VADDR + (paddr - map::VRAM_PADDR), len, write);
            },
            &memory_);
        memory_.gpu_sync = [this](uint32_t vaddr, uint32_t len, bool write) {
            uint32_t paddr = vaddr >= map::VRAM_VADDR && vaddr < map::VRAM_VADDR + map::VRAM_SIZE
                                 ? map::VRAM_PADDR + (vaddr - map::VRAM_VADDR)
                                 : map::FCRAM_PADDR + (vaddr - memory_.linear_base);
            if (write) zgpu_sync(gpu_, paddr, len);
            else zgpu_sync_depth(gpu_, paddr, len);
        };
    }
}
ZakuroGpu::~ZakuroGpu() {
    memory_.gpu_sync = nullptr;
    zgpu_free(gpu_);
}
void ZakuroGpu::write_register(uint32_t offset, uint32_t value) { zgpu_write_register(gpu_, offset, value); }
uint32_t ZakuroGpu::read_register(uint32_t offset) { return zgpu_read_register(gpu_, offset); }
void ZakuroGpu::command_list(uint32_t paddr, uint32_t size) {
    auto start = std::chrono::steady_clock::now();
    zgpu_command_list(gpu_, paddr, size);
    busy += std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    lists++;
}
void ZakuroGpu::memory_fill(uint32_t start, uint32_t end, uint32_t value, uint32_t width) {
    zgpu_memory_fill(gpu_, start, end, value, width);
}
void ZakuroGpu::display_transfer(uint32_t input, uint32_t output, uint32_t input_size, uint32_t output_size,
                                 uint32_t flags) {
    auto start = std::chrono::steady_clock::now();
    zgpu_display_transfer(gpu_, input, output, input_size, output_size, flags);
    busy += std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
}
void ZakuroGpu::texture_copy(uint32_t input, uint32_t output, uint32_t size, uint32_t input_gap, uint32_t output_gap) {
    zgpu_texture_copy(gpu_, input, output, size, input_gap, output_gap);
}
void ZakuroGpu::framebuffer(uint32_t screen, uint32_t index, uint32_t left, uint32_t right, uint32_t stride,
                            uint32_t format) {
    zgpu_set_framebuffer(gpu_, screen, index, left, right, stride, format);
}
void ZakuroGpu::sync(uint32_t paddr, uint32_t size) {
    if (hardware && size) zgpu_sync(gpu_, paddr, size);
}
Picture ZakuroGpu::screen(uint32_t screen) {
    uint32_t address, stride, format;
    zgpu_framebuffer(gpu_, screen, &address, &stride, &format);
    // what the host GPU drew there, into memory first
    if (hardware) zgpu_sync(gpu_, address, (stride ? stride : 240 * 4) * (screen ? 320 : 400));
    return read_screen(memory_, screen, address, stride, format);
}

} // namespace runtime
#endif
