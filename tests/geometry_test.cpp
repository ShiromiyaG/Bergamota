// The geometry shader without a GPU: a point goes in, the geometry shader
// emits a triangle from it, the way particle systems expand their points
// (after Zakuro's lib.rs test a_geometry_shader_expands_a_point).

#include "../src/pica/vertex.h"
#include "runtime/memory.h"

#include <cstdio>

using namespace runtime;
using namespace runtime::pica;

namespace {

int failures = 0;

#define CHECK(cond)                                                                                                    \
    do {                                                                                                               \
        if (!(cond)) {                                                                                                 \
            std::printf("%s:%d: %s\n", __FILE__, __LINE__, #cond);                                                     \
            failures++;                                                                                                \
        }                                                                                                              \
    } while (0)

constexpr uint32_t mov(uint32_t dest, uint32_t source) { return (0x13u << 26) | (dest << 21) | (source << 12); }
constexpr uint32_t setemit(uint32_t slot, bool primitive, bool winding = false) {
    return (0x2Bu << 26) | (slot << 24) | (uint32_t(primitive) << 23) | (uint32_t(winding) << 22);
}
constexpr uint32_t EMIT = 0x2Au << 26, END = 0x22u << 26;
// all four components written, sources taken as they are
constexpr uint32_t PLAIN = 0xF | (0b00011011u << 5) | (0b00011011u << 14);

// both units: o0 and o1 out, positions in o0 and colors in o1 by the map
void setup(Pica &p) {
    uint32_t vs[] = {mov(0, 0), mov(1, 1), END};
    std::copy(std::begin(vs), std::end(vs), p.vertex_shader.program.begin());
    p.vertex_shader.descriptors[0] = PLAIN;
    p.vertex_shader.version++;
    p.regs[VS_ATTRIBUTE_COUNT] = 1; // two attributes
    p.regs[VS_BLOCK + INPUT_MAP_LOW] = 0x10;
    p.regs[VS_BLOCK + OUTPUT_MASK] = 3;
    p.regs[VS_OUTPUT_TOTAL] = 1; // two outputs a vertex
    p.regs[GEOSTAGE_CONFIG] = 2;
    p.regs[GS_BLOCK + INPUT_CONFIG] = 1; // one vertex a run
    p.regs[GS_BLOCK + INPUT_MAP_LOW] = 0x10;
    p.regs[GS_BLOCK + OUTPUT_MASK] = 3;
    p.regs[OUTPUT_TOTAL] = 2;
    p.regs[OUTPUT_MAP] = 0x03020100;     // x y z w
    p.regs[OUTPUT_MAP + 1] = 0x0B0A0908; // r g b a
    p.geometry_shader.descriptors[0] = PLAIN;
    p.geometry_shader.floats[0] = {-1, -1, 0, 1};
    p.geometry_shader.floats[1] = {3, -1, 0, 1};
    p.geometry_shader.floats[2] = {-1, 3, 0, 1};
}

void expands_a_point(bool winding) {
    Memory memory(false, 64 << 20); // an old 3DS's application region
    Pica p(memory);
    setup(p);
    uint32_t gs[] = {setemit(0, false), mov(0, 0x20), mov(1, 1), EMIT, setemit(1, false), mov(0, 0x21), EMIT,
                     setemit(2, true, winding), mov(0, 0x22), EMIT, END};
    std::copy(std::begin(gs), std::end(gs), p.geometry_shader.program.begin());
    p.geometry_shader.version++;

    VertexStage stage;
    std::vector<Vertex> triangles;
    Vec4 green{0, 1, 0, 1};
    std::vector<std::array<Vec4, 16>> point(1);
    point[0][0] = {0, 0, 0, 1};
    point[0][1] = green;
    CHECK(stage.draw_immediate(p, point, triangles));
    CHECK(triangles.size() == 3);
    if (triangles.size() != 3) return;
    // turned round, the first two trade places
    float first = winding ? 3 : -1, second = winding ? -1 : 3;
    CHECK(triangles[0].position[0] == first && triangles[1].position[0] == second);
    CHECK(triangles[2].position[0] == -1 && triangles[2].position[1] == 3);
    for (const Vertex &v : triangles) CHECK(v.color == green);
}

// a mode other than 0 is refused, and the draw counted as such
void other_modes_refused() {
    Memory memory(false, 64 << 20); // an old 3DS's application region
    Pica p(memory);
    setup(p);
    p.regs[GS_CONFIG] = 1;
    VertexStage stage;
    std::vector<Vertex> triangles;
    std::vector<std::array<Vec4, 16>> point(1);
    CHECK(!stage.draw_immediate(p, point, triangles));
    CHECK(triangles.empty());
}

} // namespace

int main() {
    expands_a_point(false);
    expands_a_point(true);
    other_modes_refused();
    if (failures) {
        std::printf("%d failed\n", failures);
        return 1;
    }
    std::printf("geometry: all passed\n");
    return 0;
}
