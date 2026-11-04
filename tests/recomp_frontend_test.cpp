// The recompiler's front end without a game: a small title written for the
// tests read back, .code decompressed, CRO modules, the decomp's jumps,
// override marks, the heat, and a title's programs with what a mod changes.
// These are 3dsrecomp's own tests (rom.rs, image.rs, cro.rs, decomp.rs,
// overrides.rs, hot.rs and lib.rs; Rust, MIT, Copyright (c) 2026 fearkov;
// see LICENSE-3dsrecomp), their helpers too.

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "../recomp/cro.h"
#include "../recomp/decomp.h"
#include "../recomp/discover.h"
#include "../recomp/hot.h"
#include "../recomp/image.h"
#include "../recomp/overrides.h"
#include "../recomp/programs.h"
#include "../recomp/rom.h"

#ifdef _WIN32
#include <process.h>
static int process_id() { return _getpid(); }
#else
#include <unistd.h>
static int process_id() { return int(getpid()); }
#endif

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

using Bytes = std::vector<uint8_t>;

Bytes text_bytes(const char *text) { return Bytes(text, text + std::strlen(text)); }

Bytes bytes(const std::vector<uint32_t> &words) {
    Bytes out;
    for (uint32_t word : words)
        for (int i = 0; i < 4; i++) out.push_back(uint8_t(word >> (8 * i)));
    return out;
}

size_t next_multiple_of(size_t n, size_t rhs) { return n % rhs == 0 ? n : n + (rhs - n % rhs); }

// --- rom.rs's tests

// the id of the titles tests make, which no real title has
constexpr uint64_t PROGRAM_ID = 0x000400000FF3DE00;
constexpr uint64_t MEDIA_UNIT = 0x200;
constexpr uint32_t NONE = 0xFFFFFFFF;

void put(Bytes &out, size_t at, const Bytes &value) { std::memcpy(out.data() + at, value.data(), value.size()); }
void put32(Bytes &out, size_t at, uint32_t value) { put(out, at, bytes({value})); }
void push32(Bytes &out, uint32_t value) {
    for (int i = 0; i < 4; i++) out.push_back(uint8_t(value >> (8 * i)));
}
void push64(Bytes &out, uint64_t value) {
    for (int i = 0; i < 8; i++) out.push_back(uint8_t(value >> (8 * i)));
}

struct RomFile {
    std::string path;
    Bytes bytes;
};

