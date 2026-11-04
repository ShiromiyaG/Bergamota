// The code generator without a game: what a few instructions become, the
// tables saying what each function was made from, the overrides in place of
// what they replace, and locals::keep on bodies written by hand. The tests
// of 3dsrecomp's codegen/mod.rs and codegen/locals.rs (Rust, MIT, Copyright
// (c) 2026 fearkov; see LICENSE-3dsrecomp), translated; the ones that
// compile what they make use clang, with the flags 3dsrecomp's
// compile.rs gives.

#include "codegen/codegen.h"

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <format>
#include <fstream>

#include "codegen/scope.h"
#include "discover.h"
#include "rust.h"

#ifdef _WIN32
#include <process.h>
#define getpid _getpid
#else
#include <unistd.h>
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

using Files = std::map<std::string, std::string>;

constexpr uint32_t BASE = 0x00100000;

bool contains(const std::string &text, std::string_view what) { return text.find(what) != std::string::npos; }

Program program_of(const std::vector<uint32_t> &words) {
    Program program;
    program.text.base = BASE;
    for (uint32_t word : words) {
        for (int i = 0; i < 4; i++) program.text.bytes.push_back(uint8_t(word >> (8 * i)));
    }
    program.seeds = {{BASE, Source::Entry}};
    return program;
}

Files collect(std::vector<std::pair<std::string, std::string>> generated) {
    Files files;
    for (auto &[name, text] : generated) files.insert_or_assign(name, text);
    return files;
}

// the files for ARM code at BASE, entered at its start.
Files files(const std::vector<uint32_t> &words, const std::vector<Override> &overrides) {
    Program program = program_of(words);
    Analysis analysis = analyze(program);
    std::vector<Unit> units{{std::nullopt, &program, &analysis}};
    return collect(generate(units, overrides, Layout{}));
}

// the files for a function that calls another, the callee replaced.
Files generated(const std::vector<Override> &overrides) {
    // bl BASE + 8, bx lr, bx lr
    return files({0xEB000000, 0xE12FFF1E, 0xE12FFF1E}, overrides);
}

// compiles sources among files, in a folder of their own named after test,
// saying what the compiler said when they do not
void compiles(const char *test, const Files &files, const std::vector<std::string> &sources) {
    namespace fs = std::filesystem;
    fs::path dir = fs::temp_directory_path() / std::format("3dsrecomp-{}-{}", test, int(getpid()));
    std::error_code error;
    fs::create_directories(dir, error);
    for (auto &[name, text] : files) {
        std::ofstream file(dir / name, std::ios::binary);
        file.write(text.data(), std::streamsize(text.size()));
    }
    bool built = true;
    for (const std::string &source : sources) {
        std::string d = rust::display(dir);
        std::string command = std::format("clang -O2 -ffp-contract=off -fno-math-errno -w -I \"{}\" -c \"{}/{}\" -o \"{}/{}.o\"",
                                          d, d, source, d, source);
#ifdef _WIN32
        // cmd takes the outer quotes off
        command = "\"" + command + "\"";
#endif
        if (std::system(command.c_str()) != 0) {
            std::printf("%s: %s did not compile\n", test, source.c_str());
            built = false;
        }
    }
    fs::remove_all(dir, error);
    CHECK(built);
}

void multiple_transfers_check_one_page_for_all_their_words() {
    // push {r4, r5, lr}, vpush {d8}, vpop {d8}, pop {r4, r5, pc}
    Files files = ::files({0xE92D4030, 0xED2D8B02, 0xECBD8B02, 0xE8BD8030}, {});
    const std::string &code = files["code_0000.c"];
    CHECK(contains(code, "mem_write_span(ctx, p, 12u)"));
    CHECK(contains(code, "store32(span + 8, reg14_);"));
    CHECK(contains(code, "mem_write_span(ctx, p, 8u)"));
    CHECK(contains(code, "mem_read_span(ctx, p, 12u)"));
    CHECK(contains(code, "next = load32(span + 8);"));
    CHECK(contains(code, "RETURN_TO_A(next);"));

    compiles("transfers", files, {"code_0000.c"});
}

