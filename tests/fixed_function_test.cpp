// The rest of the fixed function without a GPU: the fog's table as the
// registers write it (Zakuro's fog.rs tests), and the logic ops as Vulkan's.

#include "../src/pica/texture.h"
#include "runtime/pica_tables.h"
#include "runtime/vulkan_renderer.h"

#include <vulkan/vulkan.h>

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

void fog_table() {
    std::array<uint32_t, 128> raw{};
    // a half, then a step down of a quarter; almost one, a step up of one 2048th
    raw[5] = (1024u << 13) | (0x2000 - 512);
    raw[6] = 2047u << 13 | 1;
    FogTable t;
    decode_fog_table(raw, t);
    CHECK(t[5][0] == 0.5f && t[5][1] == -0.25f);
    CHECK(t[6][0] == 2047.0f / 2048.0f && t[6][1] == 1.0f / 2048.0f);
    CHECK(t[0][0] == 0.0f && t[0][1] == 0.0f);
}

// a 64x32 RGBA8 texture's levels: each after the last, halved, down to a
// tile; ETC1 (4 bits) takes an eighth as many bytes
void mipmap_levels() {
    auto levels = texture_levels(0, 64, 32, 15);
    CHECK(levels.size() == 3);
    if (levels.size() == 3) {
        CHECK(levels[1].offset == 64 * 32 * 4 && levels[1].width == 32 && levels[1].height == 16);
        CHECK(levels[2].offset == 64 * 32 * 4 + 32 * 16 * 4 && levels[2].width == 16 && levels[2].height == 8);
    }
    CHECK(texture_bytes(0, levels) == (64 * 32 + 32 * 16 + 16 * 8) * 4);
    CHECK(texture_levels(0, 64, 32, 0).size() == 1);
    auto etc = texture_levels(12, 64, 64, 1);
    CHECK(etc.size() == 2 && etc[1].offset == 64 * 64 / 2 && texture_bytes(12, etc) == (64 * 64 + 32 * 32) / 2);
}

void logic_ops() {
    // blend.rs order: clear, and, and reverse, copy, set, copy inverted,
    // noop, invert, nand, or, nor, xor, equivalent, and inverted, or
    // reverse, or inverted
    const VkLogicOp want[16] = {
        VK_LOGIC_OP_CLEAR,      VK_LOGIC_OP_AND,          VK_LOGIC_OP_AND_REVERSE, VK_LOGIC_OP_COPY,
        VK_LOGIC_OP_SET,        VK_LOGIC_OP_COPY_INVERTED, VK_LOGIC_OP_NO_OP,      VK_LOGIC_OP_INVERT,
        VK_LOGIC_OP_NAND,       VK_LOGIC_OP_OR,           VK_LOGIC_OP_NOR,         VK_LOGIC_OP_XOR,
        VK_LOGIC_OP_EQUIVALENT, VK_LOGIC_OP_AND_INVERTED,  VK_LOGIC_OP_OR_REVERSE, VK_LOGIC_OP_OR_INVERTED,
    };
    for (uint32_t op = 0; op < 16; op++) CHECK(vk_logic_op(op) == want[op]);
    CHECK(vk_logic_op(0x13) == VK_LOGIC_OP_COPY); // only the low four bits
}

} // namespace

int main() {
    fog_table();
    mipmap_levels();
    logic_ops();
    if (failures) std::printf("%d failed\n", failures);
    return failures ? 1 : 0;
}
