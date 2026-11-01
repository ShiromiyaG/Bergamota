// The PICA200, the 3DS's GPU, as the runtime's own renderer sees it: the
// front end that runs command lists into registers, shader programs and
// tables, and the engines that fill, transfer and copy memory. Draws go to
// a back end (docs/vulkan-renderer.md). Zakuro's zakuro-gpu (Rust, MIT,
// Copyright (c) 2026 fearkov; see LICENSE-zakuro) is the reference for how
// the hardware behaves.
#ifndef RUNTIME_PICA_H
#define RUNTIME_PICA_H

#include "runtime/gpu.h"

#include <array>
#include <cstdint>
#include <deque>
#include <mutex>
#include <vector>

namespace runtime::pica {

// internal registers, by word
enum Reg : uint32_t {
    FACE_CULLING = 0x040,
    VIEWPORT_WIDTH = 0x041,
    VIEWPORT_HEIGHT = 0x043,
    DEPTH_RANGE = 0x04D,
    DEPTH_NEAR = 0x04E,
    OUTPUT_TOTAL = 0x04F,
    OUTPUT_MAP = 0x050, // to 0x056
    VIEWPORT_XY = 0x068,
    DEPTHMAP_ENABLE = 0x06D,
    TEXTURE_CONFIG = 0x080,
    PROCTEX_TABLE_INDEX = 0x0AF,
    PROCTEX_TABLE_DATA = 0x0B0, // to 0x0B7
    TEV_STAGE0 = 0x0C0,
    FOG_TABLE_INDEX = 0x0E6,
    FOG_TABLE_DATA = 0x0E8, // to 0x0EF
    COLOR_OPERATION = 0x100,
    BLEND_FUNC = 0x101,
    LOGIC_OP = 0x102,
    BLEND_COLOR = 0x103,
    ALPHA_TEST = 0x104,
    STENCIL_TEST = 0x105,
    STENCIL_OP = 0x106,
    DEPTH_COLOR_MASK = 0x107,
    COLOR_BUFFER_WRITE = 0x113,
    DEPTH_STENCIL_WRITE = 0x115,
    DEPTH_BUFFER_FORMAT = 0x116,
    COLOR_BUFFER_FORMAT = 0x117,
    DEPTH_BUFFER_ADDRESS = 0x11C,
    COLOR_BUFFER_ADDRESS = 0x11D,
    FRAMEBUFFER_DIMENSIONS = 0x11E,
    LIGHT_TABLE_INDEX = 0x1C5,
    LIGHT_TABLE_DATA = 0x1C8, // to 0x1CF
    ATTRIBUTE_BASE = 0x200,
    ATTRIBUTE_FORMAT_LOW = 0x201,
    ATTRIBUTE_FORMAT_HIGH = 0x202,
    ATTRIBUTE_LOADER = 0x203, // 12 of 3 words
    INDEX_ARRAY = 0x227,
    VERTEX_COUNT = 0x228,
    GEOSTAGE_CONFIG = 0x229,
    VERTEX_OFFSET = 0x22A,
    DRAW_ARRAYS = 0x22E,
    DRAW_ELEMENTS = 0x22F,
    FIXED_ATTRIBUTE_INDEX = 0x232,
    FIXED_ATTRIBUTE_DATA = 0x233, // to 0x235
    CMDBUF_SIZE0 = 0x238,
    CMDBUF_ADDR0 = 0x23A,
    CMDBUF_JUMP0 = 0x23C,
    CMDBUF_JUMP1 = 0x23D,
    VS_ATTRIBUTE_COUNT = 0x242,
    VS_COM_MODE = 0x244,
    VS_OUTPUT_TOTAL = 0x24A, // the vertex shader's outputs each vertex, less one, for the geometry shader
    GS_CONFIG = 0x252,
    PRIMITIVE_CONFIG = 0x25E,
    GS_BLOCK = 0x280,
    VS_BLOCK = 0x2B0,
    REGISTERS = 0x300,
};

// a shader unit's registers, from its block's start
constexpr uint32_t BOOL_UNIFORMS = 0x00;
constexpr uint32_t INT_UNIFORMS = 0x01; // to 0x04
constexpr uint32_t INPUT_CONFIG = 0x09;
constexpr uint32_t ENTRY_POINT = 0x0A;
constexpr uint32_t INPUT_MAP_LOW = 0x0B;
constexpr uint32_t INPUT_MAP_HIGH = 0x0C;
constexpr uint32_t OUTPUT_MASK = 0x0D;
constexpr uint32_t UNIFORM_INDEX = 0x10;
constexpr uint32_t UNIFORM_DATA = 0x11; // to 0x18
constexpr uint32_t PROGRAM_INDEX = 0x1B;
constexpr uint32_t PROGRAM_DATA = 0x1C; // to 0x23
constexpr uint32_t DESCRIPTOR_INDEX = 0x25;
constexpr uint32_t DESCRIPTOR_DATA = 0x26; // to 0x2D
constexpr uint32_t SHADER_BLOCK = 0x2E;

using Vec4 = std::array<float, 4>; // x, y, z, w

// a 24-bit float (1.7.16) as a float
float f24(uint32_t raw);

// what a shader unit holds: its program, descriptors and uniforms
struct ShaderUnit {
    std::array<uint32_t, 4096> program{};
    std::array<uint32_t, 128> descriptors{};
    std::array<Vec4, 96> floats{};
    std::array<std::array<uint8_t, 4>, 4> ints{};
    uint16_t bools = 0;
    uint32_t entry = 0;
    // a counter that moves whenever the program or descriptors change, for
    // caches of what was made of them
    uint64_t version = 0;