// a RomFS without hashes, its IVFC header and then level 3, holding files at
// their paths in the folders those need
Bytes romfs_image(const std::vector<RomFile> &files) {
    // every folder's path and its parent, the root first, and every file's
    // folder, name and bytes
    std::vector<std::pair<std::string, size_t>> dirs = {{"", 0}};
    struct Leaf {
        size_t parent;
        std::string name;
        const Bytes *bytes;
    };
    std::vector<Leaf> leaves;
    for (const RomFile &file : files) {
        size_t slash = file.path.rfind('/');
        std::string folder = slash == std::string::npos ? "" : file.path.substr(0, slash);
        std::string name = slash == std::string::npos ? file.path : file.path.substr(slash + 1);
        size_t parent = 0;
        std::string at;
        size_t start = 0;
        while (start <= folder.size()) {
            size_t end = folder.find('/', start);
            if (end == std::string::npos) end = folder.size();
            std::string part = folder.substr(start, end - start);
            start = end + 1;
            if (part.empty()) continue;
            at = at.empty() ? part : at + "/" + part;
            size_t index = 0;
            while (index < dirs.size() && dirs[index].first != at) index++;
            if (index == dirs.size()) dirs.emplace_back(at, parent);
            parent = index;
        }
        leaves.push_back({parent, name, &file.bytes});
    }
    auto utf16 = [](const std::string &name) {
        Bytes out;
        for (char c : name) {
            out.push_back(uint8_t(c));
            out.push_back(0);
        }
        return out;
    };
    std::vector<Bytes> dir_names, file_names;
    for (const auto &[path, _] : dirs) {
        size_t slash = path.rfind('/');
        dir_names.push_back(utf16(slash == std::string::npos ? path : path.substr(slash + 1)));
    }
    for (const Leaf &leaf : leaves) file_names.push_back(utf16(leaf.name));
    // where each entry starts in its table
    auto offsets = [](const std::vector<Bytes> &names, size_t header) {
        std::vector<uint32_t> out;
        size_t end = 0;
        for (const Bytes &name : names) {
            out.push_back(uint32_t(end));
            end += header + next_multiple_of(name.size(), 4);
        }
        return out;
    };
    std::vector<uint32_t> dir_at = offsets(dir_names, 0x18), file_at = offsets(file_names, 0x20);
    auto first = [](std::optional<size_t> found, const std::vector<uint32_t> &at) {
        return found ? at[*found] : NONE;
    };

    Bytes dir_table;
    for (size_t index = 0; index < dirs.size(); index++) {
        size_t parent = dirs[index].second;
        std::optional<size_t> next, child, file;
        if (index != 0)
            for (size_t other = index + 1; other < dirs.size() && !next; other++)
                if (dirs[other].second == parent) next = other;
        for (size_t other = 1; other < dirs.size() && !child; other++)
            if (dirs[other].second == index) child = other;
        for (size_t other = 0; other < leaves.size() && !file; other++)
            if (leaves[other].parent == index) file = other;
        const Bytes &name = dir_names[index];
        for (uint32_t word : {dir_at[parent], first(next, dir_at), first(child, dir_at), first(file, file_at), NONE,
                              uint32_t(name.size())})
            push32(dir_table, word);
        dir_table.insert(dir_table.end(), name.begin(), name.end());
        dir_table.resize(next_multiple_of(dir_table.size(), 4), 0);
    }
    Bytes file_table, data;
    for (size_t index = 0; index < leaves.size(); index++) {
        size_t parent = leaves[index].parent;
        data.resize(next_multiple_of(data.size(), 16), 0);
        std::optional<size_t> next;
        for (size_t other = index + 1; other < leaves.size() && !next; other++)
            if (leaves[other].parent == parent) next = other;
        const Bytes &name = file_names[index];
        push32(file_table, dir_at[parent]);
        push32(file_table, first(next, file_at));
        push64(file_table, data.size());
        push64(file_table, leaves[index].bytes->size());
        push32(file_table, NONE);
        push32(file_table, uint32_t(name.size()));
        file_table.insert(file_table.end(), name.begin(), name.end());
        file_table.resize(next_multiple_of(file_table.size(), 4), 0);
        data.insert(data.end(), leaves[index].bytes->begin(), leaves[index].bytes->end());
    }

    // the IVFC header, no master hash and level 3 in blocks of 16 bytes right
    // after it, then level 3, its header, the tables without the hash tables,
    // and the file data
    Bytes out(0x60, 0);
    put(out, 0, text_bytes("IVFC"));
    out[0x4C] = 4;
    size_t dirs_at = 0x28, files_at = 0x28 + dir_table.size();
    size_t data_at = next_multiple_of(files_at + file_table.size(), 16);
    for (size_t word : {size_t(0x28), dirs_at, size_t(0), dirs_at, dir_table.size(), files_at, size_t(0), files_at,
                        file_table.size(), data_at})
        push32(out, uint32_t(word));
    out.insert(out.end(), dir_table.begin(), dir_table.end());
    out.insert(out.end(), file_table.begin(), file_table.end());
    out.resize(0x60 + data_at, 0);
    out.insert(out.end(), data.begin(), data.end());
    return out;
}

// writes a decrypted .cxi to a file named after test and returns its path.
// code is all text, at 0x00100000, and a RomFS holds files at their paths
// when there are any.
std::filesystem::path rom(const std::string &test, const Bytes &code, const std::vector<RomFile> &files) {
    // the NCCH header, never encrypted, the exheader after it, and the ExeFS
    // at 0x600 with .code its only file
    Bytes out(0x800, 0);
    put(out, 0x100, text_bytes("NCCH"));
    Bytes id;
    push64(id, PROGRAM_ID);
    put(out, 0x118, id);
    out[0x18F] = 0x04;
    put32(out, 0x1A0, 3);
    put(out, 0x200, text_bytes("test"));
    put(out, 0x210, bytes({0x00100000, uint32_t((code.size() + 0xFFF) / 0x1000), uint32_t(code.size())}));
    put(out, 0x600, text_bytes(".code"));
    put32(out, 0x60C, uint32_t(code.size()));
    out.insert(out.end(), code.begin(), code.end());
    if (!files.empty()) {
        size_t at = next_multiple_of(out.size(), MEDIA_UNIT);
        Bytes romfs = romfs_image(files);
        put32(out, 0x1B0, uint32_t(at / MEDIA_UNIT));
        put32(out, 0x1B4, uint32_t((romfs.size() + MEDIA_UNIT - 1) / MEDIA_UNIT));
        out.resize(at, 0);
        out.insert(out.end(), romfs.begin(), romfs.end());
    }
    auto path = std::filesystem::temp_directory_path() /
                ("3dsrecomp-" + test + "-" + std::to_string(process_id()) + ".cxi");
    std::ofstream(path, std::ios::binary).write(reinterpret_cast<const char *>(out.data()), std::streamsize(out.size()));
    return path;
}

