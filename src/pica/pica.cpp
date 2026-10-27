// The PICA200's front end and its memory engines. After Zakuro's
// zakuro-gpu lib.rs (Rust, MIT, Copyright (c) 2026 fearkov; see
// LICENSE-zakuro), written anew.

#include "runtime/pica.h"
#include "runtime/memory.h"

#include <algorithm>
#include <bit>
#include <chrono>
#include <cstring>

namespace runtime::pica {

float f24(uint32_t raw) {
    raw &= 0xFFFFFF;
    if (!raw) return 0.0f;
    uint32_t sign = raw >> 23, exponent = (raw >> 16) & 0x7F, mantissa = raw & 0xFFFF;
    if (!exponent) return sign ? -0.0f : 0.0f; // subnormals are zero
    if (exponent == 0x7F) return std::bit_cast<float>(sign << 31 | 0x7F800000u);
    return std::bit_cast<float>(sign << 31 | (exponent + 64) << 23 | mantissa << 7);
}

namespace {

// four 24-bit floats in three words, the most significant first: w, z, y, x
Vec4 unpack24(const uint32_t w[3]) {
    return {f24(w[2]), f24((w[1] & 0xFFFF) << 8 | w[2] >> 24), f24((w[0] & 0xFF) << 16 | w[1] >> 16), f24(w[0] >> 8)};
}

constexpr uint32_t byte_mask(uint32_t mask) {
    return (mask & 1 ? 0xFFu : 0) | (mask & 2 ? 0xFF00u : 0) | (mask & 4 ? 0xFF0000u : 0) | (mask & 8 ? 0xFF000000u : 0);
}

} // namespace

void ShaderUnit::configure(uint32_t offset, uint32_t value) {
    switch (offset) {
    case BOOL_UNIFORMS: bools = uint16_t(value); break;
    case INT_UNIFORMS: case INT_UNIFORMS + 1: case INT_UNIFORMS + 2: case INT_UNIFORMS + 3:
        std::memcpy(ints[offset - INT_UNIFORMS].data(), &value, 4);
        break;
    case ENTRY_POINT: entry = value & 0xFFF; break;
    case UNIFORM_INDEX:
        float_at_ = value & 0x7F;
        wide_ = value >> 31;
        staged_count_ = 0;
        break;
    case PROGRAM_INDEX: program_at_ = value & 0xFFF; break;
    case DESCRIPTOR_INDEX: descriptor_at_ = value & 0x7F; break;
    default:
        if (offset >= UNIFORM_DATA && offset < UNIFORM_DATA + 8) {
            // a vector a word at a time: four floats, w first, or three words of 24-bit ones
            staged_[staged_count_++] = value;
            if (staged_count_ < (wide_ ? 4u : 3u)) break;
            staged_count_ = 0;
            Vec4 v = wide_ ? Vec4{std::bit_cast<float>(staged_[3]), std::bit_cast<float>(staged_[2]),
                                  std::bit_cast<float>(staged_[1]), std::bit_cast<float>(staged_[0])}
                           : unpack24(staged_);
            if (float_at_ < floats.size()) floats[float_at_] = v;
            float_at_++;
        } else if (offset >= PROGRAM_DATA && offset < PROGRAM_DATA + 8) {
            if (program_at_ < program.size()) {
                if (program[program_at_] != value) program[program_at_] = value, version++;
                program_at_++;
            }
        } else if (offset >= DESCRIPTOR_DATA && offset < DESCRIPTOR_DATA + 8) {
            if (descriptor_at_ < descriptors.size()) {
                if (descriptors[descriptor_at_] != value) descriptors[descriptor_at_] = value, version++;
                descriptor_at_++;
            }
        }
    }
}

Pica::Pica(Memory &m) : memory(m) {
    for (Vec4 &a : fixed_attributes) a = {0, 0, 0, 1};
}

// the external registers, from 0x1EF00000; gsp hands their offsets from
// 0x1EB00000
void Pica::write_register(uint32_t offset, uint32_t value) {
    if (offset < 0x400000 || (offset - 0x400000) / 4 >= external_.size()) return;
    uint32_t at = offset - 0x400000;
    external_[at / 4] = value;
    // the LCDs' framebuffer registers
    for (uint32_t s = 0; s < 2; s++) {
        uint32_t base = s ? 0x500 : 0x400, select = external_[(base + 0x78) / 4] & 1;
        Screen &screen = screens[s];
        screen.address = external_[(base + (select ? 0x94 : 0x68)) / 4];
        screen.format = external_[(base + 0x70) / 4];
        screen.stride = external_[(base + 0x90) / 4];
    }
}

uint32_t Pica::read_register(uint32_t offset) {
    if (offset < 0x400000 || (offset - 0x400000) / 4 >= external_.size()) return 0;
    return external_[(offset - 0x400000) / 4];
}

void Pica::framebuffer(uint32_t screen, uint32_t index, uint32_t left, uint32_t, uint32_t stride, uint32_t format) {
    if (screen > 1) return;
    uint32_t base = 0x400000 + (screen ? 0x500 : 0x400);
    write_register(base + (index ? 0x94 : 0x68), left);
    write_register(base + 0x90, stride);
    write_register(base + 0x70, format);
    write_register(base + 0x78, index & 1);
}

void Pica::sync(uint32_t paddr, uint32_t size) {
    if (backend) backend->sync(paddr, size, true);
}

// -- command lists --------------------------------------------------------

void Pica::command_list(uint32_t paddr, uint32_t size) {
    auto start = std::chrono::steady_clock::now();
    run(paddr, size);
    flush_immediate();
    lists++;
    busy += std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
}

void Pica::run(uint32_t paddr, uint32_t size) {
    std::vector<uint32_t> words(size / 4);
    for (int jumps = 0; jumps < 0x10000; jumps++) {
        const uint8_t *bytes = memory.host(paddr, size);
        if (!bytes) return;
        words.resize(size / 4);
        std::memcpy(words.data(), bytes, words.size() * 4);
        jump_ = false;
        // a command: its first value, a header (register, byte mask, how many
        // more values, whether they go to the registers after), the values,
        // and a word to keep the next command 8-byte aligned
        for (size_t i = 0; i + 1 < words.size() && !jump_;) {
            uint32_t value = words[i], header = words[i + 1];
            uint32_t reg = header & 0xFFFF, mask = (header >> 16) & 0xF, extra = (header >> 20) & 0xFF;
            bool consecutive = header >> 31;
            i += 2;
            if (reg >= REGISTERS) return; // out of step
            write(reg, value, mask);
            for (uint32_t n = 0; n < extra && i < words.size(); n++, i++) {
                write(consecutive ? reg + 1 + n : reg, words[i], mask);
            }
            if (extra & 1) i++;
        }
        if (!jump_) return;
        // a jump replaces the list
        paddr = jump_address_, size = jump_size_;
    }
}

void Pica::count_features() {
    uint32_t fog = regs[0x0E0] & 7, op = regs[COLOR_OPERATION];
    feature_draws[USES_FOG] += fog == 5;
    feature_draws[USES_GAS] += fog == 7 || (op & 3) == 1;
    feature_draws[USES_SHADOW] += (op & 3) == 3;
    feature_draws[USES_PROCTEX] += (regs[0x080] >> 3) & 1;
    // blending off is the logic op's mode; 3 is copy
    feature_draws[USES_LOGIC_OP] += !(op >> 8 & 1) && (regs[LOGIC_OP] & 0xF) != 3;
    feature_draws[USES_GEOMETRY] += (regs[GEOSTAGE_CONFIG] & 3) == 2;
    // the units on (bits 0-2): levels past the first (bits 16-19 of each's
    // level register); unit 0's type in bits 28-30 of its parameters
    static constexpr uint32_t LEVELS[3] = {0x084, 0x094, 0x09C};
    uint32_t units = regs[TEXTURE_CONFIG] & 7;
    bool mipmap = false;
    for (int u = 0; u < 3; u++) mipmap |= (units >> u & 1) && (regs[LEVELS[u]] >> 16 & 15) != 0;
    feature_draws[USES_MIPMAP] += mipmap;
    uint32_t type = units & 1 ? regs[0x083] >> 28 & 7 : 0;
    feature_draws[USES_CUBE] += type == 1 || type == 4;
    feature_draws[USES_SHADOW_TEXTURE] += type == 2 || type == 4;
    feature_draws[USES_PROJECTION] += type == 3;
    // 0 off, 1 the outside of the rectangle drawn, 3 the inside; the inside
    // counted when the rectangle (corners inclusive, x in bits 0-9 and y in
    // 16-25) leaves some of the buffer out
    uint32_t scissor = regs[0x065] & 3, low = regs[0x066], high = regs[0x067], dims = regs[FRAMEBUFFER_DIMENSIONS];
    uint32_t width = dims & 0x7FF, height = ((dims >> 12) & 0x3FF) + 1;
    bool cuts = (low & 0x3FF) || (low >> 16 & 0x3FF) || (high & 0x3FF) + 1 < width || (high >> 16 & 0x3FF) + 1 < height;
    feature_draws[USES_SCISSOR] += scissor == 3 && cuts;    feature_draws[USES_SCISSOR_OUTSIDE] += scissor == 1;
}

void Pica::flush_immediate() {
    if (immediate_vertices_.empty()) return;
    draws++;
    count_features();
    if (backend) backend->draw_immediate(*this, immediate_vertices_);
    immediate_vertices_.clear();
}

void Pica::write(uint32_t reg, uint32_t value, uint32_t mask) {
    if (reg >= REGISTERS) return;
    // immediate vertices draw with the state they came under
    if (reg < FIXED_ATTRIBUTE_DATA || reg > FIXED_ATTRIBUTE_DATA + 2) flush_immediate();
    uint32_t bits = byte_mask(mask);
    uint32_t now = (regs[reg] & ~bits) | (value & bits);
    regs[reg] = now;

    switch (reg) {
    case DRAW_ARRAYS:
    case DRAW_ELEMENTS:
        draws++;
        count_features();
        if (backend) backend->draw(*this, reg == DRAW_ELEMENTS);
        return;
    case CMDBUF_JUMP0:
    case CMDBUF_JUMP1: {
        uint32_t channel = reg - CMDBUF_JUMP0;
        jump_ = true;
        jump_size_ = (regs[CMDBUF_SIZE0 + channel] & 0x1FFFFF) * 8;
        jump_address_ = (regs[CMDBUF_ADDR0 + channel] & 0x1FFFFFFF) * 8;
        return;
    }
    case FIXED_ATTRIBUTE_INDEX:
        fixed_count_ = 0;
        immediate_next_ = 0;
        return;
    case FIXED_ATTRIBUTE_DATA:
    case FIXED_ATTRIBUTE_DATA + 1:
    case FIXED_ATTRIBUTE_DATA + 2: {
        fixed_words_[fixed_count_++] = now;
        if (fixed_count_ < 3) return;
        fixed_count_ = 0;
        Vec4 attribute = unpack24(fixed_words_);
        uint32_t index = regs[FIXED_ATTRIBUTE_INDEX] & 0xF;
        if (index < 15) {
            // set, and on to the next, so a burst sets several
            fixed_attributes[index] = attribute;
            regs[FIXED_ATTRIBUTE_INDEX] = (regs[FIXED_ATTRIBUTE_INDEX] & ~0xFu) | (index + 1);
        } else {
            // immediate mode: the attributes in order, the last makes a vertex
            immediate_[immediate_next_] = attribute;
            if (immediate_next_ < (regs[VS_ATTRIBUTE_COUNT] & 0xF)) {
                immediate_next_++;
            } else {
                immediate_next_ = 0;
                immediate_vertices_.push_back(immediate_);
            }
        }
        return;
    }
    default: break;
    }

    // the tables: an index register says where, and moves on
    if (reg >= LIGHT_TABLE_DATA && reg <= LIGHT_TABLE_DATA + 7) {
        uint32_t index = regs[LIGHT_TABLE_INDEX], entry = index & 0xFF;
        auto &slot = light_tables[(index >> 8) & 0x1F][entry];
        if (slot != now) slot = now, tables_version++;
        regs[LIGHT_TABLE_INDEX] = (index & ~0xFFu) | ((entry + 1) & 0xFF);
    } else if (reg >= PROCTEX_TABLE_DATA && reg <= PROCTEX_TABLE_DATA + 7) {
        uint32_t index = regs[PROCTEX_TABLE_INDEX], entry = index & 0xFF;
        auto &slot = proctex_tables[(index >> 8) & 0x7][entry];
        if (slot != now) slot = now, tables_version++;
        regs[PROCTEX_TABLE_INDEX] = (index & ~0xFFu) | ((entry + 1) & 0xFF);
    } else if (reg >= FOG_TABLE_DATA && reg <= FOG_TABLE_DATA + 7) {
        uint32_t index = regs[FOG_TABLE_INDEX];
        auto &slot = fog_table[index % fog_table.size()];
        if (slot != now) slot = now, tables_version++;
        regs[FOG_TABLE_INDEX] = index + 1;
    } else if (reg >= GS_BLOCK && reg < GS_BLOCK + SHADER_BLOCK) {
        geometry_shader.configure(reg - GS_BLOCK, now);
    } else if (reg >= VS_BLOCK && reg < VS_BLOCK + SHADER_BLOCK) {
        uint32_t offset = reg - VS_BLOCK;
        vertex_shader.configure(offset, now);
        // the geometry unit takes the vertex program and descriptors too,
        // unless the game set it apart
        bool program = (offset >= PROGRAM_INDEX && offset < PROGRAM_DATA + 8) ||
                       (offset >= DESCRIPTOR_INDEX && offset < DESCRIPTOR_DATA + 8);
        if (program && !(regs[VS_COM_MODE] & 1)) geometry_shader.configure(offset, now);
    }
}

// -- colors and tiling ------------------------------------------------------

uint32_t bytes_per_pixel(uint32_t format) {
    static constexpr uint32_t SIZES[8] = {4, 3, 2, 2, 2, 4, 4, 4};
    return SIZES[format & 7];
}

std::array<uint8_t, 4> decode_color(uint32_t format, const uint8_t *in) {
    uint16_t v = uint16_t(in[0] | in[1] << 8);
    auto five = [](uint32_t c) { return uint8_t(c << 3 | c >> 2); };
    switch (format & 7) {
    case 0: return {in[3], in[2], in[1], in[0]}; // RGBA8: A B G R in memory
    case 1: return {in[2], in[1], in[0], 255};   // RGB8: B G R
    case 2: return {five(v >> 11), uint8_t((v >> 5 & 63) << 2 | (v >> 5 & 63) >> 4), five(v & 31), 255};
    case 3: return {five(v >> 11), five(v >> 6 & 31), five(v >> 1 & 31), uint8_t(v & 1 ? 255 : 0)};
    case 4: return {uint8_t((v >> 12) * 17), uint8_t((v >> 8 & 15) * 17), uint8_t((v >> 4 & 15) * 17), uint8_t((v & 15) * 17)};
    default: return {in[3], in[2], in[1], in[0]};
    }
}

void encode_color(uint32_t format, std::array<uint8_t, 4> c, uint8_t *out) {
    uint16_t v;
    switch (format & 7) {
    case 1: out[0] = c[2], out[1] = c[1], out[2] = c[0]; return;
    case 2: v = uint16_t((c[0] >> 3) << 11 | (c[1] >> 2) << 5 | c[2] >> 3); break;
    case 3: v = uint16_t((c[0] >> 3) << 11 | (c[1] >> 3) << 6 | (c[2] >> 3) << 1 | (c[3] >= 0x80)); break;
    case 4: v = uint16_t((c[0] >> 4) << 12 | (c[1] >> 4) << 8 | (c[2] >> 4) << 4 | c[3] >> 4); break;
    default: out[0] = c[3], out[1] = c[2], out[2] = c[1], out[3] = c[0]; return;
    }
    out[0] = uint8_t(v), out[1] = uint8_t(v >> 8);
}

uint32_t tiled_offset(uint32_t x, uint32_t y, uint32_t width, uint32_t bpp) {
    uint32_t in_tile = (x & 1) | (y & 1) << 1 | (x & 2) << 1 | (y & 2) << 2 | (x & 4) << 2 | (y & 4) << 3;
    return ((y / 8 * (width / 8) + x / 8) * 64 + in_tile) * bpp;
}

// -- the memory engines -------------------------------------------------------

void Pica::memory_fill(uint32_t start, uint32_t end, uint32_t value, uint32_t width) {
    if (end <= start) return;
    width = width == 2 || width == 3 ? width : 4;
    if (backend) backend->fill(start, end, value, width);
    uint8_t *at = memory.host(start, end - start);
    if (!at) return;
    for (uint32_t i = 0; i + width <= end - start; i += width) std::memcpy(at + i, &value, width);
    fills++;
}

void Pica::display_transfer(uint32_t input, uint32_t output, uint32_t input_size, uint32_t output_size, uint32_t flags) {
    auto start = std::chrono::steady_clock::now();
    // flags: 0 flip, 1 linear in (tiled out), 2 crop, 5 keep the layout,
    // 8-10 and 12-14 the formats, 24-25 downscale
    uint32_t in_w = input_size & 0xFFFF, in_h = input_size >> 16;
    bool flip = flags & 1, linear_in = flags & 2, tiled_out = linear_in != bool(flags & 0x20);
    uint32_t in_format = (flags >> 8) & 7, out_format = (flags >> 12) & 7, down = (flags >> 24) & 3;
    uint32_t sx = down ? 2 : 1, sy = down == 2 ? 2 : 1;
    uint32_t out_w = (output_size & 0xFFFF) / sx, out_h = (output_size >> 16) / sy;
    uint32_t copy_w = std::min(out_w, in_w / sx), copy_h = std::min(out_h, in_h / sy);
    uint32_t in_bpp = bytes_per_pixel(in_format), out_bpp = bytes_per_pixel(out_format);
    // flipped and cropped, the output starts as much later as the lines
    // cropped off take, all but the last
    if (flip && (flags & 4)) output += (in_w > (output_size & 0xFFFF) ? in_w - (output_size & 0xFFFF) : 0) * (out_h ? out_h - 1 : 0) * out_bpp;
    uint32_t in_bytes = in_w * in_h * in_bpp, out_bytes = out_w * out_h * out_bpp;
    if (backend && copy_w && copy_h &&
        backend->display_transfer({input, output, in_w, in_h, out_w, out_h, copy_w, copy_h, sx, sy, out_format, flip, tiled_out})) {
        transfers++;
        busy += std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
        return;
    }
    if (backend) {
        backend->sync(input, in_bytes, false);
        backend->sync(output, out_bytes, true);
    }
    const uint8_t *in = memory.host(input, in_bytes);
    uint8_t *out = memory.host(output, out_bytes);
    if (in && out && copy_w && copy_h) {
        for (uint32_t y = 0; y < copy_h; y++) {
            uint32_t oy = flip ? out_h - 1 - y : y;
            for (uint32_t x = 0; x < copy_w; x++) {
                // a downscale averages each 2x1 or 2x2 block
                uint32_t sum[4] = {}, n = 0;
                for (uint32_t dy = 0; dy < sy; dy++) {
                    for (uint32_t dx = 0; dx < sx; dx++) {
                        uint32_t ix = x * sx + dx, iy = y * sy + dy;
                        uint32_t at = linear_in ? (iy * in_w + ix) * in_bpp : tiled_offset(ix, iy, in_w, in_bpp);
                        if (at + in_bpp > in_bytes) continue;
                        auto c = decode_color(in_format, in + at);
                        for (int k = 0; k < 4; k++) sum[k] += c[k];
                        n++;
                    }
                }
                if (!n) continue;
                uint32_t at = tiled_out ? tiled_offset(x, oy, out_w, out_bpp) : (oy * out_w + x) * out_bpp;
                if (at + out_bpp > out_bytes) continue;
                encode_color(out_format,
                             {uint8_t((sum[0] + n / 2) / n), uint8_t((sum[1] + n / 2) / n), uint8_t((sum[2] + n / 2) / n),
                              uint8_t((sum[3] + n / 2) / n)},
                             out + at);
            }
        }
    }
    transfers++;
    busy += std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
}

// size bytes in runs, a gap skipped after each run on either side (the low
// half of a gap word is the run, the high half the gap, both in 16s)
void Pica::texture_copy(uint32_t input, uint32_t output, uint32_t size, uint32_t input_gap, uint32_t output_gap) {
    uint32_t in_run = (input_gap & 0xFFFF) * 16, in_skip = (input_gap >> 16) * 16;
    uint32_t out_run = (output_gap & 0xFFFF) * 16, out_skip = (output_gap >> 16) * 16;
    if (!in_run) in_run = size, in_skip = 0;
    if (!out_run) out_run = size, out_skip = 0;
    if (backend) {
        backend->sync(input, size + (size / in_run + 1) * in_skip, false);
        backend->sync(output, size + (size / out_run + 1) * out_skip, true);
    }
    uint32_t in = input, out = output, in_left = in_run, out_left = out_run;
    for (uint32_t done = 0; done < size;) {
        uint32_t n = std::min({in_left, out_left, size - done});
        uint8_t *src = memory.host(in, n), *dst = memory.host(out, n);
        if (src && dst) std::memmove(dst, src, n);
        in += n, out += n, done += n, in_left -= n, out_left -= n;
        if (!in_left) in += in_skip, in_left = in_run;
        if (!out_left) out += out_skip, out_left = out_run;
    }
    transfers++;
}

Picture Pica::screen(uint32_t s) {
    const Screen &screen = screens[s & 1];
    // from the GPU, or what it keeps of it into memory first
    uint32_t stride = screen.stride ? screen.stride : 240 * bytes_per_pixel(screen.format);
    Picture direct;
    if (backend && backend->picture(s & 1, screen.address, stride, screen.format, direct)) return direct;
    if (backend) backend->sync(screen.address, stride * (s & 1 ? 320 : 400), false);
    return read_screen(memory, s & 1, screen.address, screen.stride, screen.format);
}

} // namespace runtime::pica
