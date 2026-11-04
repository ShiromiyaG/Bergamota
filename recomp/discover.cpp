// Finding the code in a program: functions from the entry points by
// following calls and branches, and from words elsewhere that point into the
// code, which is where vtables and callbacks live. Translated from
// 3dsrecomp's discover.rs (Rust, MIT, Copyright (c) 2026 fearkov; see
// LICENSE-3dsrecomp).

#include "discover.h"

#include <algorithm>
#include <cstddef>
#include <deque>
#include <tuple>

#include "decode.h"

namespace recomp {

size_t Analysis::count(Byte kind) const { return size_t(std::count(map.begin(), map.end(), kind)); }

std::vector<std::pair<uint32_t, size_t>> Analysis::largest_gaps(uint32_t base, size_t count) const {
    std::vector<std::pair<uint32_t, size_t>> gaps;
    std::optional<size_t> start;
    // the map and one code byte after it, so a gap at the end is closed
    for (size_t offset = 0; offset <= map.size(); offset++) {
        Byte byte = offset < map.size() ? map[offset] : Byte::Code;
        bool unknown = byte == Byte::Unknown;
        if (unknown && !start) {
            start = offset;
        } else if (!unknown && start) {
            gaps.emplace_back(base + uint32_t(*start), offset - *start);
            start.reset();
        }
    }
    std::stable_sort(gaps.begin(), gaps.end(), [](const auto &a, const auto &b) { return a.second > b.second; });
    if (gaps.size() > count) gaps.resize(count);
    return gaps;
}

namespace {

Mode other(Mode mode) { return mode == Mode::Arm ? Mode::Thumb : Mode::Arm; }

// instructions past which a function grew by following tail calls into
// other functions rather than by what one function holds. compilers make
// few near it, Monster Hunter 3 Ultimate's tail calls led one function
// through 62,000 instructions of others, which a C compiler then spent
// most of a build on.
constexpr size_t HUGE_FUNCTION = 10'000;

// how far a branch of a huge function goes before it is taken for a tail
// call, a function's own branches stay closer.
constexpr uint32_t FAR = 0x4000;

using Item = std::tuple<uint32_t, Mode, Source>;

uint32_t abs_diff(uint32_t a, uint32_t b) { return a > b ? a - b : b - a; }

struct Discovery {
    const Segment *text;
    const std::set<uint32_t> *slots; // null without them
    Analysis analysis;
    // function entries a branch goes to as a tail call rather than as more
    // of the function, while huge ones are explored again
    std::optional<std::set<uint32_t>> entries;
    std::deque<Item> queue;
    // pointers found in data, followed only once nothing surer is left
    std::deque<Item> guesses;

    std::vector<std::pair<uint32_t, Mode>> huge_functions() const {
        std::vector<std::pair<uint32_t, Mode>> huge;
        for (const auto &[entry, function] : analysis.functions)
            if (function.instructions.size() > HUGE_FUNCTION) huge.emplace_back(entry, function.mode);
        return huge;
    }

    // explores the huge functions again, now that every function is known,
    // their branches to other functions' entries and far away going to
    // functions there as tail calls instead of taking their code in, and
    // the functions those are explored the same way.
    void split_huge() {
        std::vector<std::pair<uint32_t, Mode>> huge = huge_functions();
        if (huge.empty()) return;
        // what exploring counts was counted the first time
        auto counts = std::make_tuple(analysis.indirect_sites, analysis.jump_tables, analysis.svc_sites,
                                      analysis.dead_ends);
        entries.emplace();
        for (const auto &[entry, function] : analysis.functions) entries->insert(entries->end(), entry);
        for (int round = 0; round < 4; round++) {
            for (const auto &[entry, mode] : huge) {
                auto [instructions, labels] = explore(entry, mode);
                auto function = analysis.functions.find(entry);
                if (function != analysis.functions.end()) {
                    function->second.instructions = std::move(instructions);
                    function->second.labels = std::move(labels);
                    function->second.tables = std::move(tables);
                }
            }
            // the functions the tail calls lead to, explored the same way
            run();
            std::vector<std::pair<uint32_t, Mode>> still = huge_functions();
            if (still.empty() || still == huge) break;
            huge = std::move(still);
        }
        entries.reset();
        std::tie(analysis.indirect_sites, analysis.jump_tables, analysis.svc_sites, analysis.dead_ends) = counts;
    }

    void run() {
        for (;;) {
            Item item;
            if (!queue.empty()) {
                item = queue.front();
                queue.pop_front();
            } else if (!guesses.empty()) {
                item = guesses.front();
                guesses.pop_front();
            } else {
                break;
            }
            auto [entry, mode, source] = item;
            if (!analysis.functions.contains(entry)) {
                auto [instructions, labels] = explore(entry, mode);
                Function function;
                function.source = source;
                function.mode = mode;
                function.instructions = std::move(instructions);
                function.labels = std::move(labels);
                function.tables = std::move(tables);
                analysis.functions.emplace(entry, std::move(function));
            }
        }
    }

