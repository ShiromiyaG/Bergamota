// The decomp's pieces without a game: decomp.txt read as build.py writes
// it, the jumps as 3dsrecomp and Zakuro write them (the bytes of their
// tests), and the addresses a module's imports get.

#include "runtime/decomp.h"

#include <cstdio>

#include "runtime/loader.h"

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

using Bytes = std::vector<uint8_t>;

void reads_every_line() {
    Decomp d;
    std::string error;
    CHECK(parse_decomp("# made by build.py\n"
                       "base 0x07800000\n"
                       "entry 0x07800001\n"
                       "hook 0x00102000 0x07800004 int\n"
                       "hook 0x00102101 0x07800010\n"
                       "module_hook Bag.cro 0x00000180 0x07800020 void\n"
                       "module_import AppDemo.cro 0x00000188 0x07AE7BC4\n"
                       "module_import AppDemo.cro 0x00000010 0x07AE7BC8 seg2\n"
                       "symbol solstice_panels 0x07900000\n",
                       d, error));
    CHECK(error.empty());
    CHECK(d.base == 0x07800000);
    CHECK(d.hooks.size() == 2 && d.hooks[1].first == 0x00102101 && d.hooks[1].second == 0x07800010);
    CHECK(d.returns.size() == 1 && d.returns[0x00102000] == "int");
    CHECK(d.module_hooks.size() == 1 && d.module_hooks[0].module == "Bag" && d.module_hooks[0].offset == 0x180 &&
          d.module_hooks[0].kind == "void");
    CHECK(d.module_imports.size() == 2 && d.module_imports[0].module == "AppDemo" &&
          d.module_imports[0].segment == -1 && d.module_imports[1].segment == 2 &&
          d.module_imports[1].slot == 0x07AE7BC8);
    CHECK(d.symbols["solstice_panels"] == 0x07900000);
}

void refuses_what_it_cannot_use() {
    std::string error;
    Decomp a;
    CHECK(!parse_decomp("hook 0x100000 0x7800000\n", a, error));
    CHECK(error == "decomp.txt needs a base on a page");
    Decomp b;
    CHECK(!parse_decomp("base 0x07800010\n", b, error));
    Decomp c;
    CHECK(!parse_decomp("base 0x07800000\nhook 0x00102000\n", c, error));
    CHECK(error == "decomp.txt: 'hook 0x00102000' is short");
    Decomp d;
    CHECK(!parse_decomp("base 0x07800000\nhook 0x0010zz00 0x07800000\n", d, error));
    CHECK(error == "decomp.txt: '0x0010zz00' is not an address");
}

void stubs_are_the_recompilers() {
    // 3dsrecomp's decomp.rs tests
    CHECK(decomp_stub(0x00402000, 0x07800004) == (Bytes{0x04, 0xF0, 0x1F, 0xE5, 0x04, 0x00, 0x80, 0x07}));
    Bytes word = decomp_stub(0x00402001, 0x07800004);
    CHECK(word.size() == 12 && (Bytes(word.begin(), word.begin() + 4) == Bytes{0x78, 0x47, 0xC0, 0x46}));
    Bytes half = decomp_stub(0x00402003, 0x07800004);
    CHECK(half.size() == 10 && half[0] == 0x78 && half[1] == 0x47);
    CHECK((Bytes(half.begin() + 2, half.begin() + 6) == Bytes{0x04, 0xF0, 0x1F, 0xE5}));
    // the target is always ARM
    CHECK(decomp_stub(0x00402000, 0x07800005)[4] == 0x04);
}

void patches_the_text() {
    Decomp d;
    d.hooks = {{0x00100010, 0x07800000}, {0x00100021, 0x07800008}, {0x00200000, 0x07800010}};
    Bytes text(0x40, 0xAA);
    CHECK(patch_text(d, 0x00100000, text) == 1); // the last is past the text
    CHECK((Bytes(text.begin() + 0x10, text.begin() + 0x18) == decomp_stub(0x00100010, 0x07800000)));
    CHECK(text[0x0F] == 0xAA && text[0x18] == 0xAA);
    CHECK(text[0x20] == 0x78 && text[0x2C] == 0xAA);
}

void module_addresses() {
    std::vector<uint32_t> segments = {0x00800000, 0x00810000, 0x00820000};
    CHECK(module_address(0x00800000, segments, 0x180, -1) == 0x00800180);
    CHECK(module_address(0x00800000, segments, 0x10, 2) == 0x00820010);
    CHECK(module_address(0x00800000, segments, 0x10, 7) == 0x10); // no such segment
}

uint64_t fnv(const uint8_t *bytes, size_t size) {
    uint64_t hash = 0xCBF29CE484222325ull;
    for (size_t i = 0; i < size; i++) hash = (hash ^ bytes[i]) * 0x100000001B3ull;
    return hash;
}

void code_a(Context *) {}
void code_b(Context *) {}

