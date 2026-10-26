// A back end that checks the vertex stage without a GPU (sketch.cpp).
#ifndef RUNTIME_PICA_SKETCH_H
#define RUNTIME_PICA_SKETCH_H

#include "runtime/pica.h"

#include <map>
#include <memory>
#include <string>

namespace runtime::pica {

class Sketch : public Backend {
public:
    Sketch();
    ~Sketch() override;
    void draw(Pica &pica, bool indexed) override;
    void draw_immediate(Pica &pica, const std::vector<std::array<Vec4, 16>> &vertices) override;
    void sync(uint32_t paddr, uint32_t size, bool write) override;

    // each canvas with something on it, as <prefix>-<address>.ppm
    void save(const std::string &prefix) const;
    uint64_t shaded() const;
    uint64_t skipped = 0; // draws the vertex stage could not do

    struct Canvas {
        uint32_t width = 0, height = 0;
        std::vector<uint8_t> rgb;
        uint64_t triangles = 0, draws = 0;
    };
    std::map<uint32_t, Canvas> canvases; // by color buffer address

private:
    void fill(Pica &pica);
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace runtime::pica

#endif