// the fixture reads back as the title it describes
void a_title_made_for_tests_reads_back() {
    auto path = rom("fixture", {1, 2, 3, 4},
                    {{"static.crs", text_bytes("crs")},
                     {"cro/Battle.cro", text_bytes("battle")},
                     {"cro/Field.cro", text_bytes("field")}});
    {
        Title title;
        std::string error;
        CHECK(Title::load(path, title, error));
        CHECK(title.program_id() == PROGRAM_ID);
        Bytes code;
        CHECK(title.code(code, error) && code == (Bytes{1, 2, 3, 4}));
        CHECK(title.exheader.text.address == 0x00100000);
        CHECK(title.romfs.has_value());
        if (title.romfs) {
            auto read = [&](const std::string &file) -> Bytes {
                auto entry = title.romfs->lookup(file, error);
                if (!entry) return {};
                return title.read_romfs(*entry, 0, size_t(entry->data_size)).value_or(Bytes{});
            };
            CHECK(read("static.crs") == text_bytes("crs"));
            CHECK(read("cro/Battle.cro") == text_bytes("battle"));
            CHECK(read("CRO/field.cro") == text_bytes("field"));
            CHECK(!title.romfs->lookup("cro/Town.cro", error));
            CHECK(error == "cro/Town.cro is not in the RomFS");
        }
    }
    std::filesystem::remove(path);
}

// a literal and a run, the smallest stream that exercises both
void backwards_lzss_decompresses() {
    // the output ends in "abcabc", three literals, then a run of three
    // copying them from three bytes further on
    Bytes stream = {0xFF, 0xFF};
    // the run, a length and an offset of zero, the shortest there is
    stream.push_back(0);
    stream.push_back(0);
    for (char c : std::string("abc")) stream.push_back(uint8_t(c));
    // control, three literals then a run, read from the high bit
    stream.push_back(0b00010000);
    // footer, 8 bytes of footer, the compressed part spans all but the two
    // filler bytes, and three more bytes come out
    uint32_t span = uint32_t(stream.size() + 8 - 2);
    push32(stream, (8u << 24) | span);
    push32(stream, 3);
    Bytes out;
    std::string error;
    CHECK(decompress(stream, out, error));
    CHECK(out.size() >= 6 && Bytes(out.end() - 6, out.end()) == text_bytes("abcabc"));
}

// --- image.rs's tests

constexpr uint32_t BASE = 0x00100000;

Segment segment(uint32_t base, const std::vector<uint32_t> &words) {
    Segment out;
    out.base = base;
    out.bytes = bytes(words);
    return out;
}

void pointers_in_data_become_functions() {
    Image image;
    image.entry = BASE;
    image.text = segment(BASE, {0xE12FFF1E, 0x47704770, 0xE12FFF1E});
    image.rodata = segment(0x00200000, {BASE + 8, 12345});
    image.data = segment(0x00300000, {BASE + 5});
    Program program = image.into_program({}, {});
    CHECK(program.seeds.size() == 3);
    Analysis analysis = analyze(program);
    CHECK(analysis.functions.count(BASE + 8) && analysis.functions.at(BASE + 8).source == Source::Pointer);
    CHECK(analysis.functions.count(BASE + 4) && analysis.functions.at(BASE + 4).mode == Mode::Thumb);
}

// --- cro.rs's tests

