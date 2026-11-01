// the frames between's jump test on draws shaded on the GPU (between.cpp,
// jumps): a draw that moves more than a quarter of the screen from one
// frame to the next (a camera cut) is drawn where it is now; one that moves
// a little is drawn between. no GPU needed: the jump is told on the CPU
#include <cstdio>
#include <memory>

#include "../src/vk/impl.h"

using namespace runtime;
using Impl = VulkanRenderer::Impl;

// a triangle near the middle, through "add o0, c0, v0; end": moved right
// by x, the uniform c0
static Impl::Op triangle_at(float x) {
    auto program = std::make_shared<ProgramBlock>();
    program->code[0] = 0x20u << 12;   // add o0, c0, v0 (descriptor 0)
    program->code[1] = 0x22u << 26;   // end
    program->desc[0] = 0xF | 0x1B << 5 | 0x1B << 14; // xyzw, sources as they are
    auto d = std::make_shared<GpuDraw>();
    d->stride = 1;
    d->inputs = {{-0.1f, -0.1f, 0.5f, 1}, {0.1f, -0.1f, 0.5f, 1}, {0, 0.1f, 0.5f, 1}};
    d->indices = {0, 1, 2};
    d->program = program;
    d->program_hash = 1;
    d->unit.f[0][0] = x;
    Impl::Op op{};
    op.kind = Impl::Op::Draw;
    op.gpu = d;
    return op;
}

int main() {
    int failed = 0;
    auto check = [&](const char *what, bool got, bool want) {
        std::printf("%s: %s\n", what, got == want ? "ok" : "WRONG");
        failed += got != want;
    };
    Impl::Op still = triangle_at(0), small = triangle_at(0.1f), cut = triangle_at(0.9f);
    check("the same place does not jump", Impl::jumps(still, triangle_at(0)), false);
    check("a small move does not jump", Impl::jumps(still, small), false);
    check("a cut jumps", Impl::jumps(still, cut), true);
    check("a cut back jumps", Impl::jumps(cut, still), true);
    auto positions = Impl::sample_positions(*cut.gpu);
    check("the program runs on the CPU", positions.size() == 3 && positions[1].position[0] > 0.99f && positions[1].position[0] < 1.01f, true);
    return failed ? 1 : 0;
}