    void mark(uint32_t address, uint32_t size, Byte kind) {
        uint32_t end = address + size;
        for (uint32_t byte = address; byte < end; byte++) {
            if (byte < text->base) continue;
            uint32_t offset = byte - text->base;
            if (offset < analysis.map.size()) {
                Byte &slot = analysis.map[offset];
                // code wins over a literal guess
                if (slot != Byte::Code) slot = kind;
            }
        }
    }

    Byte byte(uint32_t address) const { return analysis.map[size_t(address - text->base)]; }

    // likely ARM functions in the text nothing reached, the first word of
    // each gap when it is an unconditional instruction, and every push of
    // registers onto the stack. Thumb is left alone, too much data looks
    // like it.
    std::vector<uint32_t> gap_starts() const {
        std::vector<uint32_t> starts;
        bool in_gap = false;
        uint32_t address = (text->base + 3) & ~3u;
        while (auto word = text->read32(address)) {
            auto offset = std::ptrdiff_t(address - text->base);
            bool unknown = std::all_of(analysis.map.begin() + offset, analysis.map.begin() + offset + 4,
                                       [](Byte byte) { return byte == Byte::Unknown; });
            if (unknown && !analysis.functions.contains(address)) {
                Instruction instruction = arm::decode(*word, address);
                bool push = (*word & 0xFFFF0000) == 0xE92D0000 && (*word & 0xFFFF) != 0;
                bool starts_gap = !in_gap && instruction.condition == ALWAYS && instruction.flow.kind != Flow::Undefined;
                if (push || starts_gap) starts.push_back(address);
            }
            in_gap = unknown;
            address += 4;
        }
        return starts;
    }

    // whether the word at address may hold a code address
    bool may_point(uint32_t address) const { return !slots || slots->contains(address); }

    // a value that may be the address of a function, odd for Thumb
    void code_pointer(uint32_t value, Source source) {
        uint32_t address;
        Mode mode;
        if ((value & 1) != 0) {
            address = value & ~1u;
            mode = Mode::Thumb;
        } else if ((value & 3) == 0) {
            address = value;
            mode = Mode::Arm;
        } else {
            return;
        }
        if (text->contains(address) && !analysis.functions.contains(address)) {
            if (source == Source::Pointer)
                guesses.emplace_back(address, mode, source);
            else
                queue.emplace_back(address, mode, source);
        }
    }

    void call(uint32_t target, Mode mode) {
        if (text->contains(target) && !analysis.functions.contains(target))
            queue.emplace_back(target, mode, Source::Call);
    }

    std::optional<std::pair<Instruction, uint32_t>> decode(uint32_t address, Mode mode) const {
        if (mode == Mode::Arm) {
            auto word = text->read32(address);
            if (!word) return std::nullopt;
            return std::pair<Instruction, uint32_t>(arm::decode(*word, address), 4);
        }
        auto halfword = text->read16(address);
        if (!halfword) return std::nullopt;
        return thumb::decode(*halfword, text->read16(address + 2), address);
    }

    // follows every path through one function, returning its instructions
    // and labels.
    // the jump tables' targets of the function explore last went through,
    // as its labels
    std::set<uint32_t> tables;