// a module called name, its code the words at 0x200, exporting where that
// starts
Bytes named(const std::string &name, const std::vector<uint32_t> &code) {
    Bytes out(0x200 + code.size() * 4, 0);
    put(out, 0x80, text_bytes("CRO0"));
    put32(out, 0xC0, 0x180);
    // one segment, the code, and one export by index, its start
    put32(out, 0xC8, 0x138);
    put32(out, 0xCC, 1);
    put32(out, 0x138, 0x200);
    put32(out, 0x13C, uint32_t(code.size() * 4));
    put32(out, 0xD8, 0x150);
    put32(out, 0xDC, 1);
    for (size_t i = 0; i < code.size(); i++) put32(out, 0x200 + i * 4, code[i]);
    std::memcpy(out.data() + 0x180, name.data(), name.size());
    return out;
}

// a module with a header, a code segment at 0x200 and a data segment at
// 0x300, holding the code words and one relocation per pointer, each as
// (segment tag of the slot, offset in code)
Bytes module(const std::vector<uint32_t> &code, const std::vector<std::pair<uint32_t, uint32_t>> &pointers) {
    Bytes out(0x400, 0);
    put(out, 0x80, text_bytes("CRO0"));
    put32(out, 0xC8, 0x138);
    put32(out, 0xCC, 2);
    put32(out, 0x138, 0x200);
    put32(out, 0x13C, uint32_t(code.size() * 4));
    put32(out, 0x140, 0);
    put32(out, 0x144, 0x300);
    put32(out, 0x148, 0x100);
    put32(out, 0x14C, 2);
    put32(out, 0x128, 0x150);
    put32(out, 0x12C, uint32_t(pointers.size()));
    for (size_t i = 0; i < pointers.size(); i++) {
        size_t entry = 0x150 + i * 12;
        put32(out, entry, pointers[i].first);
        put32(out, entry + 4, 2);
        put32(out, entry + 8, pointers[i].second);
    }
    for (size_t i = 0; i < code.size(); i++) put32(out, 0x200 + i * 4, code[i]);
    return out;
}

// a module names what it takes from another module without a name as that
// module's segment tags, which lead into its code
void anonymous_imports_lead_into_the_other_module() {
    Bytes data = module(std::vector<uint32_t>(8, 0xE12FFF1E), {});
    // one module imported from, with one anonymous symbol, segment 0 at
    // offset 0x10
    put32(data, 0xF0, 0x380);
    put32(data, 0xF4, 1);
    put32(data, 0x380, 0x3A0);
    put32(data, 0x38C, 0x3B0);
    put32(data, 0x390, 1);
    put32(data, 0x3B0, 0x10 << 4);
    std::memcpy(data.data() + 0x3A0, "|static|\0", 9);
    auto parsed = cro::parse(data);
    CHECK(parsed.has_value());
    if (!parsed) return;
    CHECK(parsed->anonymous_imports == (std::vector<std::pair<std::string, uint32_t>>{{"|static|", 0x100}}));
    CHECK(parsed->code_address(0x100) == std::optional<uint32_t>(0x210));
    // segment 1 holds data
    CHECK(parsed->code_address(0x101) == std::nullopt);
}

void relocations_fill_in_code_pointers() {
    Bytes data = module(
        {
            0xE59F0000, // ldr r0, [pc] (literal at 0x208)
            0xE12FFF1E, // bx lr
            0,          // the literal, relocated to 0x210
            0xE12FFF1E, // bx lr, only reached through the data
            0xE12FFF1E, // bx lr, only reached through the literal
        },
        // the literal (segment 0, offset 8) and a data word (segment 1)
        {{8 << 4, 0x10}, {1, 0xC}});
    auto parsed = cro::parse(data);
    CHECK(parsed.has_value());
    if (!parsed) return;
    auto program = parsed->program(data, {});
    CHECK(program.has_value());
    if (!program) return;
    CHECK(program->text.read32(0x208) == std::optional<uint32_t>(0x210));
    CHECK(program->slots == std::optional<std::set<uint32_t>>(std::set<uint32_t>{0x208}));
    Analysis analysis = analyze(*program);
    CHECK(analysis.functions.count(0x20C));
    CHECK(analysis.functions.count(0x210));
}

// --- decomp.rs's tests

void arm_function_jumps_with_ldr() {
    CHECK(Decomp::stub(0x00402000, 0x07800004) == (Bytes{0x04, 0xF0, 0x1F, 0xE5, 0x04, 0x00, 0x80, 0x07}));
}