// every function starts by checking its stale flag, and the tables say
// what it was made from, a span for each run of its instructions and the
// hash of their bytes.
void functions_say_what_they_were_made_from() {
    // bl BASE + 8, bx lr, then the callee, bx lr
    Files files = generated({});
    const std::string &code = files["code_0000.c"];
    CHECK(contains(code, "if (LIKELY(target == recomp_starts[0])) goto L_00100000;\n    STALE_CHECK(recomp_stale[0])"));
    CHECK(contains(code, "STALE_CHECK(recomp_stale[1])"));
    CHECK(contains(files["entries.c"], "uint32_t recomp_starts[2] = {0x00100000u, 0x00100008u};"));
    const std::string &tables = files["entries.c"];
    codegen::CodeHash caller;
    const uint8_t bytes[] = {0x00, 0x00, 0x00, 0xEB, 0x1E, 0xFF, 0x2F, 0xE1};
    caller.add(bytes, sizeof bytes);
    CHECK(contains(tables, std::format("{{0x00100000u, 0x00100008u, 0x{:016X}ull}},", caller.value())));
    CHECK(contains(tables, "{0x00100008u, 0x0010000Cu, 0x"));
    // a function each, a run of one piece each
    CHECK(contains(tables, "static const Run origin_runs[] = {\n    {0, 1},\n    {1, 2},\n};"));
    CHECK(contains(tables, "RECOMP_EXPORT const Origins recomp_origins[] = {\n    {2, 2, origin_pieces, origin_functions, "
                           "origin_runs, origin_owners, recomp_stale, recomp_starts},"));

    compiles("origins", files, {"code_0000.c", "entries.c"});
}

// code several functions hold is cut where any of them starts or ends, and
// each function's code is whole pieces.
void overlapping_code_is_cut_into_pieces() {
    auto pieces = codegen::pieces({{0x100, 0x120}, {0x110, 0x118}, {0x118, 0x130}, {0x200, 0x204}});
    std::vector<std::pair<uint32_t, uint32_t>> expected{
        {0x100, 0x110}, {0x110, 0x118}, {0x118, 0x120}, {0x120, 0x130}, {0x200, 0x204}};
    CHECK(pieces == expected);
}

// each program's functions go in files of their own, which compile with
// what they declare.
void units_get_files_of_their_own_and_compile() {
    Program program = program_of({0xEB000000, 0xE12FFF1E, 0xE12FFF1E});
    Analysis analysis = analyze(program);
    std::vector<Unit> units{
        {std::nullopt, &program, &analysis},
        {"First", &program, &analysis},
        {"Second", &program, &analysis},
    };
    Files files = collect(generate(units, {}, Layout{}));
    CHECK(contains(files["code_0000.c"], "#include \"functions.h\"") && !contains(files["code_0000.c"], "m001_functions.h"));
    CHECK(contains(files["code_m001_0000.c"], "#include \"m001_functions.h\"") &&
          !contains(files["code_m001_0000.c"], "m002_functions.h"));
    const std::string &code = files["code_m002_0000.c"];
    CHECK(contains(code, "#include \"m002_functions.h\""));
    CHECK(contains(code, "STALE_CHECK(m002_recomp_stale[1])"));
    CHECK(contains(code, "if (LIKELY(target == (module_base + m002_recomp_starts[1])))"));
    CHECK(contains(files["entries.c"], "{2, 2, m002_origin_pieces, m002_origin_functions, m002_origin_runs, "
                                       "m002_origin_owners, m002_recomp_stale, m002_recomp_starts},"));

    compiles("units", files, {"code_0000.c", "code_m001_0000.c", "code_m002_0000.c", "entries.c"});
}

// a function keeps the number it had, one found anew takes the next free
// one, and numbers.txt reads back as written.
void functions_keep_their_numbers() {
    std::map<uint32_t, uint32_t> before{{0x100, 0}, {0x200, 1}, {0x300, 2}};
    auto after = codegen::numbered(&before, {0x100, 0x180, 0x300});
    CHECK((after == std::map<uint32_t, uint32_t>{{0x100, 0}, {0x180, 3}, {0x300, 2}}));
    auto fresh = codegen::numbered(nullptr, {0x10, 0x20});
    CHECK((fresh == std::map<uint32_t, uint32_t>{{0x10, 0}, {0x20, 1}}));
    std::string text = "- 00000100 0\nm001_ 00000200 5\n";
    auto read = Layout::read_numbers(text);
    CHECK(read[""][0x100] == 0);
    CHECK(read["m001_"][0x200] == 5);
}

Override replacing_the_callee() {
    Override replaced;
    replaced.module = std::nullopt;
    replaced.address = BASE + 8;
    replaced.name = "override_0x00100008";
    replaced.original = "original_0x00100008";
    return replaced;
}

