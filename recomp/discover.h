// Finding the code in a program: functions from the entry points by
// following calls and branches, and from words elsewhere that point into the
// code, which is where vtables and callbacks live. Translated from
// 3dsrecomp's discover.rs (Rust, MIT, Copyright (c) 2026 fearkov; see
// LICENSE-3dsrecomp).

#ifndef RECOMP_DISCOVER_H
#define RECOMP_DISCOVER_H

#include <cstdint>
#include <map>
#include <optional>
#include <set>
#include <utility>
#include <vector>

#include "image.h"

namespace recomp {

// how an address came to be taken for a function, in the Rust's order (the
// derived Ord compares by it)
enum class Source {
    Entry,
    Call,
    Pointer,    // a code address in a literal pool or the data segments
    Export,     // a symbol the program exports to other modules
    Relocation, // a code address a module's relocations store
    Import,     // a code address another module takes from this one without a name
    Scan,       // the start of a gap nothing led to, or a push in one
    Override,   // an address a function written by hand replaces
    Hint,       // an address the host had to interpret with the last library
};

enum class Mode { Arm, Thumb };

struct Function {
    Source source = Source::Entry;
    Mode mode = Mode::Arm;
    // the address of every instruction, in order
    std::vector<uint32_t> instructions;
    // where execution can enter its code: its entry, its branches' targets,
    // the instructions after calls and svc
    std::set<uint32_t> labels;
    // the labels a jump table leads to (add pc or ldr pc through a table):
    // the only ones the code reaches by an address it computes, which
    // interface 6's dispatch takes
    std::set<uint32_t> tables;
};

// what each byte of the text segment turned out to be
enum class Byte : uint8_t { Unknown, Code, Literal };

struct Analysis {
    // by entry, even for Thumb too as the Rust keys them (the mode is apart)
    std::map<uint32_t, Function> functions;
    std::vector<Byte> map;
    size_t indirect_sites = 0, jump_tables = 0, svc_sites = 0;
    // paths abandoned because they ran into something that cannot be code
    size_t dead_ends = 0;

    size_t count(Byte kind) const;
    // the longest runs of text nobody reached, as (start, length)
    std::vector<std::pair<uint32_t, size_t>> largest_gaps(uint32_t base, size_t count) const;
};

// a piece of code to analyze and what is known about it up front
struct Program {
    Segment text;
    // addresses that may start functions and how each is known, odd for Thumb
    std::vector<std::pair<uint32_t, Source>> seeds;
    // the words in the code that hold addresses, when relocations say which;
    // without them any literal that lands in the code is taken for a pointer
    std::optional<std::set<uint32_t>> slots;
};

Analysis analyze(const Program &program);

} // namespace recomp

#endif