    void configure(uint32_t offset, uint32_t value);

private:
    uint32_t program_at_ = 0, descriptor_at_ = 0, float_at_ = 0, staged_count_ = 0;
    bool wide_ = false;
    uint32_t staged_[4]{};
};

class Pica;

// what draws: the Vulkan renderer, or nothing (counted only)
struct Backend {
    virtual ~Backend() = default;
    // the registers hold a draw: arrays, or indexed
    virtual void draw(Pica &pica, bool indexed) = 0;
    // immediate-mode vertices, their attributes as the shader takes them
    virtual void draw_immediate(Pica &pica, const std::vector<std::array<Vec4, 16>> &vertices) = 0;
    // memory over a range is about to be read or written by something
    // else: what the back end keeps of it goes back first (or is dropped)
    virtual void sync(uint32_t paddr, uint32_t size, bool write) { (void)paddr, (void)size, (void)write; }
    // a fill of memory (done on memory as well afterwards): buffers it
    // covers whole are cleared where they are
    virtual void fill(uint32_t start, uint32_t end, uint32_t value, uint32_t width) { sync(start, end - start, true); }
    // a display transfer done where the input is, the GPU's own buffer;
    // false leaves it to memory. Sizes in output pixels, scale 1 or 2 each way
    struct Transfer {
        uint32_t input, output, in_width, in_height, out_width, out_height, copy_width, copy_height, scale_x, scale_y;
        uint32_t out_format;
        bool flip, out_tiled;
    };
    virtual bool display_transfer(const Transfer &) { return false; }
    // a vertical blank: the back end takes what the screens show and hands
    // it to pica.push_frame (a blank later, to not wait for the GPU); false
    // leaves it to the front end, which reads memory
    virtual bool vblank(Pica &) { return false; }
};

class Pica : public Gpu {
public:
    explicit Pica(Memory &memory);
    Backend *backend = nullptr;

    void write_register(uint32_t offset, uint32_t value) override;
    uint32_t read_register(uint32_t offset) override;
    void command_list(uint32_t paddr, uint32_t size) override;
    void memory_fill(uint32_t start, uint32_t end, uint32_t value, uint32_t width) override;
    void display_transfer(uint32_t input, uint32_t output, uint32_t input_size, uint32_t output_size,
                          uint32_t flags) override;
    void texture_copy(uint32_t input, uint32_t output, uint32_t size, uint32_t input_gap,
                      uint32_t output_gap) override;
    void framebuffer(uint32_t screen, uint32_t index, uint32_t left, uint32_t right, uint32_t stride,
                     uint32_t format) override;
    void sync(uint32_t paddr, uint32_t size) override;
    void vblank() override;
    bool next_frame(Frame &out) override;
    // a frame for whoever shows them, from the back end's thread
    void push_frame(Frame frame);

    // what a screen shows now, read from memory
    Picture screen(uint32_t screen);

    Memory &memory;
    std::array<uint32_t, REGISTERS> regs{};
    ShaderUnit vertex_shader, geometry_shader;
    std::array<Vec4, 16> fixed_attributes{};
    // the tables, as written: fragment lighting (24 of 256), procedural
    // texture (by its table select, 8 of 256), fog (128)
    std::array<std::array<uint32_t, 256>, 32> light_tables{};
    std::array<std::array<uint32_t, 256>, 8> proctex_tables{};
    std::array<uint32_t, 128> fog_table{};
    // bumped whenever a table changes
    uint64_t tables_version = 0;
    // the LCDs: per screen, the buffer it shows, its stride and format
    struct Screen {
        uint32_t address = 0, stride = 0, format = 0;
    } screens[2];

    // counters
    uint64_t lists = 0, draws = 0, fills = 0, transfers = 0, vblanks = 0;
    double busy = 0; // seconds in command lists and transfers

private:
    std::mutex frames_lock_;
    std::deque<Frame> frames_; // shown next, a few at most
    void run(uint32_t paddr, uint32_t size);
    void write(uint32_t reg, uint32_t value, uint32_t mask);
    void flush_immediate();

    std::array<uint32_t, 0x1000> external_{};
    uint32_t fixed_words_[3]{};
    uint32_t fixed_count_ = 0;
    std::array<Vec4, 16> immediate_{};
    uint32_t immediate_next_ = 0;
    std::vector<std::array<Vec4, 16>> immediate_vertices_;
    bool jump_ = false;
    uint32_t jump_address_ = 0, jump_size_ = 0;
};

// the five color formats a buffer can have, as RGBA8 and back
uint32_t bytes_per_pixel(uint32_t format);
std::array<uint8_t, 4> decode_color(uint32_t format, const uint8_t *in);
void encode_color(uint32_t format, std::array<uint8_t, 4> rgba, uint8_t *out);
// where a pixel of a tiled buffer lies: 8x8 tiles, in Z order inside
uint32_t tiled_offset(uint32_t x, uint32_t y, uint32_t width, uint32_t bytes_per_pixel);

} // namespace runtime::pica

#endif
