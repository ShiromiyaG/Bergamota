// y2r: the movies' YUV to RGB, with the hardware's arithmetic (as Citra
// worked it out). After Zakuro's services/y2r.rs, leaner: a pixel at a
// time, no rotation (USUM's movies need none).
// Zakuro: Rust, MIT, Copyright (c) 2026 fearkov; see LICENSE-zakuro.

#include "runtime/services.h"

#include "runtime/fs.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <cstdio>

namespace runtime {

namespace {
// the standard conversions: BT.601 and 709, full range and scaled
constexpr int32_t Y2R_STANDARD[4][8] = {
    {0x100, 0x166, 0xB6, 0x58, 0x1C5, -0x166F, 0x10EE, -0x1C5B},
    {0x100, 0x193, 0x77, 0x2F, 0x1DB, -0x1933, 0xA7C, -0x1D51},
    {0x12A, 0x198, 0xD0, 0x64, 0x204, -0x1BDE, 0x10F2, -0x229B},
    {0x12A, 0x1CA, 0x88, 0x36, 0x21C, -0x1F04, 0x99C, -0x2421},
};

uint32_t morton(uint32_t x, uint32_t y) {
    return (x & 1) | (y & 1) << 1 | (x & 2) << 1 | (y & 2) << 2 | (x & 4) << 2 | (y & 4) << 3;
}

// samples from a buffer read in transfers, a gap skipped after each; a
// 16-bit sample keeps its low byte
std::vector<uint8_t> gather(Memory &m, uint32_t address, uint32_t transfer, uint32_t gap, size_t count, uint32_t width) {
    std::vector<uint8_t> out;
    out.reserve(count);
    transfer = std::max(transfer, width);
    std::vector<uint8_t> piece(transfer);
    while (out.size() < count) {
        m.read_bytes(address, piece.data(), transfer);
        for (uint32_t i = 0; i < transfer && out.size() < count; i += width) out.push_back(piece[i]);
        address += transfer + gap;
    }
    return out;
}

int32_t channel(int32_t sum) { return std::clamp(int32_t(int16_t(std::clamp(sum >> 8, -32768, 32767))), 0, 255); }
} // namespace

void Services::y2r_convert(System &system) {
    Y2r &s = y2r_;
    uint32_t width = s.width, lines = s.lines;
    if (!width || !lines || width % 8) return;
    Memory &m = system.memory;
    size_t pixels = size_t(width) * lines;
    uint32_t sample = s.input == 2 || s.input == 3 ? 2 : 1;
    std::vector<uint8_t> Y, U, V;
    uint32_t chroma_lines = 1; // lines sharing a chroma line: 4:2:2 one, 4:2:0 two
    if (s.input <= 3) {
        bool half_lines = s.input == 1 || s.input == 3;
        chroma_lines = half_lines ? 2 : 1;
        Y = gather(m, s.y.address, s.y.transfer, s.y.gap, pixels, sample);
        U = gather(m, s.u.address, s.u.transfer, s.u.gap, pixels / (2 * chroma_lines), sample);
        V = gather(m, s.v.address, s.v.transfer, s.v.gap, pixels / (2 * chroma_lines), sample);
    } else { // YUYV, interleaved
        std::vector<uint8_t> all = gather(m, s.yuyv.address, s.yuyv.transfer, s.yuyv.gap, pixels * 2, 1);
        for (size_t i = 0; i + 3 < all.size(); i += 4) {
            Y.push_back(all[i]);
            U.push_back(all[i + 1]);
            Y.push_back(all[i + 2]);
            V.push_back(all[i + 3]);
        }
    }
    const int32_t *c = s.coefficients;
    int32_t red = (c[5] + 0x18) * 8, green = (c[6] + 0x18) * 8, blue = (c[7] + 0x18) * 8;
    uint32_t bytes = s.output == 0 ? 4 : s.output == 1 ? 3 : 2;
    std::vector<uint8_t> out(pixels * bytes);
    for (uint32_t y = 0; y < lines; y++) {
        uint32_t chroma_row = s.input <= 3 ? (y / chroma_lines) * (width / 2) : y * (width / 2);
        for (uint32_t x = 0; x < width; x++) {
            int32_t luma = int16_t(c[0]) * int32_t(Y[size_t(y) * width + x]);
            int32_t u = U[chroma_row + x / 2], v = V[chroma_row + x / 2];
            uint32_t r = channel(luma + int16_t(c[1]) * v + red);
            uint32_t g = channel(luma + green - int16_t(c[2]) * v - int16_t(c[3]) * u);
            uint32_t b = channel(luma + int16_t(c[4]) * u + blue);
            uint32_t value;
            switch (s.output) {
            case 0: value = (s.alpha & 0xFF) | b << 8 | g << 16 | r << 24; break; // alpha, then blue, green, red
            case 1: value = b | g << 8 | r << 16; break;
            case 2: value = (s.alpha >= 0x80) | (b >> 3) << 1 | (g >> 3) << 6 | (r >> 3) << 11; break;
            default: value = (b >> 3) | (g >> 2) << 5 | (r >> 3) << 11; break;
            }
            // in rows, or in 8x8 tiles as textures are
            size_t at = s.tiled ? (size_t(y / 8) * width * 8 + (x / 8) * 64 + morton(x % 8, y % 8)) : size_t(y) * width + x;
            std::memcpy(&out[at * bytes], &value, bytes);
        }
    }
    uint32_t transfer = std::max(s.out.transfer, 1u), address = s.out.address;
    // the movie's frames go where textures were read from: the GPU drops them
    gpu_sync(system, address, uint32_t((out.size() + transfer - 1) / transfer * (transfer + s.out.gap)));
    for (size_t i = 0; i < out.size(); i += transfer) {
        uint32_t n = uint32_t(std::min<size_t>(transfer, out.size() - i));
        m.write_bytes(address, &out[i], n);
        address += transfer + s.out.gap;
    }
}

bool Services::y2r(System &system, CommandBuffer &b) {
    Kernel &k = system.kernel;
    Y2r &s = y2r_;
    uint32_t command = b.command(), w = b.get(1);
    if (!s.event) {
        s.event = k.create_event(ResetType::OneShot).first;
        k.hold(*s.event);
    }
    auto coefficients = [&](const int32_t *c) {
        b.reply(command, {uint32_t(uint16_t(c[0])) | uint32_t(c[1]) << 16, uint32_t(uint16_t(c[2])) | uint32_t(c[3]) << 16,
                          uint32_t(uint16_t(c[4])) | uint32_t(c[5]) << 16, uint32_t(uint16_t(c[6])) | uint32_t(c[7]) << 16});
    };
    switch (command) {
    case 0x0001: s.input = w & 0xFF; break;
    case 0x0002: b.reply(command, {s.input}); return true;
    case 0x0003: s.output = w & 0xFF; break;
    case 0x0004: b.reply(command, {s.output}); return true;
    case 0x0005: s.rotation = w & 0xFF; break;
    case 0x0006: b.reply(command, {s.rotation}); return true;
    case 0x0007: s.tiled = (w & 0xFF) != 0; break;
    case 0x0008: b.reply(command, {s.tiled}); return true;
    case 0x000F: // GetTransferEndEvent
        b.set(0, header(command, 1, 2));
        b.set(1, result::SUCCESS);
        b.set(2, descriptor::handles(1));
        b.set(3, k.create_handle(*s.event));
        return true;
    case 0x0010: case 0x0011: case 0x0012: case 0x0013: case 0x0018: { // SetSending Y/U/V/YUYV, SetReceiving(address, size, transfer, gap)
        Y2rBuffer &buf = command == 0x0010 ? s.y : command == 0x0011 ? s.u : command == 0x0012 ? s.v : command == 0x0013 ? s.yuyv : s.out;
        buf = Y2rBuffer{w, b.get(3) & 0xFFFF, b.get(4) & 0xFFFF};
        break;
    }
    case 0x0014: case 0x0015: case 0x0016: case 0x0017: case 0x0019: b.reply(command, {1}); return true; // done at once
    case 0x001A: s.width = w & 0xFFFF; break;
    case 0x001B: b.reply(command, {s.width}); return true;
    case 0x001C: s.lines = w & 0xFFFF; break;
    case 0x001D: b.reply(command, {s.lines}); return true;
    case 0x001E: // SetCoefficient: eight halfwords
        for (int i = 0; i < 8; i++) s.coefficients[i] = int16_t(b.get(1 + i / 2) >> (16 * (i % 2)));
        break;
    case 0x001F: coefficients(s.coefficients); return true;
    case 0x0020: s.standard = w & 3; std::memcpy(s.coefficients, Y2R_STANDARD[s.standard], sizeof s.coefficients); break;
    case 0x0021: coefficients(Y2R_STANDARD[w & 3]); return true;
    case 0x0022: s.alpha = w & 0xFFFF; break;
    case 0x0023: b.reply(command, {s.alpha}); return true;
    case 0x0025: b.reply(command, {0, 0, 0, 0, 0, 0, 0, 0}); return true;
    case 0x0026: // StartConversion: done at once
        y2r_convert(system);
        k.signal_event(*s.event);
        break;
    case 0x0028: case 0x002A: b.reply(command, {0}); return true; // not busy; PingProcess
    case 0x0029: { // SetPackageParameter(formats…, size, coefficient and alpha)
        uint32_t second = b.get(2), third = b.get(3);
        s.input = w & 0xFF, s.output = (w >> 8) & 0xFF, s.rotation = (w >> 16) & 0xFF, s.tiled = (w >> 24) & 0xFF;
        s.width = second & 0xFFFF, s.lines = second >> 16;
        s.standard = third & 3;
        std::memcpy(s.coefficients, Y2R_STANDARD[s.standard], sizeof s.coefficients);
        s.alpha = third >> 16;
        break;
    }
    case 0x002B: case 0x002C: { // DriverInitialize, DriverFinalize
        auto event = s.event;
        s = Y2r{};
        s.event = event;
        break;
    }
    case 0x002D:
        b.reply(command, {s.input | s.output << 8 | s.rotation << 16 | uint32_t(s.tiled) << 24, s.width | s.lines << 16,
                          s.standard | s.alpha << 16});
        return true;
    default:
        if (command < 0x0001 || command > 0x002D) return false;
        if (command % 2 == 0 && s.other.count(command - 1)) {
            b.reply(command, {s.other[command - 1]}); // a getter of what only reads back
            return true;
        }
        s.other[command] = w; // dithering and the rest
        break;
    }
    b.reply(command);
    return true;
}

} // namespace runtime
