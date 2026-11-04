// The guest's state as locals of each generated function, the body text
// rewritten for them. Translated from 3dsrecomp's codegen/locals.rs (Rust,
// MIT, Copyright (c) 2026 fearkov; see LICENSE-3dsrecomp).
//
// keeping the guest's registers, flags and fpscr in locals for the length
// of a function, with the budget and the page tables. in the context,
// every store to guest memory, which goes through a byte pointer, makes the
// compiler read them all again, while locals it can hold in host registers.
//
// the context has to be complete whenever anything else can look at it, a
// call, the interpreter, a short vector, a return, an svc, a jump through
// dispatch, the end of the budget. recomp.h's SYNC_OUT stores the locals
// there, and SYNC_IN takes them all again after. SYNC_OUT only stores the
// locals that may have changed since the context last had them, which a
// pass over the function's lines works out, and each line that syncs gets
// the SYNC_OUT it needs defined before it. that keeps a call from storing
// every register the function ever writes, and lets the compiler drop the
// loads of the registers a function writes before it reads them.

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <optional>

#include "../rust.h"
#include "scope.h"

namespace recomp::codegen::locals {

const std::string_view RESET = "#undef SYNC_OUT\n"
                               "#undef SYNC_IN\n"
                               "#undef BUDGET_LEFT\n"
                               "#undef READ_PAGES\n"
                               "#undef WRITE_PAGES\n"
                               "#undef FPSCR\n"
                               "#define SYNC_OUT() do { } while (0)\n"
                               "#define SYNC_IN() do { } while (0)\n"
                               "#define BUDGET_LEFT ctx->budget\n"
                               "#define READ_PAGES(c) ((c)->read_pages)\n"
                               "#define WRITE_PAGES(c) ((c)->write_pages)\n"
                               "#define FPSCR(c) (*(c)->fpscr)\n";

namespace {

// the conditions recomp.h spells with the context's flags, spelled with
// the locals, and the flags each reads.
struct Condition {
    std::string_view name, expansion, flags;
};
constexpr Condition CONDITIONS[] = {
    {"C_EQ", "(flag_z_)", "z"},
    {"C_NE", "(!flag_z_)", "z"},
    {"C_CS", "(flag_c_)", "c"},
    {"C_CC", "(!flag_c_)", "c"},
    {"C_MI", "(flag_n_)", "n"},
    {"C_PL", "(!flag_n_)", "n"},
    {"C_VS", "(flag_v_)", "v"},
    {"C_VC", "(!flag_v_)", "v"},
    {"C_HI", "(flag_c_ && !flag_z_)", "cz"},
    {"C_LS", "(!flag_c_ || flag_z_)", "cz"},
    {"C_GE", "(flag_n_ == flag_v_)", "nv"},
    {"C_LT", "(flag_n_ != flag_v_)", "nv"},
    {"C_GT", "(!flag_z_ && flag_n_ == flag_v_)", "znv"},
    {"C_LE", "(flag_z_ || flag_n_ != flag_v_)", "znv"},
};

constexpr std::string_view ASSIGNMENTS[] = {"<<=", ">>=", "+=", "-=", "*=", "/=", "%=", "&=", "|=", "^=", "++", "--"};

// the words that make what follows them run only sometimes.
constexpr std::string_view GUARDS[] = {"if", "else", "switch", "case", "for", "while"};

// the locals are numbered r0 to r15, then the flags, then s0 to s31, then
// fpscr.
constexpr char FLAGS[4] = {'n', 'z', 'c', 'v'};
constexpr size_t FLAG = 16;
constexpr size_t VFP = 20;
constexpr size_t FPSCR = 52;

// a set of locals, a bit for each.
using Set = uint64_t;

// the bit of a local, a shift that wraps its amount the way Rust's does
// without overflow checks (the locals all fit)
inline Set bit(size_t index) { return Set(1) << (index & 63); }
inline Set bits(Set value, size_t index) { return value << (index & 63); }

// the ways out of a function for the budget, one for each of the most
// common sets of locals to store, and one more for all the others.
constexpr size_t EXITS = 16;

bool identifier(char byte) {
    return (byte >= '0' && byte <= '9') || (byte >= 'a' && byte <= 'z') || (byte >= 'A' && byte <= 'Z') || byte == '_';
}

bool digit(char byte) { return byte >= '0' && byte <= '9'; }

bool hex_digit(char byte) { return digit(byte) || (byte >= 'a' && byte <= 'f') || (byte >= 'A' && byte <= 'F'); }

// the local numbered index, its type, and where it lives in the context.
struct Local {
    std::string name;
    const char *kind;
    std::string home;
};

Local local(size_t index) {
    if (index < FLAG) return {std::format("reg{}_", index), "uint32_t", std::format("ctx->r[{}]", index)};
    if (index < VFP) {
        char name = FLAGS[index - FLAG];
        return {std::format("flag_{}_", name), "uint8_t", std::format("ctx->{}", name)};
    }
    if (index < FPSCR) return {std::format("vfp{}_", index - VFP), "uint32_t", std::format("ctx->vfp[{}]", index - VFP)};
    return {"fpscr_", "uint32_t", "*ctx->fpscr"};
}

// a SYNC_OUT that stores the locals in set, and the budget when the
// function counts one.
std::string sync_out(Set set, bool budget) {
    std::string stores;
    for (size_t index = 0; index <= FPSCR; index++) {
        if ((set & bit(index)) == 0) continue;
        Local l = local(index);
        stores += l.home;
        stores += " = ";
        stores += l.name;
        stores += "; ";
    }
    if (budget) stores += "ctx->budget = budget_; ";
    return std::format("#undef SYNC_OUT\n#define SYNC_OUT() do {{ {}}} while (0)\n", stores);
}

// how a local is used where it appears.
enum class Use {
    Read,
    // an assignment, which replaces the value.
    Assign,
    // a compound assignment, an increment or its address taken, which read
    // the value and may change it.
    Modify,
};

// how the token ending at end, and starting at start, is used.
Use usage(std::string_view text, size_t start, size_t end) {
    std::string_view after = text.substr(end);
    size_t skipped = 0;
    while (skipped < after.size() && after[skipped] == ' ') skipped++;
    after.remove_prefix(skipped);
    if (!after.empty() && after[0] == '=' && !(after.size() > 1 && after[1] == '=')) return Use::Assign;
    for (std::string_view op : ASSIGNMENTS) {
        if (after.starts_with(op)) return Use::Modify;
    }
    std::string_view before = text.substr(0, start);
    while (!before.empty() && before.back() == ' ') before.remove_suffix(1);
    bool address = !before.empty() && before.back() == '&' && !(before.size() >= 2 && before[before.size() - 2] == '&');
    if (address || before.ends_with("++") || before.ends_with("--")) return Use::Modify;
    return Use::Read;
}

// the number text starts with, and how many digits it has.
std::optional<std::pair<uint32_t, size_t>> number(std::string_view text) {
    size_t digits = 0;
    while (digits < text.size() && digit(text[digits])) digits++;
    std::optional<uint32_t> value = rust::parse_u32(text.substr(0, digits), 10);
    if (!value) return std::nullopt;
    return std::pair(*value, digits);
}

// the register number after prefix in text, and where what follows it
// starts, when text is prefix, a number and then close.
std::optional<std::pair<uint32_t, size_t>> indexed(std::string_view text, std::string_view prefix, std::string_view close) {
    if (!text.starts_with(prefix)) return std::nullopt;
    std::string_view rest = text.substr(prefix.size());
    auto found = number(rest);
    if (!found) return std::nullopt;
    auto [value, digits] = *found;
    if (!rest.substr(digits).starts_with(close)) return std::nullopt;
    return std::pair(value, prefix.size() + digits + close.size());
}

// what a line does that matters to which locals the context is behind on,
// and to which ones the code still reads, with whether a condition guards
// it so that it may not happen.
struct Event {
    enum Kind : uint8_t {
        // a local's value is used.
        Read,
        // a local takes a new value, the whole of it when whole.
        Write,
        // the end of a statement, the reads in it come before its writes.
        Boundary,
        // the context has to be complete for a call, the interpreter or a
        // short vector, and the locals are taken again after it.
        Sync,
        // leaving the function, by a return or an svc, with the context
        // complete.
        Leave,
        // a jump through dispatch, which syncs first, so the labels it
        // reaches start with the context complete.
        Dispatch,
        // a jump to one of the function's labels.
        Goto,
        // a block's budget check.
        Budget,
    } kind;
    bool guarded = false;
    bool whole = false;
    // the local a read or a write is of, and the line a goto's label is
    // on once the labels are known (NOWHERE when none is)
    size_t index = 0;
    // a goto's label, in the body's text
    std::string_view label;
};

constexpr size_t NOWHERE = SIZE_MAX;

// a line of the function, its locals in place, and what it does in order.
struct Line {
    std::string text;
    std::optional<std::string_view> label;
    std::vector<Event> events;
};

// where the scan stands in the function's blocks, whether a condition
// guards each open brace, and whether one guards the current statement.
struct Blocks {
    std::vector<bool> open;
    // how many of open are guarded
    size_t guarded_open = 0;
    bool statement = false;