void thumb_function_switches_state_first() {
    // word aligned: bx pc, a nop, then the ARM jump
    Bytes stub = Decomp::stub(0x00402001, 0x07800004);
    CHECK(Bytes(stub.begin(), stub.begin() + 4) == (Bytes{0x78, 0x47, 0xC0, 0x46}));
    CHECK(stub.size() == 12);
    // half a word in: bx pc lands on the next word, where the jump is
    stub = Decomp::stub(0x00402003, 0x07800004);
    CHECK(Bytes(stub.begin(), stub.begin() + 2) == (Bytes{0x78, 0x47}));
    CHECK(Bytes(stub.begin() + 2, stub.begin() + 6) == (Bytes{0x04, 0xF0, 0x1F, 0xE5}));
    CHECK(stub.size() == 10);
}

// not one of the Rust's: decomp.txt read from a folder, every line kind it
// lets through, and its errors word for word
void decomp_txt_is_read() {
    auto dir = std::filesystem::temp_directory_path() / ("3dsrecomp-decomp-" + std::to_string(process_id()));
    std::filesystem::create_directories(dir);
    auto write = [&](const char *name, const std::string &text) {
        std::ofstream(dir / name, std::ios::binary) << text;
    };
    write("decomp.bin", std::string("\x1E\xFF\x2F\xE1", 4));
    write("decomp.txt", "# made by build.py\r\n"
                        "base 0x07800000\n"
                        "  entry 0x07800001  \n"
                        "hook 0x00102000 0x07800004 int\n"
                        "module_hook Bag.cro 0x00000180 0x07800020 void\n"
                        "module_import AppDemo.cro 0x00000188 0x07AE7BC4\n"
                        "symbol solstice_panels 0x07900000\n"
                        "patch 0x00100000 0x07800000\n"
                        "option port_speed int 2 1 4 Speed\n");
    Decomp decomp;
    std::string error;
    CHECK(Decomp::read(dir, decomp, error));
    CHECK(decomp.base == 0x07800000 && decomp.code.size() == 4);
    CHECK(decomp.entries == std::vector<uint32_t>{0x07800001});
    CHECK(decomp.hooks.size() == 1 && decomp.hooks[0] == std::make_pair(0x00102000u, 0x07800004u));
    Program program = decomp.program();
    CHECK(program.seeds.size() == 1 && program.seeds[0].first == 1 && program.seeds[0].second == Source::Export);
    CHECK(program.slots && program.slots->empty());

    auto refused = [&](const std::string &text) {
        write("decomp.txt", text);
        Decomp other;
        error.clear();
        return !Decomp::read(dir, other, error);
    };
    CHECK(refused("entry 0x07800001\n") && error == "decomp.txt has no base");
    CHECK(refused("base 0x07800000\nhook 0x00102000\n") && error == "decomp.txt: 'hook 0x00102000' is short");
    CHECK(refused("base 0x0780zz00\n") && error == "decomp.txt: '0x0780zz00' is not an address");
    CHECK(refused("base 0x07800000\nhooks 0x1 0x2\n") && error == "decomp.txt: 'hooks' is not a line it knows");
    // trim_start_matches takes every 0x off
    CHECK(!refused("base 0x0x07800000\n"));
    std::filesystem::remove_all(dir);

    Segment text = segment(0x00102000, {0, 0, 0, 0});
    decomp.hooks = {{0x00102001, 0x07800004}, {0x00100000, 0x07800008}};
    decomp.patch(text);
    CHECK(Bytes(text.bytes.begin(), text.bytes.begin() + 4) == (Bytes{0x78, 0x47, 0xC0, 0x46}));
}

// --- overrides.rs's tests

void marks_are_found_outside_comments() {
    const char *source = R"src(
            #include "overrides.h"
            /* RECOMP_OVERRIDE(0x1000) is only an example */
            // RECOMP_OVERRIDE(0x2000) too
            const char *text = "RECOMP_OVERRIDE(0x3000)";
            RECOMP_OVERRIDE(0x0012A4F1) {
                RETURN_TO(ctx->r[14]);
            }
            RECOMP_OVERRIDE_IN(DllField, 0x40) {
                CALL(RECOMP_ORIGINAL_IN(DllField, 0x40));
            }
        )src";
    std::vector<Override> overrides;
    std::string error;
    CHECK(parse_overrides(source, overrides, error));
    std::vector<Override> expected = {
        {std::nullopt, 0x0012A4F1, "override_0x0012A4F1", "original_0x0012A4F1"},
        {std::string("DllField"), 0x40, "override_DllField_0x40", "original_DllField_0x40"},
    };
    CHECK(overrides == expected);
}

