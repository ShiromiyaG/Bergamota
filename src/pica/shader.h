// The PICA's shader units on the CPU (shader.cpp).
#ifndef RUNTIME_PICA_SHADER_H
#define RUNTIME_PICA_SHADER_H

#include "runtime/pica.h"

#include <algorithm>
#include <array>
#include <vector>

namespace runtime::pica {

struct Operand {
    uint8_t reg = 0;      // 0x00-0x0F inputs, 0x10-0x1F temporaries, 0x20-0x7F uniforms
    uint8_t index = 0;    // the address register that offsets it: 0 none, 1 a0.x, 2 a0.y, 3 aL
    bool negate = false;
    uint8_t swizzle[4]{}; // the component each one takes
};

// an instruction with its operands and descriptor worked out
struct Decoded {
    uint32_t word = 0;
    uint8_t op = 0, dest = 0, mask = 0;
    Operand src[3];
};

// a unit's program, decoded; again when the unit's version moves
struct Program {
    std::array<Decoded, 4096> ops;
    uint64_t version = ~0ull;
    void decode(const ShaderUnit &unit);
};

// one vertex through a program; as the geometry shader, with emitted set,
// the triangles it emits: three output sets each, in order
class Runner {
public:
    std::array<Vec4, 16> input{}, output{};
    std::vector<std::array<Vec4, 16>> *emitted = nullptr;
    void run(const ShaderUnit &unit, const Program &program);

private:
    struct Block {
        uint32_t end, back, repeat;
        int32_t increment;
        uint32_t start;
        bool loop;
    };
    Vec4 read(const Operand &o) const;
    void write(uint32_t reg, uint32_t mask, const Vec4 &v);
    bool condition(uint32_t word) const;

    const ShaderUnit *unit_ = nullptr;
    std::array<Vec4, 16> temp_{};
    int32_t address_[3]{};
    bool cond_[2]{};
    std::vector<Block> blocks_;
    // the geometry shader's: the vertices set aside (SETEMIT picks the
    // next one's slot, and whether it ends a triangle, turned round or not)
    std::array<std::array<Vec4, 16>, 3> slots_{};
    uint32_t slot_ = 0;
    bool primitive_ = false, winding_ = false;
};

// the same over BATCH vertices at once (batch.cpp): registers by component
// and then by vertex. run is false when the vertices part ways at a branch
constexpr int BATCH = 16;
class BatchRunner {
public:
    float input[16][4][BATCH]{}, output[16][4][BATCH]{};
    bool run(const ShaderUnit &unit, const Program &program);

private:
    struct Block {
        uint32_t end, back, repeat;
        int32_t increment;
        uint32_t start;
        bool loop;
    };
    void read(const Operand &o, float (&r)[4][BATCH]) const;
    void write(uint32_t reg, uint32_t mask, const float (&v)[4][BATCH]);
    int condition(uint32_t word) const;

    const ShaderUnit *unit_ = nullptr;
    float temp_[16][4][BATCH]{};
    int32_t address_[3][BATCH]{};
    bool cond_[2][BATCH]{};
    std::vector<Block> blocks_;
};

} // namespace runtime::pica

#endif