// a page of code at 0x00100000 mapped as the loader maps text
uint32_t map_page(Memory &memory, uint32_t vaddr, uint32_t permission) {
    auto block = memory.allocate(Region::Application, map::PAGE_SIZE);
    memory.map(vaddr, block->paddr, map::PAGE_SIZE, permission, MemoryState::Code);
    return block->paddr;
}

void stale_functions_are_skipped() {
    Memory memory(false, 64u << 20);
    uint32_t paddr = map_page(memory, 0x00100000, READ | EXECUTE);
    uint8_t *page = memory.host(paddr, map::PAGE_SIZE);
    for (int i = 0; i < 16; i++) page[i] = uint8_t(i);
    // two functions of a piece each, an entry of each and one written by hand
    Piece pieces[] = {{0x00100000, 0x00100008, fnv(page, 8)}, {0x00100008, 0x00100010, fnv(page + 8, 8)}};
    Run runs[] = {{0, 1}, {1, 2}};
    Origin functions[] = {{0, 1, 0x00100000}, {1, 1, 0x00100008}};
    uint32_t owners[] = {0, 1, NO_ORIGIN};
    uint8_t stale[2] = {};
    uint32_t starts[2] = {0x00100000, 0x00100008};
    Origins origins = {2, 2, pieces, functions, runs, owners, stale, starts};
    Entry entries[] = {{0x00100000, code_a}, {0x00100008, code_b}, {0x0010000C, code_a}};
    Library library;
    library.linked(entries, 3, nullptr, 0, &origins);
    CHECK(library.check(0, 0, memory) == 0);
    CHECK(library.lookup(0x00100008) == code_b);
    // the second function's first word turned into a jump
    memory.write32_privileged(0x00100008, 0xE51FF004);
    CHECK(library.check(0, 0, memory) == 1);
    CHECK(stale[1] == 1 && starts[1] == NO_START && stale[0] == 0);
    CHECK(library.lookup(0x00100008) == nullptr);
    CHECK(library.lookup(0x00100000) == code_a);
    CHECK(library.lookup(0x0010000C) == code_a); // written by hand: never stale
    // back as it was
    memory.write32_privileged(0x00100008, 0x0B0A0908);
    CHECK(library.check(0, 0, memory) == 0);
    CHECK(starts[1] == 0x00100008 && library.lookup(0x00100008) == code_b);
}

void modules_get_their_jumps() {
    Memory memory(false, 64u << 20);
    map_page(memory, 0x00800000, READ | EXECUTE);           // the module
    map_page(memory, 0x07800000, READ | WRITE | EXECUTE); // the decomp
    Decomp d;
    d.base = 0x07800000;
    d.module_hooks = {{"Bag", 0x100, 0x07800010, "void"}, {"Bag", 0x203, 0x07800020, ""}, {"Shop", 0x10, 0x07800030, ""}};
    d.module_imports = {{"Bag", 0x180, 0x07800400, -1}, {"Bag", 0x10, 0x07800404, 1}, {"Shop", 0x20, 0x07800408, -1}};
    CroLoader::Module bag{"Bag", 0x00800000, 0x1000, false, {}, {}, {0x00800000, 0x00800800}};
    CHECK(hook_module(memory, d, bag) == 2);
    CHECK(memory.read32(0x07800400) == 0x00800180);
    CHECK(memory.read32(0x07800404) == 0x00800810);
    CHECK(memory.read32(0x07800408) == 0); // another module's
    CHECK(memory.read32(0x00800100) == 0xE51FF004 && memory.read32(0x00800104) == 0x07800010);
    // Thumb from half a word in: bx pc, then the jump on the next word
    CHECK(memory.read16(0x00800202) == 0x4778 && memory.read32(0x00800204) == 0xE51FF004);
    CHECK(memory.read32(0x00800208) == 0x07800020);
    unhook_module(memory, d, "Bag.cro");
    CHECK(memory.read32(0x07800400) == 0 && memory.read32(0x07800404) == 0);
}

void the_library_has_to_have_the_decomp() {
    Memory memory(false, 64u << 20);
    Entry entries[] = {{0x00100000, code_a}};
    uint32_t decomp_base = 0;
    Entry decomp_entries[] = {{0x0, code_b}};
    Module with[] = {{"decomp", &decomp_base, 0x1000, 1, decomp_entries}};
    Decomp d;
    d.base = 0x07800000;
    std::string error;
    Library without;
    without.linked(entries, 1, nullptr, 0, nullptr);
    CHECK(!link_code(without, memory, &d, error) && !error.empty());
    CHECK(link_code(without, memory, nullptr, error)); // without a decomp, any library
    Library library;
    library.linked(entries, 1, with, 1, nullptr);
    CHECK(link_code(library, memory, &d, error));
    CHECK(decomp_base == 0x07800000 && library.lookup(0x07800000) == code_b);
}

} // namespace

int main() {
    the_library_has_to_have_the_decomp();
    stale_functions_are_skipped();
    modules_get_their_jumps();
    reads_every_line();
    refuses_what_it_cannot_use();
    stubs_are_the_recompilers();
    patches_the_text();
    module_addresses();
    if (failures) std::printf("%d failed\n", failures);
    return failures ? 1 : 0;
}