void bad_marks_are_reported() {
    std::vector<Override> overrides;
    std::string error;
    CHECK(!parse_overrides("RECOMP_OVERRIDE(main) {}", overrides, error));
    CHECK(error == "main is not an address");
    CHECK(!parse_overrides("RECOMP_OVERRIDE(0x10) {} RECOMP_OVERRIDE(16) {}", overrides, error));
    CHECK(error == "0x00000010 is overridden twice");
    CHECK(!parse_overrides("RECOMP_OVERRIDE_IN(0x10) {}", overrides, error));
    CHECK(error == "RECOMP_OVERRIDE_IN takes a module and an offset");
}

// --- hot.rs's tests

// three functions, the first called from the others, each returning
Program hot_program() {
    Program program;
    program.text = segment(0x00100000, {0xEB000002, 0xE12FFF1E, 0xEAFFFFFE, 0xE12FFF1E, 0xE12FFF1E});
    program.seeds = {{0x00100000, Source::Entry}, {0x00100008, Source::Entry}};
    return program;
}

// the hottest functions come first, until they cover the share, and a place
// in a module counts for the module's function, not the executable's at the
// same address
void the_hottest_functions_cover_the_share() {
    std::vector<std::pair<std::string, Program>> programs = {{"executable", hot_program()}, {"Battle", hot_program()}};
    std::vector<Analysis> analyses;
    for (const auto &[_, program] : programs) analyses.push_back(analyze(program));
    std::vector<uint32_t> entries;
    for (const auto &[entry, _] : analyses[0].functions) entries.push_back(entry);
    CHECK(entries.size() >= 2);
    if (entries.size() < 2) return;
    char text[256];
    std::snprintf(text, sizeof text, "# a note\nexecutable %08X 900\nexecutable %08X 1\nBattle %08X 99\n", entries[0],
                  entries[1], entries[1]);
    hot::Heat heat = hot::Heat::parse(text);
    hot::Selection all = hot::select(programs, analyses, heat, 1.0);
    CHECK(all.keep[0] == (std::set<uint32_t>{entries[0], entries[1]}));
    CHECK(all.keep[1] == (std::set<uint32_t>{entries[1]}));
    hot::Selection most = hot::select(programs, analyses, heat, 0.9);
    CHECK(most.keep[0] == (std::set<uint32_t>{entries[0]}));
    CHECK(most.keep[1].empty());

    // not one of the Rust's: prune keeps what select chose and what the
    // overrides replace
    std::map<size_t, std::set<uint32_t>> also = {{1, {entries[0]}}};
    hot::prune(analyses, most.keep, also);
    CHECK(analyses[0].functions.size() == 1 && analyses[0].functions.count(entries[0]));
    CHECK(analyses[1].functions.size() == 1 && analyses[1].functions.count(entries[0]));
}

// --- lib.rs's tests

void the_executable_is_made_of_the_code_a_mod_gives() {
    Bytes own = bytes({0xE3A00000, 0xE12FFF1E}), modded = bytes({0xE3A00001, 0xE12FFF1E});
    auto path = rom("code-mod", own, {});
    {
        Title title;
        std::string error;
        CHECK(Title::load(path, title, error));
        auto text = [&](const Mods &mods) -> Bytes {
            std::vector<std::pair<std::string, Program>> list;
            if (!programs(title, mods, list, error) || list.empty()) return {};
            return list[0].second.text.bytes;
        };
        CHECK(text(Mods{}) == own);
        Mods mods;
        mods.code = &modded;
        CHECK(text(mods) == modded);
    }
    std::filesystem::remove(path);
}

