// The recompiler's decoding and discovery without a game: the tests of
// 3dsrecomp's arm.rs, thumb.rs and discover.rs (Rust, MIT, Copyright (c)
// 2026 fearkov; see LICENSE-3dsrecomp), the same words and the same answers.

#include "decode.h"
#include "discover.h"

#include <algorithm>
#include <cstdio>

using namespace recomp;

namespace {

int failures = 0;

#define CHECK(cond)                                                                                                    \
    do {                                                                                                               \
        if (!(cond)) {                                                                                                 \
            std::printf("%s:%d: %s\n", __FILE__, __LINE__, #cond);                                                     \
            failures++;                                                                                                \
        }                                                                                                              \
    } while (0)

constexpr uint32_t BASE = 0x00100000;

Flow branch(uint32_t target, bool link) { return Flow{.kind = Flow::Branch, .target = target, .link = link}; }
Flow call_other_mode(uint32_t target) { return Flow{.kind = Flow::CallOtherMode, .target = target}; }
Flow branch_register(uint32_t reg, bool link) { return Flow{.kind = Flow::BranchRegister, .link = link, .reg = reg}; }
Flow literal_load(uint32_t literal, uint32_t size) {
    return Flow{.kind = Flow::LiteralLoad, .literal = literal, .size = size};
}
Flow of(Flow::Kind kind) { return Flow{.kind = kind}; }

namespace arm_tests {

Flow flow(uint32_t word) { return arm::decode(word, BASE).flow; }

void branches_and_calls() {
    CHECK(flow(0xEA000000) == branch(BASE + 8, false));
    CHECK(flow(0xEB000001) == branch(BASE + 12, true));
    CHECK(flow(0xEAFFFFFE) == branch(BASE, false));
    CHECK(flow(0xFB000000) == call_other_mode(BASE + 10));
}

void returns() {
    CHECK(flow(0xE12FFF1E) == of(Flow::Return)); // bx lr
    CHECK(flow(0xE8BD8010) == of(Flow::Return)); // pop r4, pc
    CHECK(flow(0xE1A0F00E) == of(Flow::Return)); // mov pc, lr
    CHECK(flow(0xE49DF004) == of(Flow::Return)); // ldr pc, [sp], 4
}

void indirect_flow() {
    CHECK(flow(0xE12FFF33) == branch_register(3, true));                      // blx r3
    CHECK(flow(0x908FF100) == of(Flow::JumpTable));                           // addls pc, pc, r0 lsl 2
    CHECK(flow(0xE593F000) == of(Flow::IndirectJump));                        // ldr pc, [r3]
    CHECK(flow(0x979FF100) == (Flow{.kind = Flow::AddressTable, .index = 0})); // ldrls pc, [pc, r0 lsl 2]
}

void literals() {
    CHECK(flow(0xE59F0004) == literal_load(BASE + 12, 4));
    CHECK(flow(0xE59FF000) == (Flow{.kind = Flow::LoadPcLiteral, .literal = BASE + 8}));
    CHECK(flow(0xED9F0A01) == literal_load(BASE + 12, 4)); // vldr s0
}

void ordinary_instructions_do_not_change_flow() {
    CHECK(flow(0xE3500005) == of(Flow::Other)); // cmp r0, 5
    CHECK(flow(0xE0810002) == of(Flow::Other)); // add r0, r1, r2
    CHECK(flow(0xE0000291) == of(Flow::Other)); // mul r0, r1, r2
    CHECK(flow(0xEF000032) == of(Flow::Svc));
}

} // namespace arm_tests

namespace thumb_tests {

Flow flow(uint16_t halfword) { return thumb::decode(halfword, std::nullopt, BASE).first.flow; }

void branches() {
    CHECK(flow(0xE7FE) == branch(BASE, false));     // b .
    CHECK(flow(0xD001) == branch(BASE + 6, false)); // beq
    CHECK(thumb::decode(0xD001, std::nullopt, BASE).first.condition == 0);
    CHECK(flow(0xDF01) == of(Flow::Svc));
}

void calls_take_two_halfwords() {
    // bl to BASE + 0x1000
    auto [instruction, size] = thumb::decode(0xF000, 0xFFFE, BASE);
    CHECK(instruction.flow == branch(BASE + 0x1000, true));
    CHECK(size == 4);
    // blx to ARM code, aligned down
    CHECK(thumb::decode(0xF000, 0xE802, BASE).first.flow == call_other_mode(BASE + 8));
}

void returns_and_indirect_jumps() {
    CHECK(flow(0x4770) == of(Flow::Return));        // bx lr
    CHECK(flow(0xBD10) == of(Flow::Return));        // pop r4, pc
    CHECK(flow(0x46F7) == of(Flow::Return));        // mov pc, lr
    CHECK(flow(0x4798) == branch_register(3, true)); // blx r3
    CHECK(flow(0x4487) == of(Flow::IndirectJump));  // add pc, r0
}

void literals_are_word_aligned() {
    // ldr r0, [pc, 4] at BASE + 2 reads BASE + 8
    CHECK(thumb::decode(0x4801, std::nullopt, BASE + 2).first.flow == literal_load(BASE + 8, 4));
}

} // namespace thumb_tests

namespace discover_tests {

Program program(const std::vector<uint32_t> &words) {
    Program program;
    program.text.base = BASE;
    for (uint32_t word : words)
        for (int i = 0; i < 4; i++) program.text.bytes.push_back(uint8_t(word >> (i * 8)));
    program.seeds = {{BASE, Source::Entry}};
    return program;
}

Analysis analyze_words(const std::vector<uint32_t> &words) { return analyze(program(words)); }

std::vector<uint32_t> keys(const Analysis &analysis) {
    std::vector<uint32_t> keys;
    for (const auto &[entry, function] : analysis.functions) keys.push_back(entry);
    return keys;
}

void follows_calls_and_marks_literals() {
    Analysis analysis = analyze_words({
        0xEB000002, // bl 0x100010
        0xEF000000, // svc 0
        0xEAFFFFFE, // b .
        0xDEADBEEF, // never reached
        0xE59F0000, // ldr r0, [pc] (literal at 0x100018)
        0xE12FFF1E, // bx lr
        0x00100000, // the literal
    });
    CHECK(keys(analysis) == (std::vector<uint32_t>{BASE, BASE + 0x10}));
    CHECK(analysis.functions[BASE].labels == (std::set<uint32_t>{BASE, BASE + 4, BASE + 8}));
    CHECK(analysis.functions[BASE + 0x10].source == Source::Call);
    CHECK(analysis.count(Byte::Code) == 5 * 4);
    CHECK(analysis.count(Byte::Literal) == 4);
    CHECK(analysis.largest_gaps(BASE, 1) == (std::vector<std::pair<uint32_t, size_t>>{{BASE + 0xC, 4}}));
    CHECK(analysis.svc_sites == 1);
}

// code reached only through branches far apart, the way tail calls reach
// other functions, is one function while it is small, as it always was.
// past what a function holds, each piece of it becomes a function of its
// own, its branch a tail call.
void a_huge_chain_of_tail_calls_is_split_into_functions() {
    constexpr uint32_t SPACING = 2 * 0x4000;
    auto chain = [](uint32_t links, uint32_t length) {
        std::vector<uint32_t> words(links * SPACING / 4, 0);
        for (uint32_t link = 0; link < links; link++) {
            uint32_t start = link * SPACING / 4;
            for (uint32_t i = 0; i < length; i++) words[start + i] = 0xE1A00000; // mov r0, r0
            words[start + length] = link + 1 == links ? 0xE12FFF1E                          // bx lr
                                                      : 0xEA000000 | ((SPACING - length * 4 - 8) / 4); // b to the next piece
        }
        return analyze_words(words);
    };
    Analysis small = chain(3, 100);
    CHECK(small.functions.size() == 1);
    CHECK(small.functions[BASE].instructions.size() == 3 * 101);
    Analysis huge = chain(30, 400);
    CHECK(huge.functions.size() == 30);
    CHECK(std::all_of(huge.functions.begin(), huge.functions.end(),
                      [](const auto &function) { return function.second.instructions.size() == 401; }));
    CHECK(std::all_of(std::next(huge.functions.begin()), huge.functions.end(),
                      [](const auto &function) { return function.second.source == Source::Call; }));
}

void a_switch_table_leads_to_every_case() {
    Analysis analysis = analyze_words({
        0xE3500001, // cmp r0, 1
        0x908FF100, // addls pc, pc, r0 lsl 2
        0xEA000002, // b default (0x100018)
        0xEA000002, // b case 0 (0x10001c)
        0xEA000002, // b case 1 (0x100020)
        0x00000000, // data between the table and the cases
        0xE12FFF1E, // default
        0xE12FFF1E, // case 0
        0xE12FFF1E, // case 1
    });
    CHECK(analysis.jump_tables == 1);
    CHECK(analysis.count(Byte::Code) == 8 * 4);
    std::vector<uint32_t> labels;
    for (uint32_t label : analysis.functions[BASE].labels) labels.push_back(label - BASE);
    CHECK(labels == (std::vector<uint32_t>{0, 8, 0xC, 0x10, 0x18, 0x1C, 0x20}));
}

void an_address_table_is_sized_by_its_compare() {
    Analysis analysis = analyze_words({
        0xE3500001, // cmp r0, 1
        0x979FF100, // ldrls pc, [pc, r0 lsl 2]
        0xE12FFF1E, // bx lr, the default case
        BASE + 0x14, // case 0
        BASE + 0x18, // case 1
        0xE12FFF1E, // case 0
        0xE12FFF1E, // case 1
    });
    CHECK(analysis.jump_tables == 1);
    CHECK(analysis.count(Byte::Literal) == 8);
    CHECK(analysis.count(Byte::Code) == 5 * 4);
}

void blx_leads_into_thumb_code() {
    Analysis analysis = analyze_words({
        0xFA000000, // blx 0x100008
        0xE12FFF1E, // bx lr
        0x47702001, // movs r0, 1 then bx lr, in Thumb
    });
    const Function &thumb = analysis.functions[BASE + 8];
    CHECK(thumb.mode == Mode::Thumb);
    CHECK(thumb.instructions == (std::vector<uint32_t>{BASE + 8, BASE + 10}));
}

void pointer_seeds_become_functions() {
    Program p = program({0xE12FFF1E, 0x47704770, 0xE12FFF1E});
    p.seeds.push_back({BASE + 8, Source::Pointer});
    p.seeds.push_back({BASE + 5, Source::Pointer});
    Analysis analysis = analyze(p);
    CHECK(analysis.functions[BASE + 8].source == Source::Pointer);
    CHECK(analysis.functions[BASE + 4].mode == Mode::Thumb);
}

void with_slots_only_relocated_literals_are_pointers() {
    std::vector<uint32_t> words = {
        0xE59F0000, // ldr r0, [pc] (literal at 0x100008)
        0xE12FFF1E, // bx lr
        BASE + 0xC, // looks like a code address
        0xE12FFF1E, // bx lr
    };
    CHECK(analyze_words(words).functions[BASE + 0xC].source == Source::Pointer);
    Program p = program(words);
    p.slots = std::set<uint32_t>{};
    // the code is still there for the scan to find, only not through the
    // literal
    CHECK(analyze(p).functions[BASE + 0xC].source == Source::Scan);
}

// code nothing leads to still becomes functions where a gap begins with an
// instruction or a push saves registers, data does not.
void gaps_are_scanned_for_functions() {
    Analysis analysis = analyze_words({
        0xE12FFF1E, // bx lr, the entry
        0xE3A00001, // mov r0, #1, the start of a gap
        0xE12FFF1E, // bx lr
        0x12345678, // data
        0xE92D4010, // push {r4, lr}
        0xE8BD8010, // pop {r4, pc}
    });
    CHECK(analysis.functions[BASE + 4].source == Source::Scan);
    CHECK(analysis.functions[BASE + 16].source == Source::Scan);
    CHECK(!analysis.functions.contains(BASE + 12));
}

} // namespace discover_tests

} // namespace

int main() {
    arm_tests::branches_and_calls();
    arm_tests::returns();
    arm_tests::indirect_flow();
    arm_tests::literals();
    arm_tests::ordinary_instructions_do_not_change_flow();
    thumb_tests::branches();
    thumb_tests::calls_take_two_halfwords();
    thumb_tests::returns_and_indirect_jumps();
    thumb_tests::literals_are_word_aligned();
    discover_tests::follows_calls_and_marks_literals();
    discover_tests::a_huge_chain_of_tail_calls_is_split_into_functions();
    discover_tests::a_switch_table_leads_to_every_case();
    discover_tests::an_address_table_is_sized_by_its_compare();
    discover_tests::blx_leads_into_thumb_code();
    discover_tests::pointer_seeds_become_functions();
    discover_tests::with_slots_only_relocated_literals_are_pointers();
    discover_tests::gaps_are_scanned_for_functions();
    if (failures) std::printf("%d failed\n", failures);
    return failures ? 1 : 0;
}
