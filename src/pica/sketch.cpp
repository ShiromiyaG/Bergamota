// A back end for checking the vertex stage without a GPU: each draw's
// triangles filled flat (the vertices' color, averaged) on a canvas per
// color buffer, which boot saves beside its screenshots.

#include "runtime/pica_sketch.h"
#include "vertex.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <fstream>

namespace runtime::pica {

struct Sketch::Impl {
    VertexStage stage;
    std::vector<Vertex> triangles;
};

Sketch::Sketch() : impl_(std::make_unique<Impl>()) {}
Sketch::~Sketch() = default;

void Sketch::sync(uint32_t paddr, uint32_t size, bool write) {
    // a fill over a canvas starts it again
    if (!write) return;
    for (auto &[address, canvas] : canvases) {
        if (address >= paddr && address < paddr + size) {
            std::fill(canvas.rgb.begin(), canvas.rgb.end(), 0);
            canvas.triangles = 0;
            canvas.draws = 0;
        }
    }
}

void Sketch::draw(Pica &p, bool indexed) {
    if (!impl_->stage.draw(p, indexed, impl_->triangles)) skipped++;
    fill(p);
}

void Sketch::draw_immediate(Pica &p, const std::vector<std::array<Vec4, 16>> &vertices) {
    if (!impl_->stage.draw_immediate(p, vertices, impl_->triangles)) skipped++;
    fill(p);
}

void Sketch::fill(Pica &p) {
    const auto &r = p.regs;
    uint32_t address = (r[COLOR_BUFFER_ADDRESS] & 0x0FFFFFFF) << 3;
    uint32_t dims = r[FRAMEBUFFER_DIMENSIONS], width = dims & 0x7FF, height = ((dims >> 12) & 0x3FF) + 1;
    if (!address || !width || width > 1024 || height > 1024) return;
    Canvas &c = canvases[address];
    if (c.width != width || c.height != height) c = {width, height, std::vector<uint8_t>(width * height * 3), 0, 0};
    float vw = f24(r[VIEWPORT_WIDTH]) * 2, vh = f24(r[VIEWPORT_HEIGHT]) * 2;
    float vx = float(int32_t(r[VIEWPORT_XY] << 22) >> 22), vy = float(int32_t((r[VIEWPORT_XY] >> 16) << 22) >> 22);
    const auto &t = impl_->triangles;
    c.draws++;
    for (size_t i = 0; i + 2 < t.size(); i += 3) {
        float x[3], y[3];
        bool behind = false;
        for (int k = 0; k < 3; k++) {
            const Vec4 &pos = t[i + k].position;
            if (pos[3] <= 1e-6f) behind = true;
            x[k] = vx + (pos[0] / pos[3] * 0.5f + 0.5f) * vw;
            y[k] = vy + (pos[1] / pos[3] * 0.5f + 0.5f) * vh;
        }
        if (behind) continue;
        // a color per draw, to tell them apart
        uint32_t hash = uint32_t(c.draws * 2654435761u);
        uint8_t rgb[3] = {uint8_t(80 + (hash >> 8) % 176), uint8_t(80 + (hash >> 16) % 176), uint8_t(80 + (hash >> 24) % 176)};
        // a bounding box with edge tests, either winding
        int x0 = std::max(0, int(std::floor(std::min({x[0], x[1], x[2]})))), x1 = std::min(int(width) - 1, int(std::ceil(std::max({x[0], x[1], x[2]}))));
        int y0 = std::max(0, int(std::floor(std::min({y[0], y[1], y[2]})))), y1 = std::min(int(height) - 1, int(std::ceil(std::max({y[0], y[1], y[2]}))));
        float area = (x[1] - x[0]) * (y[2] - y[0]) - (x[2] - x[0]) * (y[1] - y[0]);
        if (std::abs(area) < 1e-6f) continue;
        for (int py = y0; py <= y1; py++) {
            for (int px = x0; px <= x1; px++) {
                float fx = px + 0.5f, fy = py + 0.5f;
                float e0 = (x[1] - x[0]) * (fy - y[0]) - (fx - x[0]) * (y[1] - y[0]);
                float e1 = (x[2] - x[1]) * (fy - y[1]) - (fx - x[1]) * (y[2] - y[1]);
                float e2 = (x[0] - x[2]) * (fy - y[2]) - (fx - x[2]) * (y[0] - y[2]);
                bool in = area > 0 ? e0 >= 0 && e1 >= 0 && e2 >= 0 : e0 <= 0 && e1 <= 0 && e2 <= 0;
                if (!in) continue;
                // the edges only, a pixel wide, so what is behind shows
                float l0 = std::hypot(x[1] - x[0], y[1] - y[0]), l1 = std::hypot(x[2] - x[1], y[2] - y[1]), l2 = std::hypot(x[0] - x[2], y[0] - y[2]);
                if (std::abs(e0) > l0 && std::abs(e1) > l1 && std::abs(e2) > l2) continue;
                uint8_t *d = &c.rgb[(size_t(py) * width + px) * 3];
                d[0] = rgb[0], d[1] = rgb[1], d[2] = rgb[2];
            }
        }
        c.triangles++;
    }
}

uint64_t Sketch::shaded() const { return impl_->stage.shaded; }

void Sketch::save(const std::string &prefix) const {
    for (const auto &[address, c] : canvases) {
        if (!c.triangles) continue;
        char name[512];
        std::snprintf(name, sizeof name, "%s-%08X.ppm", prefix.c_str(), address);
        std::ofstream out(name, std::ios::binary);
        out << "P6\n" << c.width << " " << c.height << "\n255\n";
        // the PICA's y goes up
        for (uint32_t y = c.height; y-- > 0;) out.write(reinterpret_cast<const char *>(&c.rgb[y * c.width * 3]), c.width * 3);
    }
}

} // namespace runtime::pica
