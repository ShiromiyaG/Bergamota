// Turning the functions discovery found into C. Translated from 3dsrecomp's
// codegen/mod.rs (Rust, MIT, Copyright (c) 2026 fearkov; see
// LICENSE-3dsrecomp).
//
// every function becomes a C function that can be entered at any of its
// labels. guest calls are C calls, and anything the code cannot follow on
// its own, an svc, an unknown target, a full budget, returns all the way
// out to the host, which picks up again from r15.

#include "codegen.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <optional>
#include <tuple>

#include "../decode.h"
#include "../rust.h"
#include "scope.h"

namespace recomp {

namespace codegen {

std::string name(const std::string &prefix, uint32_t entry, bool thumb) {
    return std::format("{}{}_{:08X}", prefix, thumb ? 't' : 'f', entry);
}

bool call(std::string &out, const Scope &scope, uint32_t link, uint32_t target, bool thumb) {
    bool caller_thumb = (link & 1) != 0;
    std::string lr = caller_thumb ? std::format("{} | 1", scope.at(link & ~1u)) : scope.at(link);
    emit(out, "    ctx->r[14] = {}; ctx->r[15] = {};", lr, scope.at(target));
    if (thumb != caller_thumb) emit(out, "    ctx->thumb = {};", unsigned(thumb));
    if (const std::string *name = scope.function(target, thumb)) {
        emit(out, "    CALL({});", *name);
    } else {
        emit(out, "    CALL(recomp_call);");
    }
    const char *check = caller_thumb ? "RETURNED_T" : "RETURNED";
    emit(out, "    {}({});", check, scope.at(link & ~1u));
    return true;
}

// they nearly always lie on one page the code reaches directly, which one
// check covers, and otherwise go through the usual path one at a time.
void words(std::string &out, bool load, const std::vector<std::string> &words) {
    size_t bytes = words.size() * 4;
    if (words.size() == 1) {
        // nothing to share
        const std::string &word = words[0];
        if (load) {
            emit(out, "    {} = mem_read32(ctx, p);", word);
        } else {
            emit(out, "    mem_write32(ctx, p, {});", word);
        }
        return;
    }
    // the slow way, a word at a time, in one call out of line: written out
    // here it was most of what a transfer made the compiler work through
    size_t count = words.size();
    if (load) {
        emit(out, "    {{ const uint8_t *span = mem_read_span(ctx, p, {}u);", bytes);
        emit(out, "    if (LIKELY(span)) {{");
        for (size_t i = 0; i < words.size(); i++) emit(out, "    {} = load32(span + {});", words[i], i * 4);
        emit(out, "    }} else {{");
        emit(out, "    uint32_t words_[{}]; mem_read_words(ctx, READ_PAGES(ctx), p, words_, {}u);", count, count);
        for (size_t i = 0; i < words.size(); i++) emit(out, "    {} = words_[{}];", words[i], i);
    } else {
        emit(out, "    {{ uint8_t *span = mem_write_span(ctx, p, {}u);", bytes);
        emit(out, "    if (LIKELY(span)) {{");
        for (size_t i = 0; i < words.size(); i++) emit(out, "    store32(span + {}, {});", i * 4, words[i]);
        emit(out, "    }} else {{");
        std::string joined;
        for (size_t i = 0; i < words.size(); i++) {
            if (i) joined += ", ";
            joined += words[i];
        }
        emit(out, "    const uint32_t words_[{}] = {{{}}}; mem_write_words(ctx, WRITE_PAGES(ctx), p, words_, {}u);", count,
             joined, count);
    }
    emit(out, "    }} }}");
}

bool jump(std::string &out, const Scope &scope, uint32_t target, bool thumb) {
    if (scope.labels.contains(target)) {
        emit(out, "    goto L_{:08X};", target);
    } else if (const std::string *name = scope.function(target, thumb)) {
        // a tail call, which interface 6 makes one in C too
        emit(out, "    ctx->r[15] = {}; TAIL_CALL({});", scope.at(target), *name);
    } else {
        emit(out, "    target = {}; goto dispatch;", scope.at(target));
    }
    return false;
}

std::vector<std::pair<uint32_t, uint32_t>> pieces(std::vector<std::pair<uint32_t, uint32_t>> spans) {
    std::sort(spans.begin(), spans.end());
    std::set<uint32_t> cuts;
    for (auto [start, end] : spans) {
        cuts.insert(start);
        cuts.insert(end);
    }
    std::vector<std::pair<uint32_t, uint32_t>> pieces;
    std::optional<std::pair<uint32_t, uint32_t>> covered;
    // each stretch covered without a gap, cut at every start and end in it
    auto cut = [&](std::pair<uint32_t, uint32_t> stretch) {
        auto [start, end] = stretch;
        uint32_t from = start;
        for (auto at = cuts.upper_bound(start); at != cuts.end() && *at < end; ++at) {
            pieces.emplace_back(from, *at);
            from = *at;
        }
        pieces.emplace_back(from, end);
    };
    for (auto [start, end] : spans) {
        if (covered && start <= covered->second) {
            covered = std::pair(covered->first, std::max(covered->second, end));
        } else {
            if (covered) cut(*covered);
            covered = std::pair(start, end);
        }
    }
    if (covered) cut(*covered);
    return pieces;
}

std::map<uint32_t, uint32_t> numbered(const std::map<uint32_t, uint32_t> *before, const std::vector<uint32_t> &entries) {
    std::map<uint32_t, uint32_t> numbers;
    uint32_t next = 0;
    if (before && !before->empty()) {
        uint32_t max = 0;
        for (auto &[_, number] : *before) max = std::max(max, number);
        next = max + 1;
    }
    for (uint32_t entry : entries) {
        if (!before) continue;
        auto found = before->find(entry);
        if (found != before->end()) numbers.insert({entry, found->second});
    }
    for (uint32_t entry : entries) {
        if (numbers.contains(entry)) continue;
        next += 1;
        numbers.emplace(entry, next - 1);
    }
    return numbers;
}

namespace {

// a source file holds the functions starting in 32 KiB of a program's
// code, about 5,000 instructions where it is dense: the files compile in
// parallel and none gets too big for the compiler's memory (gcc keeps a
// whole file, and files of 20,000 took it up to 7.8 GB). a function found
// anew (by hints) only changes the file of its own stretch, so the others'
// objects are kept
constexpr uint32_t FILE_SHIFT = 15;

// a stretch whose code is denser goes on into a part after another every
// this many instructions: one file of 11 MB held a whole build up at its end
constexpr size_t FILE_SIZE = 5'000;

// the instructions up to which a function another one holds is still
// written as a function of its own.
constexpr size_t OWN_FUNCTION = 128;

[[noreturn]] void fail(const char *what) {
    std::fprintf(stderr, "codegen: %s\n", what);
    std::abort();
}

// an address as the host looks it up, bit 0 set for Thumb.
uint32_t key(uint32_t address, Mode mode) { return address | uint32_t(mode == Mode::Thumb); }

using Functions = std::map<uint32_t, const Function *>;

// the functions whose code another function already has, each with the
// entry of the function that runs it. a jump table's cases and the target
// of a tail call turn up both ways, as functions of their own and inside
// the function that reaches them.
std::map<uint32_t, uint32_t> containers(const Functions &functions) {
    std::map<uint32_t, std::vector<uint32_t>> owners;
    for (auto &[entry, function] : functions) {
        for (uint32_t label : function->labels) owners[label].push_back(entry);
    }
    // a function only moves into a bigger one, so the links cannot loop:
    // the rank is the size and then the lower entry
    auto size = [&](uint32_t entry) { return functions.at(entry)->instructions.size(); };
    auto outranks = [&](uint32_t a, uint32_t b) {
        size_t sa = size(a), sb = size(b);
        return sa > sb || (sa == sb && a < b);
    };
    std::map<uint32_t, uint32_t> parent;
    for (auto &[entry, function] : functions) {
        auto contains = [&](uint32_t other) {
            const std::vector<uint32_t> &code = functions.at(other)->instructions;
            return std::all_of(function->instructions.begin(), function->instructions.end(),
                               [&](uint32_t address) { return std::binary_search(code.begin(), code.end(), address); });
        };
        auto candidates = owners.find(entry);
        if (candidates == owners.end()) fail("a function without its entry among the labels");
        std::optional<uint32_t> best;
        for (uint32_t other : candidates->second) {
            if (functions.at(other)->mode != function->mode || !outranks(other, entry)) continue;
            if (!contains(other)) continue;
            // max_by_key keeps the last of the greatest
            if (!best || !outranks(*best, other)) best = other;
        }
        if (best) parent.emplace(entry, *best);
    }
    std::map<uint32_t, uint32_t> result;
    for (auto &[entry, first] : parent) {
        uint32_t container = first;
        for (auto next = parent.find(container); next != parent.end(); next = parent.find(container)) {
            container = next->second;
        }
        result.emplace(entry, container);
    }
    return result;
}

// the stretches of code a function was made from, as runs of instructions
// one right after another.
std::vector<std::pair<uint32_t, uint32_t>> spans(const Program &program, const Function &function) {
    std::vector<std::pair<uint32_t, uint32_t>> spans;
    for (uint32_t address : function.instructions) {
        uint32_t size = 4;
        if (function.mode == Mode::Thumb) {
            uint16_t op = program.text.read16(address).value_or(0);
            size = recomp::thumb::decode(op, program.text.read16(address + 2), address).second;
        }
        if (!spans.empty() && spans.back().second == address) {
            spans.back().second = address + size;
        } else {
            spans.emplace_back(address, address + size);
        }
    }
    return spans;
}

// a unit's stale flags and the tables saying what its functions were
// made from, with its Origins added to origins. the code is cut into pieces
// wherever a function's code starts or ends, so that the host hashes every
// byte once, functions holding the code of others as they do.
void write_origins(std::string &tables, std::string &origins, const std::string &prefix, const Program &program,
                   const Functions &functions, const std::map<uint32_t, uint32_t> &numbers, size_t count,
                   const std::map<uint32_t, std::pair<std::string, uint32_t>> &entries) {
    // by number: a number no function has now (it went since the build
    // before) starts nowhere and is made from nothing
    std::vector<std::optional<uint32_t>> by_number(count);
    for (auto &[entry, number] : numbers) by_number[number] = entry;
    emit(tables, "RECOMP_HIDDEN uint8_t {}recomp_stale[{}];", prefix, std::max<size_t>(count, 1));
    std::string starts;
    for (size_t i = 0; i < by_number.size(); i++) {
        if (i) starts += ", ";
        starts += std::format("0x{:08X}u", by_number[i].value_or(NO_START));
    }
    emit(tables, "RECOMP_HIDDEN uint32_t {}recomp_starts[{}] = {{{}}};", prefix, std::max<size_t>(count, 1), starts);
    std::vector<std::vector<std::pair<uint32_t, uint32_t>>> all;
    all.reserve(by_number.size());
    for (auto &entry : by_number) {
        all.push_back(entry ? spans(program, *functions.at(*entry)) : std::vector<std::pair<uint32_t, uint32_t>>{});
    }
    std::vector<std::pair<uint32_t, uint32_t>> flat;
    for (auto &spans : all) flat.insert(flat.end(), spans.begin(), spans.end());
    std::vector<std::pair<uint32_t, uint32_t>> pieces = codegen::pieces(std::move(flat));
    if (pieces.empty()) {
        emit(origins, "    {{0, 0, 0, 0, 0, 0, {}recomp_stale, {}recomp_starts}},", prefix, prefix);
        return;
    }

    emit(tables, "static const Piece {}origin_pieces[] = {{", prefix);
    auto at = [&](uint32_t address) { return size_t(address - program.text.base); };
    for (auto [start, end] : pieces) {
        CodeHash hash;
        size_t from = at(start), to = at(end);
        if (from <= to && to <= program.text.bytes.size()) hash.add(program.text.bytes.data() + from, to - from);
        emit(tables, "    {{0x{:08X}u, 0x{:08X}u, 0x{:016X}ull}},", start, end, hash.value());
    }
    emit(tables, "}};");
    std::string made;
    std::vector<std::pair<size_t, size_t>> runs;
    for (size_t i = 0; i < all.size(); i++) {
        uint32_t entry = by_number[i].value_or(NO_START);
        emit(made, "    {{{}, {}, 0x{:08X}u}},", runs.size(), all[i].size(), entry);
        for (auto [start, end] : all[i]) {
            auto below = [](uint32_t bound) {
                return [bound](const std::pair<uint32_t, uint32_t> &piece) { return piece.first < bound; };
            };
            size_t first = size_t(std::partition_point(pieces.begin(), pieces.end(), below(start)) - pieces.begin());
            size_t last = size_t(std::partition_point(pieces.begin(), pieces.end(), below(end)) - pieces.begin());
            runs.emplace_back(first, last);
        }
    }
    emit(tables, "static const Origin {}origin_functions[] = {{\n{}}};", prefix, made);
    emit(tables, "static const Run {}origin_runs[] = {{", prefix);
    for (auto [first, end] : runs) emit(tables, "    {{{}, {}}},", first, end);
    emit(tables, "}};");
    emit(tables, "static const uint32_t {}origin_owners[] = {{", prefix);
    for (auto &[_, owner] : entries) emit(tables, "    {}u,", owner.second);
    emit(tables, "}};\n");
    emit(origins,
         "    {{{}, {}, {}origin_pieces, {}origin_functions, {}origin_runs, {}origin_owners, {}recomp_stale, "
         "{}recomp_starts}},",
         count, pieces.size(), prefix, prefix, prefix, prefix, prefix, prefix);
}

// the labels a branch comes back to from at or after them: where the
// function's loops start, where interface 6's code counts its budget and
// lets the other threads run once it is spent (every cycle has such a
// branch, falling through only ever goes on)
std::set<uint32_t> loop_starts(const Program &program, const Function &function) {
    std::set<uint32_t> out;
    for (uint32_t address : function.instructions) {
        Instruction instruction;
        if (function.mode == Mode::Thumb) {
            uint16_t op = program.text.read16(address).value_or(0);
            std::optional<uint16_t> second = program.text.read16(address + 2);
            instruction = recomp::thumb::decode(op, second, address).first;
        } else {
            instruction = recomp::arm::decode(program.text.read32(address).value_or(0), address);
        }
        if (instruction.flow.kind != Flow::Branch || instruction.flow.link) continue;
        uint32_t target = instruction.flow.target;
        if (target <= address && function.labels.contains(target)) out.insert(target);
    }
    return out;
}

// what a function does, between its braces. inner: the entries of the
// functions it holds that have no C function of their own, which calls
// reach through its dispatch
void write_body(std::string &out, const Program &program, const Scope &scope, uint32_t entry,
                const Function &function, size_t number, const std::set<uint32_t> &inner) {
    bool thumb = function.mode == Mode::Thumb;
    const char *state = thumb ? "ctx->thumb" : "!ctx->thumb";
    if (scope.relative) emit(out, "    const uint32_t module_base = {}base;", scope.prefix);
    emit(out, "    uint32_t target = ctx->r[15], resume = 0;");
    // where the function returns to, and its stack then: a jump the code
    // cannot follow has the host run the rest until it gets there
    emit(out, "    const uint32_t enter_lr_ = ctx->r[14], enter_sp_ = ctx->r[13];");
    // a function is only ever entered in its own state, callers and the
    // host's lookups see to that, so the entry needs no look at thumb
    // its start comes from memory, where the host puts an address nothing
    // starts at once the function is stale, so that calls pay for the check
    // with nothing more than the compare they make anyway, and whatever
    // misses goes past the flag
    const std::string &prefix = scope.prefix;
    std::string start = scope.relative ? std::format("(module_base + {}recomp_starts[{}])", prefix, number)
                                       : std::format("{}recomp_starts[{}]", prefix, number);
    emit(out, "    if (LIKELY(target == {})) goto L_{:08X};", start, entry);
    emit(out, "    STALE_CHECK({}recomp_stale[{}])", prefix, number);
    emit(out, "dispatch:");
    const char *offset = scope.relative ? "target - module_base" : "target";
    emit(out, "    if ({}) switch ({}) {{", state, offset);
    // interface 6 never enters a function partway: only the labels an
    // address the code computes leads to, a table's, and the functions it
    // holds
    std::set<uint32_t> dispatched = function.tables;
    dispatched.insert(inner.begin(), inner.end());
    dispatched.insert(entry);
    for (uint32_t label : dispatched) {
        if (function.labels.contains(label)) emit(out, "    case 0x{:08X}u: goto L_{:08X};", label, label);
    }
    emit(out, "    }}");
    // somewhere else: the host runs it until the function would have
    // returned
    emit(out, "    ctx->r[15] = target;");
    emit(out, "    JUMP_AWAY(enter_lr_, enter_sp_);");

    const std::vector<uint32_t> &instructions = function.instructions;
    std::set<uint32_t> loops = loop_starts(program, function);
    for (size_t i = 0; i < instructions.size(); i++) {
        uint32_t address = instructions[i];
        if (function.labels.contains(address)) {
            size_t run = 1;
            for (size_t j = i + 1; j < instructions.size() && !function.labels.contains(instructions[j]); j++) run++;
            emit(out, "L_{:08X}:", address);
            // the budget looked at where loops start and where the function
            // does, counted everywhere else
            if (address == entry || loops.contains(address)) emit(out, "    BLOCK({}, {});", scope.at(address), run);
            else emit(out, "    BLOCK_COUNT({});", run);
        }
        bool continues;
        uint32_t size;
        if (thumb) {
            uint32_t op = program.text.read16(address).value_or(0);
            uint32_t second = program.text.read16(address + 2).value_or(0);
            size = recomp::thumb::decode(uint16_t(op), std::optional<uint16_t>(uint16_t(second)), address).second;
            emit(out, "    /* {:08X} {:04X} */", address, op);
            continues = thumb::lower(out, scope, address, op, second);
        } else {
            uint32_t op = program.text.read32(address).value_or(0);
            emit(out, "    /* {:08X} {:08X} */", address, op);
            continues = arm::lower(out, scope, address, op);
            size = 4;
        }
        uint32_t next = address + size;
        if (continues && !(i + 1 < instructions.size() && instructions[i + 1] == next)) {
            if (function.labels.contains(next)) emit(out, "    goto L_{:08X};", next);
            else emit(out, "    target = {}; goto dispatch;", scope.at(next));
        }
    }
    emit(out, "    OUT_OF_BUDGET();");
}

// a function, the number-th of its unit.
void write_function(std::string &out, const Program &program, const Scope &scope, uint32_t entry,
                    const Function &function, size_t number,
                    const std::set<uint32_t> &inner) {
    bool thumb = function.mode == Mode::Thumb;
    emit(out, "void {}(Context *ctx) {{", name(scope.prefix, entry, thumb));
    std::string body;
    write_body(body, program, scope, entry, function, number, inner);
    out += locals::keep(body);
    emit(out, "}}");
    out += locals::RESET;
    out += '\n';
}

} // namespace

} // namespace codegen

using namespace codegen;

std::map<std::string, std::map<uint32_t, uint32_t>> Layout::read_numbers(const std::string &text) {
    std::map<std::string, std::map<uint32_t, uint32_t>> numbers;
    for (std::string_view line : rust::lines(text)) {
        std::vector<std::string_view> fields = rust::split_whitespace(line);
        if (fields.size() != 3) continue;
        std::optional<uint32_t> entry = rust::parse_u32(fields[1], 16), number = rust::parse_u32(fields[2], 10);
        if (!entry || !number) continue;
        std::string prefix = fields[0] == "-" ? std::string() : std::string(fields[0]);
        numbers[prefix].insert_or_assign(*entry, *number);
    }
    return numbers;
}

bool recompiles(uint32_t entry, const Function &function) { return function.labels.contains(entry); }

std::vector<std::pair<std::string, std::string>> generate(const std::vector<Unit> &units,
                                                          const std::vector<Override> &overrides,
                                                          const Layout &layout) {
    std::vector<std::pair<std::string, std::string>> files;
    files.emplace_back("recomp.h", HEADER);
    std::vector<std::pair<std::string, std::string>> sources;
    std::string numbers_file;
    std::string tables = "#include \"recomp.h\"\n";
    std::string modules;
    // each unit's Origins, the executable's first
    std::string origins;
    std::string originals;
    std::set<std::string> original_headers;

    for (size_t index = 0; index < units.size(); index++) {
        const Unit &unit = units[index];
        const Program &program = *unit.program;
        std::string prefix = unit.module ? std::format("m{:03}_", index) : std::string();
        std::string header = std::format("{}functions.h", prefix);
        Functions functions;
        for (auto &[entry, f] : unit.analysis->functions) {
            if (recompiles(entry, f)) functions.emplace(entry, &f);
        }
        // a small function another one holds still gets a C function of its
        // own, so that calls to it skip the bigger one's way in, its loads
        // and its dispatch
        std::map<uint32_t, uint32_t> containers;
        for (auto &[entry, container] : codegen::containers(functions)) {
            if (functions.at(entry)->instructions.size() > OWN_FUNCTION) containers.emplace(entry, container);
        }
        std::map<uint32_t, std::string> names;
        for (auto &[entry, f] : functions) {
            auto found = containers.find(entry);
            uint32_t home = found != containers.end() ? found->second : entry;
            names.insert_or_assign(key(entry, f->mode), name(prefix, home, f->mode == Mode::Thumb));
        }
        std::erase_if(functions, [&](const auto &item) { return containers.contains(item.first); });
        // what each function holds that has no C function of its own
        std::map<uint32_t, std::set<uint32_t>> inner;
        for (auto &[entry, home] : containers) inner[home].insert(entry);

        // calls go to the functions written by hand, which reach the
        // generated ones through a function that enters them where they
        // replace them
        std::vector<const Override *> replaced;
        for (const Override &o : overrides) {
            if (o.module == unit.module) replaced.push_back(&o);
        }
        for (const Override *item : replaced) {
            auto found = names.find(item->address);
            if (found == names.end()) {
                names.emplace(item->address, item->name);
                continue;
            }
            std::string original = std::exchange(found->second, item->name);
            uint32_t address = item->address & ~1u;
            std::string at = unit.module ? std::format("{}base + 0x{:X}u", prefix, address) : std::format("0x{:08X}u", address);
            emit(originals, "static inline void {}(Context *ctx) {{", item->original);
            emit(originals, "    ctx->r[15] = {};", at);
            emit(originals, "    ctx->thumb = {};", item->address & 1);
            emit(originals, "    {}(ctx);", original);
            emit(originals, "}}\n");
            original_headers.insert(header);
        }

        std::string prototypes;
        if (unit.module) emit(prototypes, "extern uint32_t {}base;", prefix);
        emit(prototypes, "extern RECOMP_HIDDEN uint8_t {}recomp_stale[];", prefix);
        emit(prototypes, "extern RECOMP_HIDDEN uint32_t {}recomp_starts[];", prefix);
        for (const Override *item : replaced) emit(prototypes, "void {}(Context *ctx);", item->name);
        for (auto &[entry, function] : functions) {
            emit(prototypes, "void {}(Context *ctx);", name(prefix, entry, function->mode == Mode::Thumb));
        }
        files.emplace_back(header, std::move(prototypes));

        std::string include = std::format("#include \"{}\"\n\n", header);
        std::vector<uint32_t> keys;
        keys.reserve(functions.size());
        for (auto &[entry, _] : functions) keys.push_back(entry);
        auto before = layout.numbers.find(prefix);
        std::map<uint32_t, uint32_t> numbers =
            numbered(before == layout.numbers.end() ? nullptr : &before->second, keys);
        size_t count = 0;
        for (auto &[_, number] : numbers) count = std::max(count, size_t(number) + 1);
        for (auto &[entry, number] : numbers) {
            emit(numbers_file, "{} {:08X} {}", prefix.empty() ? "-" : prefix, entry, number);
        }
        const std::set<uint32_t> *hot = layout.hot && index < layout.hot->size() ? &(*layout.hot)[index] : nullptr;
        // each file's C, by whether it is hot, the stretch of code and the
        // part of it; and how far each stretch's last part has got
        std::map<std::tuple<bool, uint32_t, uint32_t>, std::string> files_of;
        std::map<std::pair<bool, uint32_t>, std::pair<uint32_t, size_t>> filled;
        bool relative = unit.module.has_value();
        for (auto &[entry, function] : functions) {
            size_t number = numbers.at(entry);
            uint32_t stretch = (entry - program.text.base) >> FILE_SHIFT;
            bool is_hot = hot && hot->contains(entry);
            auto &part = filled.try_emplace(std::pair(is_hot, stretch), 0u, size_t(0)).first->second;
            if (part.second >= FILE_SIZE) part = {part.first + 1, 0};
            part.second += function->instructions.size();
            auto source = files_of.find(std::tuple(is_hot, stretch, part.first));
            if (source == files_of.end()) {
                source = files_of.emplace(std::tuple(is_hot, stretch, part.first), "#include \"recomp.h\"\n" + include).first;
            }
            // interface 6: nothing comes back in after what the interpreter
            // runs, the code goes on (or unwinds), so no labels after it
            std::vector<uint32_t> interpreted;
            static const std::set<uint32_t> none;
            auto held = inner.find(entry);
            Scope scope{function->labels, names, prefix, relative, interpreted};
            write_function(source->second, program, scope, entry, *function, number,
                           held != inner.end() ? held->second : none);
        }
        // each program's code in files of its own, named after it and the
        // stretch, so that functions found in one leave the others' as they
        // were, and their objects with them
        for (auto &[place, source] : files_of) {
            auto [is_hot, stretch, part] = place;
            const char *kind = is_hot ? "hot" : "code";
            std::string name = part == 0 ? std::format("{}_{}{:04}.c", kind, prefix, stretch)
                                         : std::format("{}_{}{:04}_{}.c", kind, prefix, stretch, part);
            sources.emplace_back(std::move(name), std::move(source));
        }

        // where the host enters the code (interface 6: only where functions
        // start, the ones held inside another through it)
        std::map<uint32_t, std::pair<std::string, uint32_t>> entries;
        for (auto &[entry, function] : functions) {
            std::pair<std::string, uint32_t> owner(name(prefix, entry, function->mode == Mode::Thumb), numbers.at(entry));
            entries.insert_or_assign(key(entry, function->mode), owner);
            auto held = inner.find(entry);
            if (held == inner.end()) continue;
            for (uint32_t label : held->second) entries.try_emplace(key(label, function->mode), owner);
        }
        for (const Override *item : replaced) entries.insert_or_assign(item->address, std::pair(item->name, NO_ORIGIN));
        tables += include;
        std::string table;
        if (unit.module) {
            emit(tables, "uint32_t {}base;", prefix);
            emit(modules, "    {{\"{}\", &{}base, 0x{:X}u, {}, {}entries}},", *unit.module, prefix, program.text.end(),
                 entries.size(), prefix);
            table = std::format("static const Entry {}entries[]", prefix);
        } else {
            emit(tables, "RECOMP_EXPORT const uint32_t recomp_entry_count = {};", entries.size());
            table = "RECOMP_EXPORT const Entry recomp_entries[]";
        }
        emit(tables, "{} = {{", table);
        for (auto &[label, owner] : entries) emit(tables, "    {{0x{:08X}u, {}}},", label, owner.first);
        emit(tables, "}};\n");
        write_origins(tables, origins, prefix, program, functions, numbers, count, entries);
    }

    emit(tables, "RECOMP_EXPORT const uint32_t recomp_abi = RECOMP_ABI;");
    emit(tables, "RECOMP_EXPORT const uint32_t recomp_generation = {}u;", GENERATION);
    emit(tables, "RECOMP_EXPORT const uint32_t recomp_module_count = {};", units.size() - 1);
    emit(tables, "RECOMP_EXPORT const Module recomp_modules[] = {{\n{}}};", modules);
    emit(tables, "RECOMP_EXPORT const Origins recomp_origins[] = {{\n{}}};", origins);
    files.emplace_back("entries.c", std::move(tables));
    if (!overrides.empty()) {
        std::string header = "/* what overrides include, see docs/overrides.md. */\n\n#include \"recomp.h\"\n";
        for (const std::string &included : original_headers) emit(header, "#include \"{}\"", included);
        emit(header, "\n/* the generated functions the overrides replace. */\n\n{}", originals);
        files.emplace_back("overrides.h", std::move(header));
    }
    files.emplace_back("numbers.txt", std::move(numbers_file));
    for (auto &source : sources) files.push_back(std::move(source));
    return files;
}

} // namespace recomp