    bool guarded() const { return statement || guarded_open > 0; }
    void push(bool guarded) {
        open.push_back(guarded);
        guarded_open += guarded;
    }
    void pop() {
        if (open.empty()) return;
        guarded_open -= open.back();
        open.pop_back();
    }
};

// a line with the registers, flags, VFP registers and fpscr it touches as
// locals, adding each to used.
Line scan(std::string_view line, Set &used, Blocks &blocks) {
    std::string_view text = line;
    std::string_view trimmed = rust::trim(line);
    std::optional<std::string_view> label;
    if (trimmed == "dispatch:") {
        label = std::string_view("dispatch");
    } else if (trimmed.starts_with("L_") && trimmed.ends_with(':') && trimmed.find(' ') == std::string_view::npos) {
        label = trimmed.substr(0, trimmed.size() - 1);
    }
    std::string out;
    out.reserve(line.size() + 16);
    std::vector<Event> events;
    size_t i = 0;
    size_t copied = 0;
    // puts with in place of the text from i to end, and goes on after it
    auto swap = [&](size_t end, std::string_view with) {
        out.append(line.substr(copied, i - copied));
        out.append(with);
        i = end;
        copied = end;
    };
    auto read = [&](size_t index) { events.push_back({Event::Read, blocks.guarded(), false, index, {}}); };
    auto assign = [&](size_t index) { events.push_back({Event::Write, blocks.guarded(), true, index, {}}); };
    // a local used as usage says
    auto touch = [&](size_t index, Use usage) {
        switch (usage) {
        case Use::Read: read(index); break;
        case Use::Assign: assign(index); break;
        case Use::Modify:
            read(index);
            events.push_back({Event::Write, blocks.guarded(), false, index, {}});
            break;
        }
    };
    auto push = [&](Event::Kind kind, bool guarded) { events.push_back({kind, guarded, false, 0, {}}); };
    while (i < text.size()) {
        std::string_view rest = text.substr(i);
        // the comment before each instruction, its address and opcode
        if (rest.starts_with("/*")) {
            size_t at = rest.find("*/");
            i += at == std::string_view::npos ? rest.size() : at + 2;
            continue;
        }
        switch (rest[0]) {
        case '{':
            push(Event::Boundary, false);
            blocks.push(blocks.statement);
            blocks.statement = false;
            break;
        case '}':
            push(Event::Boundary, false);
            blocks.pop();
            blocks.statement = false;
            break;
        case ';':
            push(Event::Boundary, false);
            blocks.statement = false;
            break;
        case '?': blocks.statement = true; break;
        default: break;
        }
        if (auto found = indexed(rest, "ctx->r[", "]")) {
            auto [n, length] = *found;
            used |= bit(n);
            touch(n, usage(text, i, i + length));
            swap(i + length, std::format("reg{}_", n));
            continue;
        }
        if (auto found = indexed(rest, "ctx->vfp[", "]")) {
            auto [n, length] = *found;
            used |= bit(VFP + n);
            touch(VFP + n, usage(text, i, i + length));
            swap(i + length, std::format("vfp{}_", n));
            continue;
        }
        if (rest.starts_with("*ctx->fpscr") && !(rest.size() > 11 && identifier(rest[11]))) {
            used |= bit(FPSCR);
            touch(FPSCR, usage(text, i, i + 11));
            swap(i + 11, "fpscr_");
            continue;
        }
        if (i > 0 && identifier(text[i - 1])) {
            i += 1;
            continue;
        }
        size_t word = 0;
        while (word < rest.size() && identifier(rest[word])) word++;
        std::string_view name = rest.substr(0, word);
        if (std::find(std::begin(GUARDS), std::end(GUARDS), name) != std::end(GUARDS)) blocks.statement = true;
        if (rest.starts_with("ctx->") && rest.size() > 5) {
            char flag_name = rest[5];
            bool single = !(rest.size() > 6 && identifier(rest[6]));
            const char *flag = std::find(std::begin(FLAGS), std::end(FLAGS), flag_name);
            if (flag != std::end(FLAGS) && single) {
                size_t index = FLAG + size_t(flag - FLAGS);
                used |= bit(index);
                touch(index, usage(text, i, i + 6));
                swap(i + 6, std::format("flag_{}_", flag_name));
                continue;
            }
        }
        const Condition *condition = std::find_if(std::begin(CONDITIONS), std::end(CONDITIONS),
                                                  [&](const Condition &c) { return c.name == name; });
        if (condition != std::end(CONDITIONS)) {
            for (char flag : condition->flags) {
                const char *at = std::find(std::begin(FLAGS), std::end(FLAGS), flag);
                size_t index = FLAG + size_t(at - FLAGS);
                used |= bit(index);
                read(index);
            }
            swap(i + word, condition->expansion);
            continue;
        }
        if (rest.starts_with("vfp_")) {
            // a double is two singles, 2N and 2N + 1
            if (auto found = indexed(rest, "vfp_s(ctx, ", ")")) {
                auto [n, length] = *found;
                used |= bit(VFP + n);
                read(VFP + n);
                swap(i + length, std::format("vfp_from_s(vfp{}_)", n));
                continue;
            }
            if (auto found = indexed(rest, "vfp_d(ctx, ", ")")) {
                auto [d, length] = *found;
                used |= bits(3, VFP + 2 * size_t(d));
                read(VFP + 2 * size_t(d));
                read(VFP + 2 * size_t(d) + 1);
                swap(i + length, std::format("vfp_from_d(vfp{}_, vfp{}_)", 2 * d, 2 * d + 1));
                continue;
            }
            // the value that follows closes what replaces the start, and
            // the flush reads fpscr
            if (auto found = indexed(rest, "vfp_set_s(ctx, ", ", ")) {
                auto [n, length] = *found;
                used |= bit(VFP + n) | bit(FPSCR);
                read(FPSCR);
                assign(VFP + n);
                swap(i + length, std::format("vfp{}_ = vfp_to_s(ctx, ", n));
                continue;
            }
            if (auto found = indexed(rest, "vfp_set_d(ctx, ", ", ")) {
                auto [d, length] = *found;
                used |= bits(3, VFP + 2 * size_t(d)) | bit(FPSCR);
                read(FPSCR);
                assign(VFP + 2 * size_t(d));
                assign(VFP + 2 * size_t(d) + 1);
                swap(i + length, std::format("VFP_SET_D(vfp{}_, vfp{}_, ", 2 * d, 2 * d + 1));
                continue;
            }
            if (rest.starts_with("vfp_vector(")) {
                push(Event::Sync, blocks.guarded());
                swap(i + 11, "VFP_VECTOR(");
                continue;
            }
            if (rest.starts_with("vfp_compare(")) {
                used |= bit(FPSCR);
                read(FPSCR);
                events.push_back({Event::Write, blocks.guarded(), false, FPSCR, {}});
            }
        }
        bool guarded = blocks.guarded();
        // interface 6's svc is a call that comes back, as CALL is
        if (name == "CALL" || name == "INTERPRET" || name == "SVC") {
            push(Event::Sync, guarded);
        } else if (name == "RETURN" || name == "RETURN_TO" || name == "RETURN_TO_A" || name == "RETURN_TO_T" ||
                   name == "JUMP_AWAY" || name == "TAIL_CALL") {
            push(Event::Leave, guarded);
        } else if (name == "JUMP_TO") {
            push(Event::Dispatch, guarded);
        } else if (name == "BLOCK") {
            push(Event::Budget, guarded);
        } else if (name == "goto" && rest.starts_with("goto dispatch;")) {
            push(Event::Dispatch, guarded);
            // one statement still, should it follow a bare if, and the
            // statement ends with it
            blocks.statement = false;
            swap(i + 14, "{ SYNC_OUT(); goto dispatch; }");
            continue;
        } else if (name == "goto" && rest.starts_with("goto L_")) {
            size_t length = 0;
            while (7 + length < rest.size() && hex_digit(rest[7 + length])) length++;
            events.push_back({Event::Goto, guarded, false, 0, line.substr(i + 5, 2 + length)});
        }
        i += std::max<size_t>(word, 1);
    }
    out.append(line.substr(copied));
    return Line{std::move(out), label, std::move(events)};
}

[[noreturn]] void fail(const char *what) {
    std::fprintf(stderr, "locals: %s\n", what);
    std::abort();
}

} // namespace

std::string keep(std::string_view body) {
    Set used = 0;
    Blocks blocks;
    std::vector<Line> lines;
    for (std::string_view line : rust::lines(body)) lines.push_back(scan(line, used, blocks));
    size_t count = lines.size();
    bool budget = false;
    for (const Line &line : lines) {
        for (const Event &event : line.events) budget |= event.kind == Event::Budget;
    }

    // which locals the context may be behind on where each line starts,
    // and what each line's syncs store. a jump to a label carries what it
    // was behind on there, dispatch starts with nothing behind, and a sync
    // that surely happens, nothing guarding it, leaves nothing behind.
    std::map<std::string_view, size_t> labels;
    for (size_t i = 0; i < count; i++) {
        if (lines[i].label) labels.insert_or_assign(*lines[i].label, i);
    }
    // the line each goto leads to, the labels being what they are from here
    for (Line &line : lines) {
        for (Event &event : line.events) {
            if (event.kind != Event::Goto) continue;
            auto found = labels.find(event.label);
            event.index = found == labels.end() ? NOWHERE : found->second;
        }
    }
    std::vector<Set> jumped(count, 0);
    std::vector<std::optional<Set>> stores(count);
    std::vector<std::optional<Set>> budgets(count);
    // whether execution can go on from the end of each line to the next
    std::vector<char> falls(count, 1);
    for (;;) {
        bool changed = false;
        // the function starts with the context complete
        std::optional<Set> falling = Set(0);
        for (size_t i = 0; i < count; i++) {
            const Line &line = lines[i];
            Set behind = falling.value_or(0) | jumped[i];
            std::optional<Set> syncs;
            bool ends = false;
            for (const Event &event : line.events) {
                switch (event.kind) {
                case Event::Read:
                case Event::Boundary: break;
                case Event::Write: behind |= bit(event.index); break;
                case Event::Sync:
                    syncs = syncs.value_or(0) | behind;
                    if (!event.guarded) behind = 0;
                    break;
                case Event::Leave:
                case Event::Dispatch:
                    syncs = syncs.value_or(0) | behind;
                    ends = !event.guarded;
                    break;
                case Event::Goto:
                    if (event.index != NOWHERE) {
                        Set &target = jumped[event.index];
                        changed |= (target | behind) != target;
                        target |= behind;
                    }
                    ends = !event.guarded;
                    break;
                case Event::Budget: budgets[i] = behind; break;
                }
                if (ends) break;
            }
            stores[i] = syncs;
            falls[i] = !ends;
            falling = ends ? std::nullopt : std::optional<Set>(behind);
        }
        if (!changed) break;
    }

    // which locals the code may still read where each line starts, going
    // backwards. everything that syncs reads them all, a jump reads what its
    // label does, and a write only ends a value's life when nothing guards
    // it and it replaces the whole of it. a budget check notes what is live
    // just after it, all its way out has to store.
    Set all = used;
    std::vector<Set> live_in(count, 0);
    std::vector<Set> live_at_budget(count, all);
    for (;;) {
        bool changed = false;
        for (size_t i = count; i-- > 0;) {
            const Line &line = lines[i];
            Set live = falls[i] ? (i + 1 < count ? live_in[i + 1] : 0) : 0;
            Set reads = 0, kills = 0;
            for (auto event = line.events.rbegin(); event != line.events.rend(); ++event) {
                // a statement's reads come before its writes
                if (event->kind != Event::Read && event->kind != Event::Write) {
                    live = (live & ~kills) | reads;
                    reads = 0, kills = 0;
                }
                switch (event->kind) {
                case Event::Read: reads |= bit(event->index); break;
                case Event::Write:
                    if (event->whole && !event->guarded) kills |= bit(event->index);
                    break;
                case Event::Boundary: break;
                case Event::Sync:
                case Event::Leave:
                case Event::Dispatch: live = all; break;
                case Event::Goto: {
                    Set there = event->index == NOWHERE ? all : live_in[event->index];
                    live = event->guarded ? live | there : there;
                    break;
                }
                case Event::Budget: live_at_budget[i] = live; break;
                }
            }
            live = (live & ~kills) | reads;
            if ((live_in[i] | live) != live_in[i]) {
                live_in[i] |= live;
                changed = true;
            }
        }
        if (!changed) break;
    }

    std::string prologue;
    std::vector<std::string> loads;
    for (size_t index = 0; index < FPSCR; index++) {
        if ((used & bit(index)) == 0) continue;
        Local l = local(index);
        emit(prologue, "    {} {} = {};", l.kind, l.name, l.home);
    }
    for (size_t index = 0; index < FPSCR; index++) {
        if ((used & bit(index)) == 0) continue;
        Local l = local(index);
        loads.push_back(std::format("{} = {};", l.name, l.home));
    }
    emit(prologue, "#undef SYNC_OUT\n#undef SYNC_IN");
    if ((used & bit(FPSCR)) != 0) {
        emit(prologue, "    uint32_t fpscr_ = *ctx->fpscr;\n#undef FPSCR\n#define FPSCR(c) fpscr_");
        loads.push_back("fpscr_ = *ctx->fpscr;");
    }
    if (budget) {
        emit(prologue, "    int32_t budget_ = ctx->budget;\n#undef BUDGET_LEFT\n#define BUDGET_LEFT budget_");
        loads.push_back("budget_ = ctx->budget;");
    }
    // the page tables only ever get read, and anything that could change
    // which ones the code uses is something it syncs around
    if (body.find("mem_") != std::string_view::npos) {
        emit(prologue, "    uint8_t *const *read_pages_ = ctx->read_pages, *const *write_pages_ = ctx->write_pages;");
        emit(prologue,
             "#undef READ_PAGES\n#undef WRITE_PAGES\n#define READ_PAGES(c) read_pages_\n#define WRITE_PAGES(c) write_pages_");
        loads.push_back("read_pages_ = ctx->read_pages; write_pages_ = ctx->write_pages;");
    }
    std::string joined;
    for (size_t i = 0; i < loads.size(); i++) {
        if (i) joined += ' ';
        joined += loads[i];
    }
    emit(prologue, "#define SYNC_IN() do {{ {} }} while (0)", joined);

    // a block the budget stops stores what is behind there and still read
    // from there on, r15 aside, which the way out sets. the blocks share a
    // way out for each such set, the most common ones, and the rest share
    // one that stores what any of them needs
    std::vector<std::optional<Set>> exits(count);
    for (size_t i = 0; i < count; i++) {
        if (budgets[i]) exits[i] = *budgets[i] & live_at_budget[i] & ~bit(15);
    }
    std::map<Set, size_t> counts;
    for (const auto &set : exits) {
        if (set) counts[*set] += 1;
    }
    std::vector<std::pair<Set, size_t>> common(counts.begin(), counts.end());
    std::stable_sort(common.begin(), common.end(), [](const auto &a, const auto &b) {
        if (a.second != b.second) return a.second > b.second;
        return a.first < b.first;
    });
    std::vector<Set> ways;
    for (size_t k = 0; k < common.size() && k < EXITS; k++) ways.push_back(common[k].first);
    Set rest = 0;
    for (size_t k = EXITS; k < common.size(); k++) rest |= common[k].first;
    if (common.size() > EXITS) ways.push_back(rest);
    auto way = [&](Set set) {
        auto found = std::find(ways.begin(), ways.end(), set);
        return found != ways.end() ? size_t(found - ways.begin()) : ways.size() - 1;
    };

    std::string out = std::move(prologue);
    Set defined = 0;
    out += sync_out(0, budget);
    for (size_t i = 0; i < count; i++) {
        const Line &line = lines[i];
        if (rust::trim(line.text) == "OUT_OF_BUDGET();") {
            for (size_t k = 0; k < ways.size(); k++) {
                out += sync_out(ways[k], budget);
                emit(out, "    OUT_OF_BUDGET_AT(out_of_budget_{});", k);
            }
            continue;
        }
        if (stores[i] && *stores[i] != defined) {
            out += sync_out(*stores[i], budget);
            defined = *stores[i];
        }
        if (exits[i]) {
            std::string_view text = rust::trim_end(line.text);
            if (!text.ends_with(");")) fail("a block's check");
            text.remove_suffix(2);
            std::string replaced(text);
            size_t at = replaced.find("BLOCK(");
            if (at != std::string::npos) replaced.replace(at, 6, "BLOCK_TO(");
            emit(out, "{}, out_of_budget_{});", replaced, way(*exits[i]));
        } else {
            out += line.text;
            out += '\n';
        }
    }
    return out;
}

} // namespace recomp::codegen::locals