void overrides_written_as_the_docs_show_compile() {
    Files files = generated({replacing_the_callee()});
    files.insert_or_assign("override.c", R"(#include "overrides.h"

RECOMP_OVERRIDE(0x00100008) {
    uint32_t start = ctx->r[0], end = start;
    BUDGET(0x00100008u, 4);
    while (mem_read8(ctx, end))
        end++;
    mem_write32(ctx, ctx->r[1], mem_read32(ctx, ctx->r[2]));
    vfp_set_s(ctx, 0, vfp_s(ctx, 1) * 2.0f);
    vfp_set_d(ctx, 1, vfp_d(ctx, 2) + 1.0);
    vfp_compare(ctx, vfp_d(ctx, 1), 0.0);
    ctx->r[0] = end - start;
    if (ctx->r[0] == 0) {
        CALL(RECOMP_ORIGINAL(0x00100008));
        return;
    }
    RETURN_TO(ctx->r[14]);
}
)");
    compiles("overrides", files, {"code_0000.c", "override.c"});
}

void overrides_take_the_place_of_what_they_replace() {
    Files files = generated({replacing_the_callee()});
    const std::string &code = files["code_0000.c"];
    CHECK(contains(code, "CALL(override_0x00100008);"));
    CHECK(contains(files["functions.h"], "void override_0x00100008(Context *ctx);"));
    CHECK(contains(files["entries.c"], "{0x00100008u, override_0x00100008},"));
    CHECK(contains(files["entries.c"], std::format("recomp_generation = {}u;", codegen::GENERATION)));
    const std::string &header = files["overrides.h"];
    CHECK(contains(header, "static inline void original_0x00100008(Context *ctx) {"));
    CHECK(contains(header, "f_00100008(ctx);"));

    // without overrides nothing changes
    Files plain = generated({});
    CHECK(contains(plain["code_0000.c"], "CALL(f_00100008);"));
    CHECK(!plain.contains("overrides.h"));
}

// locals.rs

using codegen::locals::keep;

void registers_and_flags_become_locals() {
    std::string body = "    uint32_t target = ctx->r[15];\n    ctx->r[4] = ctx->r[0] + 1;\n    if (C_GE) ctx->z = 1;\n    "
                       "shift_lsl(ctx->r[2], 3, &ctx->c);\n    ctx->ge = ctx->r[3];\n";
    std::string kept = keep(body);
    CHECK(contains(kept, "uint32_t target = reg15_;"));
    CHECK(contains(kept, "reg4_ = reg0_ + 1;"));
    CHECK(contains(kept, "if ((flag_n_ == flag_v_)) flag_z_ = 1;"));
    CHECK(contains(kept, "shift_lsl(reg2_, 3, &flag_c_);"));
    // other fields stay in the context
    CHECK(contains(kept, "ctx->ge = reg3_;"));
    CHECK(contains(kept, "reg0_ = ctx->r[0]; reg2_ = ctx->r[2]; reg3_ = ctx->r[3]; reg4_ = ctx->r[4]; reg15_ = ctx->r[15];"));
    CHECK(contains(kept, "uint8_t flag_v_ = ctx->v;"));
}

// the SYNC_OUT defined right before the first line containing what.
std::string sync_before(const std::string &kept, std::string_view what) {
    size_t at = kept.find(what);
    if (at == std::string::npos) return "(no such line)";
    size_t define = std::string_view(kept).substr(0, at).rfind("#define SYNC_OUT() ");
    if (define == std::string::npos) return "(no SYNC_OUT)";
    auto lines = rust::lines(std::string_view(kept).substr(define));
    return lines.empty() ? std::string() : std::string(lines[0]);
}

