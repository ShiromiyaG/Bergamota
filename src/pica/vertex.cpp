// The vertex stage. After Zakuro's zakuro-gpu raster.rs (Rust, MIT,
// Copyright (c) 2026 fearkov; see LICENSE-zakuro), written anew.

#include "vertex.h"
#include "runtime/memory.h"

#include <cstring>
#include <execution>
#include <numeric>

namespace runtime::pica {

VertexStage::VertexStage() : program_(std::make_unique<Program>()) {}

namespace {

// an attribute's component types: byte, unsigned byte, short, float
constexpr uint32_t SIZE[4] = {1, 1, 2, 4};

struct Field {
    uint32_t offset, type, count, reg; // reg: the input register it lands in
};
struct Array {
    uint32_t offset, stride, size;
    std::vector<Field> fields;
};

// how a draw's vertices are laid out: the arrays and their fields, and
// what the registers no array feeds hold
struct Layout {
    std::vector<Array> arrays;
    std::array<Vec4, 16> base{};
};

Layout layout(const Pica &p) {
    Layout l;
    const auto &r = p.regs;
    uint64_t formats = r[ATTRIBUTE_FORMAT_LOW] | uint64_t(r[ATTRIBUTE_FORMAT_HIGH]) << 32;
    uint32_t fixed = (r[ATTRIBUTE_FORMAT_HIGH] >> 16) & 0xFFF;
    // the input register each attribute goes to
    uint64_t map = r[VS_BLOCK + INPUT_MAP_LOW] | uint64_t(r[VS_BLOCK + INPUT_MAP_HIGH]) << 32;
    uint32_t count = std::min((r[VS_BLOCK + INPUT_CONFIG] & 0xF) + 1, 12u);
    int owner[16];
    std::fill(std::begin(owner), std::end(owner), -1);
    for (uint32_t id = 0; id < count; id++) owner[(map >> (id * 4)) & 0xF] = int(id);
    // which array field gives each attribute (the last, when several do)
    struct At {
        int array = -1;
        Field field{};
    } given[12];
    for (uint32_t a = 0; a < 12; a++) {
        uint32_t base = ATTRIBUTE_LOADER + a * 3, w1 = r[base + 1], w2 = r[base + 2];
        uint64_t components = w1 | uint64_t(w2 & 0xFFFF) << 32;
        uint32_t n = std::min(w2 >> 28, 12u);
        if (!n) continue;
        Array array{r[base] & 0x0FFFFFFF, (w2 >> 16) & 0xFF, 0, {}};
        uint32_t offset = 0;
        for (uint32_t s = 0; s < n; s++) {
            uint32_t id = (components >> (s * 4)) & 0xF;
            if (id >= 12) { // padding: to a word, then 4 to 16 bytes
                offset = (offset + 3) / 4 * 4 + (id - 11) * 4;
                continue;
            }
            uint32_t nibble = (formats >> (id * 4)) & 0xF, type = nibble & 3, size = SIZE[type];
            offset = (offset + size - 1) / size * size;
            given[id] = {int(l.arrays.size()), {offset, type, (nibble >> 2) + 1, 0}};
            offset += size * ((nibble >> 2) + 1);
        }
        array.size = offset;
        l.arrays.push_back(std::move(array));
    }
    for (uint32_t reg = 0; reg < 16; reg++) {
        int id = owner[reg];
        if (id < 0) continue;
        if (given[id].array >= 0) {
            Field f = given[id].field;
            f.reg = reg;
            l.arrays[given[id].array].fields.push_back(f);
        } else {
            l.base[reg] = fixed >> id & 1 ? p.fixed_attributes[id] : Vec4{0, 0, 0, 1};
        }
    }
    std::erase_if(l.arrays, [](const Array &a) { return a.fields.empty(); });
    return l;
}

// a vertex's input registers, from the arrays
void fetch(Memory &m, uint32_t base, const Layout &l, uint32_t index, std::array<Vec4, 16> &in) {
    in = l.base;
    for (const Array &a : l.arrays) {
        // the base is physical, an array's offset can carry it into another
        // region (games put the base at VRAM and reach FCRAM)
        const uint8_t *bytes = m.host(base + a.offset + index * a.stride, a.size);
        if (!bytes) continue;
        for (const Field &f : a.fields) {
            Vec4 v{0, 0, 0, 1};
            const uint8_t *at = bytes + f.offset;
            for (uint32_t c = 0; c < f.count; c++) {
                switch (f.type) {
                case 0: v[c] = float(int8_t(at[c])); break;
                case 1: v[c] = float(at[c]); break;
                case 2: { int16_t s; std::memcpy(&s, at + c * 2, 2); v[c] = s; break; }
                default: std::memcpy(&v[c], at + c * 4, 4); break;
                }
            }
            in[f.reg] = v;
        }
    }
}

} // namespace

// one vertex through the shader, and its outputs to the rasterizer's
// values: the enabled output registers in order, each component named by
// the output map
static void shade(const Pica &p, const Program &program, Runner &runner, const std::array<Vec4, 16> &inputs, Vertex &v) {
    runner.input = inputs;
    runner.run(p.vertex_shader, program);
    std::array<Vec4, 16> packed{};
    uint32_t mask = p.regs[VS_BLOCK + OUTPUT_MASK] & 0xFFFF;
    if (!mask) packed = runner.output;
    for (uint32_t reg = 0, slot = 0; reg < 16 && mask; reg++) {
        if (mask >> reg & 1) packed[slot++] = runner.output[reg];
    }
    float values[24] = {};
    values[3] = 1; // w, when nothing says
    uint32_t total = std::min(p.regs[OUTPUT_TOTAL] & 7, 7u);
    for (uint32_t slot = 0; slot < total; slot++) {
        uint32_t word = p.regs[OUTPUT_MAP + slot];
        for (uint32_t c = 0; c < 4; c++) {
            uint32_t semantic = (word >> (c * 8)) & 0x1F;
            if (semantic < 24) values[semantic] = packed[slot][c];
        }
    }
    if (!total) { // nothing described: position in o0, color in o2, texture in o3
        for (int c = 0; c < 4; c++) values[c] = packed[0][c], values[8 + c] = packed[2][c];
        values[12] = packed[3][0], values[13] = packed[3][1];
    }
    v.position = {values[0], values[1], values[2], values[3]};
    v.quaternion = {values[4], values[5], values[6], values[7]};
    v.color = {values[8], values[9], values[10], values[11]};
    v.uv0[0] = values[12], v.uv0[1] = values[13];
    v.uv1[0] = values[14], v.uv1[1] = values[15];
    v.w0 = values[16];
    v.view[0] = values[18], v.view[1] = values[19], v.view[2] = values[20];
    v.uv2[0] = values[22], v.uv2[1] = values[23];
}

void VertexStage::shade_all(Pica &p) {
    size_t n = inputs_.size();
    shaded_.resize(n);
    shaded += n;
    constexpr size_t CHUNK = 128;
    if (n < CHUNK * 2) {
        for (size_t i = 0; i < n; i++) shade(p, *program_, runner_, inputs_[i], shaded_[i]);
        return;
    }
    // a runner each chunk: they keep their own registers
    std::vector<size_t> chunks((n + CHUNK - 1) / CHUNK);
    std::iota(chunks.begin(), chunks.end(), size_t(0));
    std::for_each(std::execution::par, chunks.begin(), chunks.end(), [&](size_t c) {
        Runner runner;
        for (size_t i = c * CHUNK; i < std::min(n, (c + 1) * CHUNK); i++) shade(p, *program_, runner, inputs_[i], shaded_[i]);
    });
}

// triangles from the vertices, in the topology the registers say: a list,
// a strip (every other one turned round), a fan
void VertexStage::assemble(Pica &p, const std::vector<Vertex> &vertices, const std::vector<uint32_t> *order, size_t count,
                           std::vector<Vertex> &triangles) {
    auto at = [&](size_t i) -> const Vertex & { return vertices[order ? (*order)[i] : i]; };
    triangles.clear();
    switch ((p.regs[PRIMITIVE_CONFIG] >> 8) & 3) {
    case 1:
        for (size_t i = 2; i < count; i++) {
            bool even = i % 2 == 0;
            triangles.push_back(at(even ? i - 2 : i - 1));
            triangles.push_back(at(even ? i - 1 : i - 2));
            triangles.push_back(at(i));
        }
        break;
    case 2:
        for (size_t i = 2; i < count; i++) {
            triangles.push_back(at(0));
            triangles.push_back(at(i - 1));
            triangles.push_back(at(i));
        }
        break;
    default:
        for (size_t i = 0; i + 2 < count; i += 3) {
            triangles.push_back(at(i));
            triangles.push_back(at(i + 1));
            triangles.push_back(at(i + 2));
        }
    }
}

bool VertexStage::draw(Pica &p, bool indexed, std::vector<Vertex> &triangles) {
    triangles.clear();
    if ((p.regs[GEOSTAGE_CONFIG] & 3) == 2) return false; // a geometry shader: not yet
    uint32_t count = p.regs[VERTEX_COUNT], first = p.regs[VERTEX_OFFSET];
    if (!count || count > 0x10000) return true;
    if (program_->version != p.vertex_shader.version) program_->decode(p.vertex_shader);
    Layout l = layout(p);
    uint32_t base = (p.regs[ATTRIBUTE_BASE] & 0x0FFFFFFF) << 3;
    inputs_.clear();
    if (indexed) {
        // each vertex the index buffer names, fetched and shaded once: a
        // stamp per index says where it went this draw
        uint32_t config = p.regs[INDEX_ARRAY];
        bool shorts = config >> 31;
        const uint8_t *bytes = p.memory.host(base + (config & 0x0FFFFFFF), count * (shorts ? 2 : 1));
        if (!bytes) return true;
        if (seen_.size() < 0x10000 * 2) seen_.assign(0x10000 * 2, 0);
        if (++stamp_ == 0) std::fill(seen_.begin(), seen_.end(), 0), stamp_ = 1;
        order_.resize(count);
        for (uint32_t i = 0; i < count; i++) {
            uint32_t index = shorts ? uint32_t(bytes[i * 2] | bytes[i * 2 + 1] << 8) : bytes[i];
            if (seen_[index * 2] != stamp_) {
                seen_[index * 2] = stamp_;
                seen_[index * 2 + 1] = uint32_t(inputs_.size());
                fetch(p.memory, base, l, index, inputs_.emplace_back());
            }
            order_[i] = seen_[index * 2 + 1];
        }
        shade_all(p);
        assemble(p, shaded_, &order_, count, triangles);
    } else {
        inputs_.resize(count);
        for (uint32_t i = 0; i < count; i++) fetch(p.memory, base, l, first + i, inputs_[i]);
        shade_all(p);
        assemble(p, shaded_, nullptr, count, triangles);
    }
    return true;
}

bool VertexStage::draw_immediate(Pica &p, const std::vector<std::array<Vec4, 16>> &attributes, std::vector<Vertex> &triangles) {
    triangles.clear();
    if ((p.regs[GEOSTAGE_CONFIG] & 3) == 2) return false;
    if (program_->version != p.vertex_shader.version) program_->decode(p.vertex_shader);
    uint64_t map = p.regs[VS_BLOCK + INPUT_MAP_LOW] | uint64_t(p.regs[VS_BLOCK + INPUT_MAP_HIGH]) << 32;
    uint32_t count = (p.regs[VS_ATTRIBUTE_COUNT] & 0xF) + 1;
    inputs_.assign(attributes.size(), {});
    for (size_t v = 0; v < attributes.size(); v++) {
        for (uint32_t id = 0; id < count && id < 16; id++) inputs_[v][(map >> (id * 4)) & 0xF] = attributes[v][id];
    }
    shade_all(p);
    assemble(p, shaded_, nullptr, shaded_.size(), triangles);
    return true;
}

} // namespace runtime::pica