// a mod that makes the code longer gives an exheader saying so, and its text
// is all of it, where the title's own exheader would cut it
void a_mods_exheader_says_how_much_of_its_code_is_text() {
    Bytes own = bytes({0xE3A00000, 0xE12FFF1E});
    Bytes longer = bytes({0xE3A00000, 0xE12FFF1E, 0xE12FFF1E});
    auto path = rom("exheader-mod", own, {});
    {
        Title title;
        std::string error;
        CHECK(Title::load(path, title, error));
        Bytes exheader(0x400, 0);
        put(exheader, 0x10, bytes({0x00100000, 1, 12}));
        auto text = [&](const Mods &mods) -> Bytes {
            std::vector<std::pair<std::string, Program>> list;
            if (!programs(title, mods, list, error) || list.empty()) return {};
            return list[0].second.text.bytes;
        };
        Mods mods;
        mods.code = &longer;
        CHECK(text(mods) == own);
        mods.exheader = &exheader;
        CHECK(text(mods) == longer);
    }
    std::filesystem::remove(path);
}

// the modules and the static module a mod replaces come from it, asked for
// by their paths, and the rest from the title
void the_modules_a_mod_replaces_come_from_it() {
    std::vector<uint32_t> own = {0xE3A00000, 0xE12FFF1E}, modded = {0xE3A00001, 0xE12FFF1E};
    std::vector<RomFile> files = {
        {"static.crs", named("|static|", {})},
        {"cro/Battle.cro", named("Battle", own)},
        {"cro/Field.cro", named("Field", {0xE12FFF1E})},
    };
    auto path = rom("module-mod", bytes(own), files);
    {
        Title title;
        std::string error;
        CHECK(Title::load(path, title, error));
        Bytes battle = named("Battle", modded), crs = named("|modded|", {});
        std::vector<std::string> asked;
        Mods mods;
        mods.romfs = [&](const std::string &file) -> std::optional<Bytes> {
            asked.push_back(file);
            if (file == "cro/Battle.cro") return battle;
            if (file == "static.crs") return crs;
            return std::nullopt;
        };
        auto text = [&](const Mods &with, const std::string &name) -> Bytes {
            std::vector<std::pair<std::string, Program>> list;
            if (!programs(title, with, list, error)) return {};
            for (auto &[module, program] : list)
                if (module == name) return program.text.bytes;
            return {};
        };
        CHECK(text(mods, "Battle") == bytes(modded));
        CHECK(text(mods, "Field") == bytes({0xE12FFF1E}));
        auto modded_crs = static_module(title, mods);
        CHECK(modded_crs && modded_crs->name == "|modded|");
        CHECK(text(Mods{}, "Battle") == bytes(own));
        auto own_crs = static_module(title, Mods{});
        CHECK(own_crs && own_crs->name == "|static|");
        for (const char *file : {"static.crs", "cro/Battle.cro", "cro/Field.cro"}) {
            bool found = false;
            for (const auto &one : asked) found = found || one == file;
            CHECK(found);
        }
        // not one of the Rust's: the executable first, the modules by their
        // place in the RomFS, the decomp last
        Decomp decomp;
        decomp.base = 0x07800000;
        decomp.code = bytes({0xE12FFF1E});
        decomp.entries = {0x07800000};
        Mods with_decomp;
        with_decomp.decomp = &decomp;
        std::vector<std::pair<std::string, Program>> list;
        CHECK(programs(title, with_decomp, list, error));
        std::vector<std::string> names;
        for (const auto &[name, _] : list) names.push_back(name);
        CHECK(names == (std::vector<std::string>{"executable", "Battle", "Field", "decomp"}));
    }
    std::filesystem::remove(path);
}

void paths_leave_out_the_nameless_folders() {
    CHECK(join("", "") == "");
    CHECK(join("", "cro") == "cro");
    CHECK(join("cro", "") == "cro");
    CHECK(join("cro", "Battle.cro") == "cro/Battle.cro");
}

} // namespace

int main() {
    a_title_made_for_tests_reads_back();
    backwards_lzss_decompresses();
    pointers_in_data_become_functions();
    anonymous_imports_lead_into_the_other_module();
    relocations_fill_in_code_pointers();
    arm_function_jumps_with_ldr();
    thumb_function_switches_state_first();
    decomp_txt_is_read();
    marks_are_found_outside_comments();
    bad_marks_are_reported();
    the_hottest_functions_cover_the_share();
    the_executable_is_made_of_the_code_a_mod_gives();
    a_mods_exheader_says_how_much_of_its_code_is_text();
    the_modules_a_mod_replaces_come_from_it();
    paths_leave_out_the_nameless_folders();
    if (failures) std::printf("%d failed\n", failures);
    return failures ? 1 : 0;
}