void a_call_stores_what_changed_since_the_last_sync() {
    std::string body = "L_00100000:\n    BLOCK(0x00100000u, 6);\n    ctx->r[4] = ctx->r[0] + 1;\n"
                       "    ctx->r[14] = 0x00100008u; ctx->r[15] = 0x00200000u;\n    CALL(f_00200000);\n    "
                       "RETURNED(0x00100008u);\n"
                       "    ctx->r[0] = ctx->r[4];\n    CALL(f_00300000);\n    RETURNED(0x0010000Cu);\n"
                       "    ctx->r[15] = ctx->r[14] & ~3u; RETURN();\n    OUT_OF_BUDGET();\n";
    std::string kept = keep(body);
    CHECK(sync_before(kept, "CALL(f_00200000)") == "#define SYNC_OUT() do { ctx->r[4] = reg4_; ctx->r[14] = reg14_; "
                                                    "ctx->r[15] = reg15_; ctx->budget = budget_; } while (0)");
    CHECK(sync_before(kept, "CALL(f_00300000)") ==
          "#define SYNC_OUT() do { ctx->r[0] = reg0_; ctx->budget = budget_; } while (0)");
    CHECK(sync_before(kept, "RETURN();") ==
          "#define SYNC_OUT() do { ctx->r[15] = reg15_; ctx->budget = budget_; } while (0)");
    // nothing changed yet where the budget is checked
    CHECK(contains(kept, "BLOCK_TO(0x00100000u, 6, out_of_budget_0);"));
    CHECK(sync_before(kept, "OUT_OF_BUDGET_AT(out_of_budget_0);") ==
          "#define SYNC_OUT() do { ctx->budget = budget_; } while (0)");
}

void a_guarded_call_may_not_happen() {
    std::string body = "    ctx->r[4] = 1;\n    if (C_EQ) {\n    CALL(f_00200000);\n    RETURNED(0x00100008u);\n    }\n"
                       "    ctx->r[0] = 2;\n    CALL(f_00200000);\n    RETURNED(0x0010000Cu);\n";
    std::string kept = keep(body);
    size_t second = kept.rfind("    CALL(f_00200000);");
    CHECK(second != std::string::npos);
    if (second == std::string::npos) return;
    size_t define = std::string_view(kept).substr(0, second).rfind("#define SYNC_OUT() ");
    CHECK(define != std::string::npos);
    if (define == std::string::npos) return;
    CHECK(rust::lines(std::string_view(kept).substr(define))[0] ==
          "#define SYNC_OUT() do { ctx->r[0] = reg0_; ctx->r[4] = reg4_; } while (0)");
}

void a_loop_carries_what_changed_back_to_its_label() {
    std::string body = "    uint32_t target = ctx->r[15], resume = 0;\n    if (LIKELY(target == 0x00100000u && "
                       "!ctx->thumb)) goto L_00100000;\n"
                       "L_00100000:\n    BLOCK(0x00100000u, 2);\n    /* 00100000 E2800001 */\n    ctx->r[0] = ctx->r[0] "
                       "+ 1;\n"
                       "    /* 00100004 EAFFFFFD */\n    goto L_00100000;\n    OUT_OF_BUDGET();\n";
    std::string kept = keep(body);
    CHECK(contains(kept, "    BLOCK_TO(0x00100000u, 2, out_of_budget_0);"));
    CHECK(sync_before(kept, "OUT_OF_BUDGET_AT(out_of_budget_0);") ==
          "#define SYNC_OUT() do { ctx->r[0] = reg0_; ctx->budget = budget_; } while (0)");
}

void the_budget_leaves_out_what_the_block_writes_before_reading() {
    // a loop whose compare sets the flags the next turn sets again first
    std::string body = "    uint32_t target = ctx->r[15], resume = 0;\n    if (LIKELY(target == 0x00100000u && "
                       "!ctx->thumb)) goto L_00100000;\n"
                       "L_00100000:\n    BLOCK(0x00100000u, 3);\n    ctx->r[0] = ctx->r[0] + 1;\n"
                       "    { uint32_t r = ctx->r[0] - 10u; ctx->n = r >> 31; ctx->z = r == 0; }\n"
                       "    if (C_NE) {\n    goto L_00100000;\n    }\n    ctx->r[15] = ctx->r[14] & ~3u; RETURN();\n    "
                       "OUT_OF_BUDGET();\n";
    std::string kept = keep(body);
    CHECK(contains(kept, "    BLOCK_TO(0x00100000u, 3, out_of_budget_0);"));
    CHECK(sync_before(kept, "OUT_OF_BUDGET_AT(out_of_budget_0);") ==
          "#define SYNC_OUT() do { ctx->r[0] = reg0_; ctx->budget = budget_; } while (0)");
    // the return still stores them
    CHECK(sync_before(kept, "RETURN();") == "#define SYNC_OUT() do { ctx->r[0] = reg0_; ctx->r[15] = reg15_; ctx->n = "
                                            "flag_n_; ctx->z = flag_z_; ctx->budget = budget_; } while (0)");
}

