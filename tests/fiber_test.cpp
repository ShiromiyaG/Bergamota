// The game's threads' stacks without a game: two fibers taking turns with
// the main one, a deep call chain on a fiber's own stack, a fiber left for
// good and destroyed from another.

#include "runtime/fiber.h"

#include <cstdio>
#include <string>
#include <vector>

using namespace runtime;

namespace {

int failures = 0;

#define CHECK(cond)                                                                                                    \
    do {                                                                                                               \
        if (!(cond)) {                                                                                                 \
            std::printf("%s:%d: %s\n", __FILE__, __LINE__, #cond);                                                     \
            failures++;                                                                                                \
        }                                                                                                              \
    } while (0)

// as deep as the game's code goes before it unwinds (2048 calls), with
// frames as big as a generated function's
int deep(int n) {
    volatile char frame[512];
    frame[0] = char(n);
    return n ? deep(n - 1) + frame[0] - char(n) + 1 : 0;
}

void fibers_take_turns() {
    auto main = Fiber::main();
    std::string trace;
    std::unique_ptr<Fiber> a, b;
    a = Fiber::make(1 << 20, [&] {
        for (int i = 0; i < 3; i++) {
            trace += 'a';
            b->switch_from(*a);
        }
        trace += '.';
        main->switch_from(*a);
    });
    b = Fiber::make(1 << 20, [&] {
        for (;;) {
            trace += 'b';
            a->switch_from(*b);
        }
    });
    a->switch_from(*main);
    CHECK(trace == "ababab.");
    // a left for good (its body switched away), destroyed from here
    a.reset();
    b.reset();
}

void a_fiber_has_room_for_the_game() {
    auto main = Fiber::main();
    int got = 0;
    std::unique_ptr<Fiber> f = Fiber::make(8 << 20, [&] {
        got = deep(2048 * 4);
        main->switch_from(*f);
    });
    f->switch_from(*main);
    CHECK(got == 2048 * 4);
}

} // namespace

int main() {
    fibers_take_turns();
    a_fiber_has_room_for_the_game();
    if (failures) std::printf("%d failed\n", failures);
    return failures ? 1 : 0;
}
