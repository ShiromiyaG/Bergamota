// An ARM11 (ARMv6K) interpreter, ARM, Thumb and VFPv2, user mode only: the
// safety net for code the recompiled C does not have (a path no session
// visited, a jump into the middle of a function). It runs until it reaches
// an address the C has, a syscall, or an instruction it cannot run.
// Translated from Zakuro's zakuro-cpu (Rust, MIT, Copyright (c) 2026
// fearkov; see LICENSE-zakuro).

#ifndef RUNTIME_INTERPRETER_H
#define RUNTIME_INTERPRETER_H

#include <cstdint>
#include <optional>

#include "recomp.h"
#include "runtime/memory.h"

namespace runtime {

struct Psr {
    bool n = false, z = false, c = false, v = false, q = false, thumb = false;
    uint8_t ge = 0;
    // user mode always: MRS reads it, MSR cannot change it
    uint32_t bits() const;
};

struct Cpu {
    uint32_t regs[16]{};
    Psr cpsr;
    uint32_t *vfp = nullptr; // s0..s31; dN is s(2N) low and s(2N+1) high
    uint32_t *fpscr = nullptr;
    uint32_t fpexc = 1u << 30;
    std::optional<uint32_t> exclusive;
    uint32_t tls = 0, tid_rw = 0;
    bool branched = false;

    uint32_t current_pc() const { return regs[15] - (cpsr.thumb ? 4 : 8); }
    void branch(uint32_t a) {
        regs[15] = cpsr.thumb ? a & ~1u : a & ~3u;
        branched = true;
    }
    void branch_exchange(uint32_t a) {
        cpsr.thumb = a & 1;
        branch(a);
    }
    void set_reg(unsigned i, uint32_t v) {
        if (i == 15) branch(v);
        else regs[i] = v;
    }
    void set_nz(uint32_t v) {
        cpsr.n = static_cast<int32_t>(v) < 0;
        cpsr.z = v == 0;
    }
};

struct Exit {
    enum Kind { None, Supervisor, Undefined, Breakpoint } kind = None;
    uint32_t value = 0; // the svc's number, or the opcode
};

// one instruction at regs[15]
Exit step(Cpu &cpu, Memory &bus);

// the context's state into an interpreter and back
void load(const Context &c, Cpu &cpu);
void store(const Cpu &cpu, Context &c);

} // namespace runtime

#endif