void a_jump_through_dispatch_syncs_first() {
    std::string kept = keep("    ctx->r[3] = 1; target = 0x00200000u; goto dispatch;\n");
    CHECK(contains(kept, "reg3_ = 1; target = 0x00200000u; { SYNC_OUT(); goto dispatch; }"));
    std::string guarded = keep("    if (C_EQ) goto dispatch;\n");
    CHECK(contains(guarded, "if ((flag_z_)) { SYNC_OUT(); goto dispatch; }"));
    CHECK(sync_before(kept, "goto dispatch") == "#define SYNC_OUT() do { ctx->r[3] = reg3_; } while (0)");
}

void the_budget_counts_down_in_a_local() {
    std::string kept = keep("L_00100000:\n    BLOCK(0x00100000u, 3);\n    ctx->r[0] = 1;\n    OUT_OF_BUDGET();\n");
    CHECK(contains(kept, "int32_t budget_ = ctx->budget;"));
    CHECK(contains(kept, "#define BUDGET_LEFT budget_"));
    CHECK(contains(kept, "budget_ = ctx->budget; } while (0)"));
    CHECK(!contains(keep("    ctx->r[0] = 1;\n"), "budget_"));
}

void vfp_registers_and_fpscr_become_locals() {
    std::string body = "    { float a = vfp_s(ctx, 3), b = vfp_s(ctx, 4); vfp_set_s(ctx, 3, a * b); }\n"
                       "    { double a = vfp_d(ctx, 1); vfp_set_d(ctx, 2, a + 1.0); }\n"
                       "    ctx->r[0] = ctx->vfp[7]; vfp_compare(ctx, vfp_d(ctx, 1), 0.0);\n"
                       "    if (UNLIKELY(*ctx->fpscr & FPSCR_LEN)) vfp_vector(ctx, 6, 0, 8, 9, 10); else { }\n";
    std::string kept = keep(body);
    CHECK(contains(kept, "float a = vfp_from_s(vfp3_), b = vfp_from_s(vfp4_); vfp3_ = vfp_to_s(ctx, a * b);"));
    CHECK(contains(kept, "double a = vfp_from_d(vfp2_, vfp3_); VFP_SET_D(vfp4_, vfp5_, a + 1.0);"));
    CHECK(contains(kept, "reg0_ = vfp7_; vfp_compare(ctx, vfp_from_d(vfp2_, vfp3_), 0.0);"));
    CHECK(contains(kept, "if (UNLIKELY(fpscr_ & FPSCR_LEN)) VFP_VECTOR(ctx, 6, 0, 8, 9, 10);"));
    CHECK(contains(kept, "uint32_t vfp7_ = ctx->vfp[7];"));
    CHECK(contains(kept, "uint32_t fpscr_ = *ctx->fpscr;"));
    CHECK(sync_before(kept, "VFP_VECTOR(") == "#define SYNC_OUT() do { ctx->r[0] = reg0_; ctx->vfp[3] = vfp3_; ctx->vfp[4] "
                                              "= vfp4_; ctx->vfp[5] = vfp5_; *ctx->fpscr = fpscr_; } while (0)");
}

void comparisons_are_not_writes() {
    std::string kept = keep("    if (ctx->r[1] == 0 && ctx->z != 1) ctx->r[2] = 0;\n    RETURN();\n");
    CHECK(sync_before(kept, "RETURN();") == "#define SYNC_OUT() do { ctx->r[2] = reg2_; } while (0)");
}

} // namespace

int main() {
    multiple_transfers_check_one_page_for_all_their_words();
    functions_say_what_they_were_made_from();
    overlapping_code_is_cut_into_pieces();
    units_get_files_of_their_own_and_compile();
    functions_keep_their_numbers();
    overrides_written_as_the_docs_show_compile();
    overrides_take_the_place_of_what_they_replace();
    registers_and_flags_become_locals();
    a_call_stores_what_changed_since_the_last_sync();
    a_guarded_call_may_not_happen();
    a_loop_carries_what_changed_back_to_its_label();
    the_budget_leaves_out_what_the_block_writes_before_reading();
    a_jump_through_dispatch_syncs_first();
    the_budget_counts_down_in_a_local();
    vfp_registers_and_fpscr_become_locals();
    comparisons_are_not_writes();
    if (failures) std::printf("%d failed\n", failures);
    return failures ? 1 : 0;
}