    std::pair<std::vector<uint32_t>, std::set<uint32_t>> explore(uint32_t entry, Mode mode) {
        tables.clear();
        std::vector<uint32_t> blocks{entry};
        std::set<uint32_t> seen;
        std::set<uint32_t> code;
        std::set<uint32_t> labels;

        while (!blocks.empty()) {
            uint32_t start = blocks.back();
            blocks.pop_back();
            labels.insert(start);
            uint32_t address = start;
            while (text->contains(address) && seen.insert(address).second) {
                if (byte(address) == Byte::Literal) {
                    // ran into a literal pool, the path is wrong or it ended
                    break;
                }
                auto decoded = decode(address, mode);
                if (!decoded) break;
                auto [instruction, size] = *decoded;
                const Flow &flow = instruction.flow;
                if (flow.kind == Flow::Undefined) {
                    analysis.dead_ends++;
                    break;
                }
                mark(address, size, Byte::Code);
                code.insert(address);
                bool conditional = instruction.is_conditional();

                bool returns = (flow.kind == Flow::Branch && flow.link) || flow.kind == Flow::CallOtherMode ||
                               (flow.kind == Flow::BranchRegister && flow.link) || flow.kind == Flow::Svc;
                if (returns) {
                    // execution comes back after a call or an svc
                    labels.insert(address + size);
                }
                bool stop = false;
                switch (flow.kind) {
                case Flow::Branch:
                    if (flow.link) {
                        call(flow.target, mode);
                    } else {
                        uint32_t target = flow.target;
                        bool far = target < entry || abs_diff(target, address) > FAR;
                        bool tail_call = target != entry && entries && (entries->contains(target) || far);
                        if (tail_call && text->contains(target)) {
                            if (entries) entries->insert(target);
                            call(target, mode);
                        }
                        if (text->contains(target) && !tail_call) blocks.push_back(target);
                        stop = !conditional;
                    }
                    break;
                case Flow::CallOtherMode: call(flow.target, other(mode)); break;
                case Flow::BranchRegister:
                    analysis.indirect_sites++;
                    if (!flow.link) stop = !conditional;
                    break;
                case Flow::IndirectJump:
                    analysis.indirect_sites++;
                    stop = !conditional;
                    break;
                case Flow::Return: stop = !conditional; break;
                case Flow::JumpTable:
                    branch_table(address, blocks);
                    stop = true;
                    break;
                case Flow::AddressTable:
                    address_table(address, flow.index, blocks);
                    stop = !conditional;
                    break;
                case Flow::LoadPcLiteral: {
                    uint32_t literal = flow.literal;
                    mark(literal, 4, Byte::Literal);
                    auto target = text->read32(literal);
                    if (target && may_point(literal)) code_pointer(*target, Source::Pointer);
                    stop = !conditional;
                    break;
                }
                case Flow::LiteralLoad: {
                    uint32_t literal = flow.literal;
                    // a load from something already decoded as code is
                    // left alone
                    if (text->contains(literal) && byte(literal) != Byte::Code) {
                        mark(literal, flow.size, Byte::Literal);
                        if (flow.size == 4 && may_point(literal)) {
                            if (auto value = text->read32(literal)) code_pointer(*value, Source::Pointer);
                        }
                    }
                    break;
                }
                case Flow::Svc: analysis.svc_sites++; break;
                case Flow::Undefined:
                case Flow::Other: break;
                }
                if (stop) break;
                address += size;
            }
        }
        std::erase_if(labels, [&](uint32_t label) { return !code.contains(label); });
        std::erase_if(tables, [&](uint32_t label) { return !labels.contains(label); });
        return {std::vector<uint32_t>(code.begin(), code.end()), std::move(labels)};
    }

    // add pc, pc, rm lsl 2 jumps into a run of branches that starts with
    // the default case right after it.
    void branch_table(uint32_t address, std::vector<uint32_t> &blocks) {
        analysis.jump_tables++;
        uint32_t entry = address + 4;
        while (auto word = text->read32(entry)) {
            Instruction instruction = arm::decode(*word, entry);
            if (instruction.condition == ALWAYS && instruction.flow.kind == Flow::Branch && !instruction.flow.link) {
                blocks.push_back(entry);
                tables.insert(entry);
                entry += 4;
            } else {
                break;
            }
        }
    }

    // ldr pc, [pc, rm lsl 2] loads its target from the table of addresses
    // right after the default branch. its size comes from the cmp just
    // before it, or failing that from how far the entries keep pointing
    // into the code.
    void address_table(uint32_t address, uint32_t index, std::vector<uint32_t> &blocks) {
        analysis.jump_tables++;
        // the Rust's entries, named apart from the member
        std::optional<uint64_t> sized;
        if (address >= 4) {
            if (auto previous = text->read32(address - 4)) {
                if (auto compare = arm::compare_immediate(*previous)) {
                    if (compare->first == index) sized = uint64_t(compare->second) + 1;
                }
            }
        }
        uint32_t table = address + 8;
        uint64_t count = sized.value_or(256);
        for (uint64_t i = 0; i < count; i++) {
            uint32_t slot = table + uint32_t(i) * 4;
            auto target = text->read32(slot);
            if (!target) break;
            if (!text->contains(*target & ~1u) || !may_point(slot)) break;
            mark(slot, 4, Byte::Literal);
            if ((*target & 3) == 0) {
                blocks.push_back(*target);
                tables.insert(*target);
            }
            else
                code_pointer(*target, Source::Pointer);
        }
    }
};

} // namespace

Analysis analyze(const Program &program) {
    Discovery discovery;
    discovery.text = &program.text;
    discovery.slots = program.slots ? &*program.slots : nullptr;
    discovery.analysis.map.assign(program.text.bytes.size(), Byte::Unknown);
    // where relocations say the code holds data, it cannot be code
    if (program.slots)
        for (uint32_t slot : *program.slots) discovery.mark(slot, 4, Byte::Literal);
    for (const auto &[address, source] : program.seeds) discovery.code_pointer(address, source);
    discovery.run();
    // once nothing surer is left, the gaps get looked at for how functions
    // begin, each round of them can lead to more
    for (;;) {
        std::vector<uint32_t> starts = discovery.gap_starts();
        if (starts.empty()) break;
        for (uint32_t address : starts) discovery.queue.emplace_back(address, Mode::Arm, Source::Scan);
        discovery.run();
    }
    discovery.split_huge();
    return std::move(discovery.analysis);
}

} // namespace recomp
