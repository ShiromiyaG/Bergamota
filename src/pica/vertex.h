// The PICA's vertex stage on the CPU: a draw's attributes loaded, shaded,
// and assembled into triangles for the rasterizer (vertex.cpp).
#ifndef RUNTIME_PICA_VERTEX_H
#define RUNTIME_PICA_VERTEX_H

#include "shader.h"

#include <memory>
#include <vector>

namespace runtime::pica {

// what the rasterizer takes of a shaded vertex, by the output map's
// semantics
struct Vertex {
    Vec4 position;       // clip space
    Vec4 quaternion;     // the surface's orientation, for fragment lighting
    Vec4 color;
    float uv0[2], uv1[2], uv2[2];
    float w0;            // texture 0's w, for cube and projected maps
    float view[3];       // to the viewer, for fragment lighting
};

class VertexStage {
public:
    VertexStage();
    // a draw's triangles, three vertices each, from the arrays (or the
    // index buffer); false for a draw it cannot do (the geometry stage)
    bool draw(Pica &pica, bool indexed, std::vector<Vertex> &triangles);
    bool draw_immediate(Pica &pica, const std::vector<std::array<Vec4, 16>> &attributes, std::vector<Vertex> &triangles);

    uint64_t shaded = 0; // vertices through the shader

private:
    void shade(Pica &pica, std::array<Vec4, 16> &inputs, Vertex &out);
    void assemble(Pica &pica, const std::vector<Vertex> &vertices, const std::vector<uint32_t> *order, size_t count,
                  std::vector<Vertex> &triangles);

    std::unique_ptr<Program> program_;
    Runner runner_;
    std::vector<Vertex> shaded_;
    std::vector<uint32_t> indices_, order_;
};

} // namespace runtime::pica

#endif
