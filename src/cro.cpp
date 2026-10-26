// CRO modules. After Zakuro's cro/mod.rs and cro/format.rs (Rust, MIT,
// Copyright (c) 2026 fearkov; see LICENSE-zakuro).

#include "runtime/cro.h"

#include <algorithm>
#include <cstring>

namespace runtime {

namespace {

// the header, from 0x80
enum : uint32_t {
    MAGIC = 0x080, NAME = 0x084, NEXT = 0x088, PREVIOUS = 0x08C, FIXED_SIZE = 0x098, ON_UNRESOLVED = 0x0AC,
    CODE = 0x0B0, CODE_SIZE = 0x0B4, MODULE_NAME = 0x0C0, SEGMENTS = 0x0C8, SEGMENT_COUNT = 0x0CC,
    EXPORT_NAMED = 0x0D0, EXPORT_NAMED_COUNT = 0x0D4, EXPORT_INDEXED = 0x0D8, EXPORT_INDEXED_COUNT = 0x0DC,
    IMPORT_MODULES = 0x0F0, IMPORT_MODULE_COUNT = 0x0F4, EXTERNAL_RELOCATIONS = 0x0F8, EXTERNAL_RELOCATION_COUNT = 0x0FC,
    IMPORT_NAMED = 0x100, IMPORT_NAMED_COUNT = 0x104, IMPORT_INDEXED = 0x108, IMPORT_INDEXED_COUNT = 0x10C,
    IMPORT_ANONYMOUS = 0x110, IMPORT_ANONYMOUS_COUNT = 0x114, STATIC_ANONYMOUS = 0x120,
    INTERNAL_RELOCATIONS = 0x128, INTERNAL_RELOCATION_COUNT = 0x12C, STATIC_RELOCATIONS = 0x130,
    FIRST_REBASED = CODE, LAST_REBASED = STATIC_RELOCATIONS,
};
enum Kind : uint32_t { CODE_SEGMENT, RODATA, DATA, BSS };
constexpr uint32_t SEGMENT_SIZE = 0xC, RELOCATION_SIZE = 0xC, IMPORT_MODULE_SIZE = 0x14;

// the size of an entry of each table the header lists from the module's
// name on (1: counted in bytes), and the first table each fix level drops
constexpr uint32_t TABLE_ENTRY_SIZES[15] = {1, SEGMENT_SIZE, 8, 4, 1, 8, IMPORT_MODULE_SIZE, RELOCATION_SIZE,
                                            8, 8, 8, 1, 8, RELOCATION_SIZE, RELOCATION_SIZE};
constexpr uint32_t FIX_BARRIERS[4] = {0x138, STATIC_ANONYMOUS, IMPORT_MODULES, EXPORT_NAMED};

struct Segment {
    uint32_t offset, size, kind;
};

uint32_t field(Memory &m, uint32_t base, uint32_t at) { return m.read32(base + at); }
void set_field(Memory &m, uint32_t base, uint32_t at, uint32_t v) { m.write32(base + at, v); }

std::optional<Segment> segment(Memory &m, uint32_t base, uint32_t index) {
    if (index >= field(m, base, SEGMENT_COUNT)) return std::nullopt;
    uint32_t e = field(m, base, SEGMENTS) + index * SEGMENT_SIZE;
    return Segment{m.read32(e), m.read32(e + 4), m.read32(e + 8)};
}

void set_segment(Memory &m, uint32_t base, uint32_t index, const Segment &s) {
    uint32_t e = field(m, base, SEGMENTS) + index * SEGMENT_SIZE;
    m.write32(e, s.offset);
    m.write32(e + 4, s.size);
    m.write32(e + 8, s.kind);
}

// a segment tag: the segment in the low 4 bits, the offset above
std::optional<uint32_t> tag_address(Memory &m, uint32_t base, uint32_t tag) {
    auto s = segment(m, base, tag & 0xF);
    if (!s || (tag >> 4) >= s->size) return std::nullopt;
    return s->offset + (tag >> 4);
}

// relocations write into code and read-only data too
void write_privileged(Memory &m, uint32_t at, uint32_t value) { m.write32_privileged(at, value); }

void relocate(Memory &m, uint32_t target, uint8_t kind, uint32_t addend, uint32_t symbol, uint32_t final_target) {
    if (kind == 2 || kind == 10) write_privileged(m, target, symbol + addend);                       // absolute
    else if (kind == 3) write_privileged(m, target, symbol + addend - final_target);                // relative
    // no game of the three uses the branch kinds
}

// a batch of external relocations, all to one symbol; its first entry's
// third byte says whether it is resolved
void relocate_batch(Memory &m, uint32_t base, uint32_t batch, uint32_t symbol, bool resolved) {
    for (uint32_t at = batch; at < batch + RELOCATION_SIZE * 0x1000; at += RELOCATION_SIZE) {
        if (auto target = tag_address(m, base, m.read32(at))) relocate(m, *target, m.read8(at + 4), m.read32(at + 8), symbol, *target);
        if (m.read8(at + 5)) break; // the batch's end
    }
    m.write8(batch + 6, resolved);
}

bool resolved(Memory &m, uint32_t batch) { return m.read8(batch + 6) != 0; }

uint32_t unresolved_handler(Memory &m, uint32_t base) {
    return tag_address(m, base, field(m, base, ON_UNRESOLVED)).value_or(0);
}

// offsets in a table's entries into addresses, or back
void move_table(Memory &m, uint32_t base, uint32_t table_field, uint32_t count_field, uint32_t size,
                std::initializer_list<uint32_t> offsets, bool forward) {
    uint32_t table = field(m, base, table_field), count = field(m, base, count_field);
    if (!table) return;
    for (uint32_t i = 0; i < count; i++) {
        for (uint32_t o : offsets) {
            uint32_t v = m.read32(table + i * size + o);
            if (v) m.write32(table + i * size + o, forward ? v + base : v - base);
        }
    }
}

void move_tables(Memory &m, uint32_t base, bool forward) {
    move_table(m, base, EXPORT_NAMED, EXPORT_NAMED_COUNT, 8, {0}, forward);
    move_table(m, base, IMPORT_MODULES, IMPORT_MODULE_COUNT, IMPORT_MODULE_SIZE, {0, 4, 12}, forward);
    move_table(m, base, IMPORT_NAMED, IMPORT_NAMED_COUNT, 8, {0, 4}, forward);
    move_table(m, base, IMPORT_INDEXED, IMPORT_INDEXED_COUNT, 8, {4}, forward);
    move_table(m, base, IMPORT_ANONYMOUS, IMPORT_ANONYMOUS_COUNT, 8, {4}, forward);
}

// the lists the game walks from the CRS to find modules: its next field
// heads the auto-linked ones, its previous the others; a head keeps the
// tail in its previous field
void enlist(Memory &m, uint32_t crs, uint32_t base, bool auto_link) {
    uint32_t head_field = auto_link ? NEXT : PREVIOUS;
    uint32_t head = field(m, crs, head_field);
    if (!head) {
        set_field(m, crs, head_field, base);
        set_field(m, base, PREVIOUS, base);
    } else {
        uint32_t tail = field(m, head, PREVIOUS);
        set_field(m, tail, NEXT, base);
        set_field(m, base, PREVIOUS, tail);
        set_field(m, head, PREVIOUS, base);
    }
    set_field(m, base, NEXT, 0);
}

void delist(Memory &m, uint32_t crs, uint32_t base) {
    uint32_t next = field(m, base, NEXT), previous = field(m, base, PREVIOUS);
    if (!previous) return;
    if (field(m, crs, NEXT) == base || field(m, crs, PREVIOUS) == base) { // the head
        uint32_t head_field = field(m, crs, NEXT) == base ? NEXT : PREVIOUS;
        if (next) set_field(m, next, PREVIOUS, previous);
        set_field(m, crs, head_field, next);
    } else if (next) {
        set_field(m, previous, NEXT, next);
        set_field(m, next, PREVIOUS, previous);
    } else { // the tail
        set_field(m, previous, NEXT, 0);
        for (uint32_t head_field : {NEXT, PREVIOUS}) {
            uint32_t head = field(m, crs, head_field);
            if (head && field(m, head, PREVIOUS) == base) set_field(m, head, PREVIOUS, previous);
        }
    }
    set_field(m, base, NEXT, 0);
    set_field(m, base, PREVIOUS, 0);
}

} // namespace

bool CroLoader::rebase(Memory &m, uint32_t base, uint32_t size, uint32_t data, uint32_t data_size, uint32_t bss,
                       uint32_t bss_size, bool is_static, uint32_t &old_data) {
    char magic[4];
    m.read_bytes(base + MAGIC, magic, 4);
    if (std::memcmp(magic, "CRO0", 4) && std::memcmp(magic, "CRS0", 4)) return false;
    // every offset in the header becomes an address
    if (uint32_t name = field(m, base, NAME)) set_field(m, base, NAME, name + base);
    for (uint32_t f = FIRST_REBASED; f <= LAST_REBASED; f += 8) {
        if (uint32_t v = field(m, base, f)) set_field(m, base, f, v + base);
    }
    // the CRS's segments describe the executable, already addresses; a
    // module's code and read-only data stay in its buffer, its data and bss
    // go where the game says
    old_data = 0;
    for (uint32_t i = 0; !is_static && i < field(m, base, SEGMENT_COUNT); i++) {
        Segment s = *segment(m, base, i);
        if (s.kind == DATA && s.size) {
            if (s.size > data_size) return false;
            old_data = s.offset + base;
            s.offset = data;
        } else if (s.kind == BSS && s.size) {
            if (s.size > bss_size) return false;
            s.offset = bss;
        } else if (s.offset) {
            s.offset += base;
            if (s.offset > base + size) return false;
        }
        set_segment(m, base, i, s);
    }
    move_tables(m, base, true);
    // the relocations within the module; those into its data go to the copy
    // still in its buffer, which the game moves afterwards
    uint32_t table = field(m, base, INTERNAL_RELOCATIONS);
    for (uint32_t i = 0; i < field(m, base, INTERNAL_RELOCATION_COUNT); i++) {
        uint32_t e = table + i * RELOCATION_SIZE, tag = m.read32(e);
        auto target_segment = segment(m, base, tag & 0xF);
        auto symbol = segment(m, base, m.read8(e + 5));
        if (!target_segment || !symbol || (tag >> 4) >= target_segment->size) continue;
        uint32_t final_target = target_segment->offset + (tag >> 4);
        uint32_t target = target_segment->kind == DATA ? old_data + (tag >> 4) : final_target;
        relocate(m, target, m.read8(e + 4), m.read32(e + 8), symbol->offset, final_target);
    }
    reset_imports(m, base);
    return true;
}

// every import at the module's own handler for what nothing provides, as a
// freshly loaded module has them
void CroLoader::reset_imports(Memory &m, uint32_t base) {
    uint32_t handler = unresolved_handler(m, base), table = field(m, base, EXTERNAL_RELOCATIONS);
    for (uint32_t i = 0; i < field(m, base, EXTERNAL_RELOCATION_COUNT); i++) {
        uint32_t e = table + i * RELOCATION_SIZE;
        if (handler) {
            if (auto target = tag_address(m, base, m.read32(e))) relocate(m, *target, m.read8(e + 4), m.read32(e + 8), handler, *target);
        }
        m.write8(e + 6, 0);
    }
}

CroLoader::Module CroLoader::describe(Memory &m, uint32_t base, uint32_t size, bool auto_link) {
    Module module{m.read_cstring(field(m, base, MODULE_NAME), 64), base, size, auto_link, {}, {}, {}};
    uint32_t table = field(m, base, EXPORT_NAMED);
    for (uint32_t i = 0; i < field(m, base, EXPORT_NAMED_COUNT); i++) {
        if (auto at = tag_address(m, base, m.read32(table + i * 8 + 4))) {
            module.exports[m.read_cstring(m.read32(table + i * 8), 512)] = *at;
        }
    }
    table = field(m, base, EXPORT_INDEXED);
    for (uint32_t i = 0; i < field(m, base, EXPORT_INDEXED_COUNT); i++) {
        module.indexed_exports.push_back(tag_address(m, base, m.read32(table + i * 4)).value_or(0));
    }
    for (uint32_t i = 0; i < field(m, base, SEGMENT_COUNT); i++) module.segments.push_back(segment(m, base, i)->offset);
    return module;
}

// resolves a module's imports against the auto-linked modules (named) and
// the modules it names (by index, anonymous)
void CroLoader::link(Memory &m, const Module &module) {
    uint32_t base = module.base, table = field(m, base, IMPORT_NAMED);
    for (uint32_t i = 0; i < field(m, base, IMPORT_NAMED_COUNT); i++) {
        uint32_t batch = m.read32(table + i * 8 + 4);
        if (!batch || resolved(m, batch)) continue;
        std::string name = m.read_cstring(m.read32(table + i * 8), 512);
        for (const Module &source : modules_) {
            auto it = source.exports.find(name);
            if (source.auto_link && it != source.exports.end()) {
                relocate_batch(m, base, batch, it->second, true);
                break;
            }
        }
    }
    table = field(m, base, IMPORT_MODULES);
    for (uint32_t i = 0; i < field(m, base, IMPORT_MODULE_COUNT); i++) {
        uint32_t e = table + i * IMPORT_MODULE_SIZE;
        std::string name = m.read_cstring(m.read32(e), 64);
        auto source = std::find_if(modules_.begin(), modules_.end(), [&](const Module &s) { return s.name == name; });
        if (source == modules_.end()) continue;
        for (uint32_t j = 0; j < m.read32(e + 8); j++) { // by index
            uint32_t s = m.read32(e + 4) + j * 8, index = m.read32(s), batch = m.read32(s + 4);
            if (batch && !resolved(m, batch) && index < source->indexed_exports.size())
                relocate_batch(m, base, batch, source->indexed_exports[index], true);
        }
        for (uint32_t j = 0; j < m.read32(e + 16); j++) { // anonymous: a segment tag
            uint32_t s = m.read32(e + 12) + j * 8, tag = m.read32(s), batch = m.read32(s + 4);
            if (batch && !resolved(m, batch) && (tag & 0xF) < source->segments.size())
                relocate_batch(m, base, batch, source->segments[tag & 0xF] + (tag >> 4), true);
        }
    }
}

// the modules already loaded whose imports the new one provides
void CroLoader::link_others_to(Memory &m, const Module &module) {
    for (const Module &other : modules_) {
        if (other.base == module.base) continue;
        uint32_t base = other.base, table = field(m, base, IMPORT_NAMED);
        for (uint32_t i = 0; i < field(m, base, IMPORT_NAMED_COUNT); i++) {
            uint32_t batch = m.read32(table + i * 8 + 4);
            if (!batch || resolved(m, batch)) continue;
            auto it = module.exports.find(m.read_cstring(m.read32(table + i * 8), 512));
            if (it != module.exports.end()) relocate_batch(m, base, batch, it->second, true);
        }
        table = field(m, base, IMPORT_MODULES);
        for (uint32_t i = 0; i < field(m, base, IMPORT_MODULE_COUNT); i++) {
            uint32_t e = table + i * IMPORT_MODULE_SIZE;
            if (m.read_cstring(m.read32(e), 64) != module.name) continue;
            for (uint32_t j = 0; j < m.read32(e + 8); j++) {
                uint32_t s = m.read32(e + 4) + j * 8, index = m.read32(s), batch = m.read32(s + 4);
                if (batch && !resolved(m, batch) && index < module.indexed_exports.size())
                    relocate_batch(m, base, batch, module.indexed_exports[index], true);
            }
            for (uint32_t j = 0; j < m.read32(e + 16); j++) {
                uint32_t s = m.read32(e + 12) + j * 8, tag = m.read32(s), batch = m.read32(s + 4);
                if (batch && !resolved(m, batch) && (tag & 0xF) < module.segments.size())
                    relocate_batch(m, base, batch, module.segments[tag & 0xF] + (tag >> 4), true);
            }
        }
    }
}

bool CroLoader::initialize(Memory &m, uint32_t base, uint32_t size) {
    uint32_t old_data;
    if (!rebase(m, base, size, 0, 0, 0, 0, true, old_data)) return false;
    set_field(m, base, NEXT, 0);
    set_field(m, base, PREVIOUS, 0);
    crs_ = base;
    modules_.push_back(describe(m, base, size, true));
    return true;
}

std::optional<uint32_t> CroLoader::load(Memory &m, uint32_t base, uint32_t size, uint32_t data, uint32_t data_size,
                                        uint32_t bss, uint32_t bss_size, bool auto_link, uint32_t fix_level) {
    uint32_t old_data;
    if (!rebase(m, base, size, data, data_size, bss, bss_size, false, old_data)) return std::nullopt;
    modules_.push_back(describe(m, base, size, auto_link));
    const Module &module = modules_.back();
    if (crs_) enlist(m, *crs_, base, auto_link);
    link(m, module);
    link_others_to(m, module);

    // fixing: the tables the level says are no longer needed go, and the
    // game takes back the memory past what is left
    if (fix_level == 0 || fix_level > 3) return 0;
    uint32_t barrier = FIX_BARRIERS[fix_level];
    uint32_t end = field(m, base, CODE) + field(m, base, CODE_SIZE);
    uint32_t at = MODULE_NAME;
    for (uint32_t entry : TABLE_ENTRY_SIZES) {
        uint32_t table = field(m, base, at), count = field(m, base, at + 4);
        if (at < barrier) {
            if (count) end = std::max(end, table + count * entry);
        } else {
            set_field(m, base, at + 4, 0);
        }
        at += 8;
    }
    uint32_t fixed = std::min((end - base + 0xFFF) & ~0xFFFu, size);
    for (uint32_t f = barrier; f <= STATIC_RELOCATIONS; f += 8) set_field(m, base, f, base + fixed);
    set_field(m, base, FIXED_SIZE, fixed);
    return fixed;
}

void CroLoader::unload(Memory &m, uint32_t base) {
    auto it = std::find_if(modules_.begin(), modules_.end(), [&](const Module &s) { return s.base == base; });
    if (it == modules_.end()) return;
    Module module = *it;
    modules_.erase(it);
    // what linked against it goes back to its own unresolved handler
    for (const Module &other : modules_) {
        if (!other.auto_link) continue;
        uint32_t ob = other.base, handler = unresolved_handler(m, ob);
        auto reset = [&](uint32_t batch) {
            if (batch && resolved(m, batch)) relocate_batch(m, ob, batch, handler, false);
        };
        uint32_t table = field(m, ob, IMPORT_NAMED);
        for (uint32_t i = 0; i < field(m, ob, IMPORT_NAMED_COUNT); i++) {
            if (module.exports.count(m.read_cstring(m.read32(table + i * 8), 512))) reset(m.read32(table + i * 8 + 4));
        }
        table = field(m, ob, IMPORT_MODULES);
        for (uint32_t i = 0; i < field(m, ob, IMPORT_MODULE_COUNT); i++) {
            uint32_t e = table + i * IMPORT_MODULE_SIZE;
            if (m.read_cstring(m.read32(e), 64) != module.name) continue;
            for (uint32_t j = 0; j < m.read32(e + 8); j++) reset(m.read32(m.read32(e + 4) + j * 8 + 4));
            for (uint32_t j = 0; j < m.read32(e + 16); j++) reset(m.read32(m.read32(e + 12) + j * 8 + 4));
        }
    }
    if (crs_) delist(m, *crs_, base);
    // the module as its file has it again: offsets, not addresses
    move_tables(m, base, false);
    for (uint32_t i = 0; i < field(m, base, SEGMENT_COUNT); i++) {
        Segment s = *segment(m, base, i);
        s.offset = s.kind == BSS || !s.offset ? 0 : s.offset - base;
        set_segment(m, base, i, s);
    }
    if (uint32_t name = field(m, base, NAME)) set_field(m, base, NAME, name - base);
    for (uint32_t f = FIRST_REBASED; f <= LAST_REBASED; f += 8) {
        if (uint32_t v = field(m, base, f)) set_field(m, base, f, v - base);
    }
    set_field(m, base, FIXED_SIZE, 0);
}

} // namespace runtime
