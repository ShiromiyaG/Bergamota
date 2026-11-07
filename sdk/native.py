"""Builds a port's own code for the game's ARM11 (its native/) into what the
runtime and 3dsrecomp take with --decomp: decomp.bin, the code, and
decomp.txt, what it replaces and changes in the game.

    python -I sdk/native.py --out <dir> <source folder or file>...
        [--extra <folder or file>]... [-I <folder>]... [--symbols <file>]...
        [--starts <folder>] [--program <update>] [--base 0x07800000]
        [--game <update> --reference <update>]

The port's tool (sdk/port.py native/build/package) runs it from port.toml;
this command line is for other tools (decomp-tools builds a decomp with it).

Every .c and .cpp is compiled with clang for armv6k with VFPv2 and floats
passed the softfp way, as the game's own calls are (in r0-r3 and on the
stack, a float result in r0); freestanding, one section per function, with
sdk/include (port.h) on the include path. The objects are linked at --base
with ld.lld against the game's functions by address (game_symbols.s, as
below), and written to --out:

    decomp.elf   the linked code, with symbols, for debugging
    decomp.bin   the same, flat, to be loaded at its base
    decomp.txt   its lines (docs/porting.md): base, entry, hook, patch,
                 module_hook, module_import, symbol, option

What the code calls in the game (port.h has the names): f_XXXXXXXX and
t_XXXXXXXX by address, a name from the --symbols files (name, address and
kind f, t or l a line, as `3dsrecomp symbols` writes them; a later file's
name wins), f_<Module>_XXXXXXXX through a word the runtime fills as the
module loads. A run-time helper (division, memcpy) no symbol file names is
taken from sdk/lib/aeabi.c. Anything left unknown is listed and the build
fails.

--extra adds sources that lose to the port's: a function both define is the
port's, the other weakened (a decomp's, under the port's code).

--starts is a folder of `3dsrecomp symbols` output for the game the
addresses are from: a function shorter than the jump that replaces it is
left to the game, as the jump would run over what follows. --program is
that game's update (or program) unpacked, exheader.bin and code.bin (or
exefs/code.bin): a function the game enters past its start, inside where
the jump goes, is left to the game too. Without either, nothing is left
out for that reason.

--game builds for another game than the one the addresses are from (Ultra
Moon from Ultra Sun's): every address the code uses is looked up in --game
by the bytes around it in --reference (sdk/addresses.py); a file needing
one that is not found is left out, that feature off for that game.
"""

import argparse
import bisect
import os
import re
import struct
import subprocess
import sys
import tempfile
from concurrent.futures import ThreadPoolExecutor

SDK = os.path.dirname(os.path.abspath(__file__))
INCLUDE = os.path.join(SDK, "include")
HELPERS = os.path.join(SDK, "lib", "aeabi.c")
LLVM = os.environ.get("LLVM_BIN", r"C:\Program Files\LLVM\bin" if os.name == "nt" else "")
BASE = 0x07800000

# armv6k with VFPv2, floats passed and returned the softfp way, as the game's
# own calls do; freestanding, no unwinding, one section per function
CFLAGS = ["--target=armv6k-none-eabi", "-marm", "-mfloat-abi=softfp", "-mfpu=vfpv2", "-O2",
          "-ffreestanding", "-fno-builtin", "-fno-exceptions", "-fno-rtti", "-fno-unwind-tables",
          "-fno-asynchronous-unwind-tables", "-ffunction-sections", "-fdata-sections", "-fno-pic",
          "-Wall", "-Wno-unused-function"]

# the run-time helpers clang calls by the names of a game's own: the divmod
# ones leave the quotient in r0 as the plain ones do, the 4-aligned memory
# ones take any alignment
ALIASES = {"__aeabi_uidiv": "__aeabi_uidivmod", "__aeabi_idiv": "__aeabi_idivmod",
           "__aeabi_memcpy8": "__aeabi_memcpy", "__aeabi_memset4": "__aeabi_memset",
           "__aeabi_memset8": "__aeabi_memset", "__aeabi_memclr8": "__aeabi_memclr4"}


class BuildError(Exception):
    pass


def tool(name):
    for candidate in (os.path.join(LLVM, name + ".exe"), os.path.join(LLVM, name)):
        if LLVM and os.path.exists(candidate):
            return candidate
    return name


def run(command):
    try:
        result = subprocess.run(command, capture_output=True, text=True)
    except FileNotFoundError:
        raise BuildError(f"{command[0]} not found: LLVM (clang, ld.lld, llvm-nm, llvm-objcopy) is needed, "
                         "on the PATH or in LLVM_BIN")
    if result.returncode:
        raise BuildError(f"{' '.join(os.path.basename(c) for c in command[:2])} failed:\n{result.stdout}{result.stderr}")
    return result.stdout


def sources_in(paths):
    """the .c and .cpp files of folders (walked) and files, absolute"""
    out = []
    for path in paths:
        if os.path.isdir(path):
            for folder, _, files in os.walk(path):
                out += [os.path.abspath(os.path.join(folder, f)) for f in sorted(files) if f.endswith((".c", ".cpp"))]
        elif path.endswith((".c", ".cpp")):
            out.append(os.path.abspath(path))
    return out


def object_of(source, out, root):
    try:
        name = os.path.relpath(source, root)
    except ValueError:  # another drive
        name = source
    return os.path.join(out, re.sub(r"\W", "_", name) + ".o")


def known(symbol_files):
    """name -> address, bit 0 set for Thumb, from symbol files (a later
    file's name wins)"""
    names = {}
    for path in symbol_files:
        if not path or not os.path.exists(path):
            continue
        for line in open(path, encoding="utf-8"):
            parts = line.split()
            if len(parts) < 3 or line.startswith("#"):
                continue
            name, address, kind = parts[0], int(parts[1], 16), parts[2]
            names[name] = address | (1 if kind == "t" else 0)
    for helper, game in ALIASES.items():
        if helper not in names and game in names:
            names[helper] = names[game]
    return names


def nm(objects, flag):
    # many objects through a response file: a decomp's are past a command
    # line's length
    listed, response = list(objects), None
    if sum(len(o) + 3 for o in listed) > 8000:
        handle, response = tempfile.mkstemp(suffix=".rsp")
        with os.fdopen(handle, "w", encoding="utf-8") as f:
            f.write("\n".join('"' + o.replace("\\", "/") + '"' for o in listed) + "\n")
        listed = ["@" + response]
    try:
        out = run([tool("llvm-nm"), flag, "--format=just-symbols", *listed])
    finally:
        if response:
            os.remove(response)
    # with several objects nm names each one on a line of its own
    return {line.strip() for line in out.splitlines() if line.strip() and not line.endswith(":")}


def undefined(objects):
    # one function of the code calling another links to it
    return sorted(nm(objects, "--undefined-only") - nm(objects, "--defined-only"))


def game_symbols(objects, names, moved, out):
    """the assembly file that gives each name the code uses in the game its
    address. An assembly file rather than a linker script: a symbol defined
    there with .thumb_set, or typed as a function, carries its mode, so the
    linker calls a Thumb function with BLX (or through a veneer, for a tail
    call), which an address given in a linker script does not tell it to.
    moved: {address: address in the game built for}, None for the same."""
    lines, missing = [], []
    for symbol in undefined(objects):
        if re.fullmatch(r"__slot_(at?|s\d+)_\w+_[0-9A-Fa-f]{8}", symbol):
            lines += [".data", ".balign 4", f".globl {symbol}", f"{symbol}:", "    .word 0", ".text"]
            continue
        m = re.fullmatch(r"([ft])_(\w+)_([0-9A-Fa-f]{8})", symbol)
        if m:
            # a module's function: a jump through a word the runtime fills
            # as the module loads (ldr pc takes the mode from its bit 0)
            lines += [f".section .text.{symbol},\"ax\",%progbits", ".arm", ".balign 4", f".globl {symbol}",
                      f".type {symbol}, %function", f"{symbol}:", "    ldr pc, [pc, #-4]",
                      f".globl __slot_{symbol}", f"__slot_{symbol}:", "    .word 0", ".text"]
            continue
        m = re.fullmatch(r"([ft])_([0-9A-Fa-f]{8})", symbol)
        if m:
            address = int(m.group(2), 16) | (1 if m.group(1) == "t" else 0)
        elif symbol in names:
            address = names[symbol]
        else:
            missing.append(symbol)
            continue
        if moved is not None:
            if address not in moved:
                missing.append(symbol)
                continue
            address = moved[address]
        lines.append(f".globl {symbol}")
        if address & 1:
            lines.append(f".thumb_set {symbol}, 0x{address & ~1:08X}")
        else:
            lines.append(f".type {symbol}, %function\n.set {symbol}, 0x{address:08X}")
    if missing:
        raise BuildError("no address for:\n  " + "\n  ".join(missing) +
                         "\n(f_XXXXXXXX for a function of the game's by address, or a name in a symbol file:"
                         " port.toml's [native] symbols, `port.py symbols`)")
    with open(out, "w", encoding="utf-8") as f:
        f.write("@ the game's functions the code calls (sdk/native.py)\n")
        f.write("\n".join(lines) + "\n")


def kind_of(type):
    """what a function of this return type leaves in r0, or r0 and r1 (softfp:
    a float in r0, a double in r0 and r1)."""
    words = type.replace("*", " * ").split()
    if "*" in words:
        return "int"
    if words[-1:] == ["void"]:
        return "void"
    if {"float", "f32"} & set(words):
        return "float"
    if {"double", "f64"} & set(words):
        return "double"
    if words.count("long") == 2 or {"u64", "s64", "uint64_t", "int64_t"} & set(words):
        return "long"
    return "int"


def stub_length(old):
    """the jump the runtime writes over a replaced function (runtime/decomp.h,
    decomp_stub): ldr pc and its word from ARM, after a bx pc (and a nop from
    a word's start) from Thumb."""
    return 8 if not old & 1 else 12 if not old & 2 else 10


def read_program(folder):
    """an update or program unpacked: (exheader, code), None without one"""
    if not folder:
        return None
    for code in (os.path.join(folder, "code.bin"), os.path.join(folder, "exefs", "code.bin")):
        exheader = os.path.join(folder, "exheader.bin")
        if os.path.exists(code) and os.path.exists(exheader):
            return open(exheader, "rb").read(), open(code, "rb").read()
    return None


def entered_inside(hooks, starts, program):
    """the hooks whose jump covers an address other code branches to: the
    game enters some functions past their first instruction (a nop before
    the function, which code reaches with a b), and would land in the
    jump's word. The b, bl and blx of the executable's text (ARM, and
    Thumb's bl pairs) from outside the function; a word of data that looks
    like one only leaves a function to the game. Nothing without the code."""
    read = read_program(program)
    if not read:
        return set()
    exheader, code = read
    text, _, text_size = struct.unpack_from("<III", exheader, 0x10)
    sources = {}
    for off in range(0, text_size - 3, 4):
        word = struct.unpack_from("<I", code, off)[0]
        if (word >> 25) & 7 == 5:
            imm = ((word & 0xFFFFFF) ^ 0x800000) - 0x800000
            target = text + off + 8 + imm * 4 + (((word >> 24) & 1) * 2 if word >> 28 == 0xF else 0)
            sources.setdefault(target & ~1, []).append(text + off)
    for off in range(0, text_size - 3, 2):
        high, low = struct.unpack_from("<HH", code, off)
        if high >> 11 == 0x1E and low >> 11 in (0x1F, 0x1D):
            imm = ((((high & 0x7FF) << 12) | ((low & 0x7FF) << 1)) ^ 0x400000) - 0x400000
            target = text + off + 4 + imm
            sources.setdefault(target & ~3 if low >> 11 == 0x1D else target & ~1, []).append(text + off)
    table = sorted(set(starts or []) | {old & ~1 for old, _, _ in hooks})
    out = set()
    for old, _, _ in hooks:
        start = old & ~1
        i = bisect.bisect_right(table, start)
        end = table[i] if i < len(table) else text + text_size
        for at in range(start + 2, start + stub_length(old), 2):
            if any(not start <= source < end for source in sources.get(at, ())):
                out.add(old)
    return out


def fits(hooks, module_hooks, starts_folder, program, log):
    """the hooks whose jump fits in their function. A function shorter than
    the jump (a bare bx lr, a 6-byte Thumb one) would have the jump run over
    whatever follows it, the next function or a literal pool, which breaks
    the game even if the function is never called; the game's own runs
    instead. A function ends where the next symbol `3dsrecomp symbols` found
    starts (starts_folder); without them nothing is left out."""

    def starts(program_name):
        path = os.path.join(starts_folder, program_name + ".txt") if starts_folder else None
        if not path or not os.path.exists(path):
            return None
        out = set()
        for line in open(path, encoding="utf-8"):
            parts = line.split()
            if len(parts) == 3 and not line.startswith("#"):
                out.add(int(parts[1], 16) & ~1)
        return sorted(out)

    def room(table, at):
        if table is None:
            return 1 << 32
        i = bisect.bisect_right(table, at)
        return table[i] - at if i < len(table) else 1 << 32

    executable, modules, left = starts("executable"), {}, []
    entered = entered_inside(hooks, executable, program)
    kept = [(old, new, kind) for old, new, kind in hooks
            if room(executable, old & ~1) >= stub_length(old) and old not in entered or left.append(f"0x{old:08X}")]
    kept_modules = []
    for module, offset, new, kind in module_hooks:
        if module not in modules:
            modules[module] = starts(module)
        if room(modules[module], offset & ~1) >= stub_length(offset):
            kept_modules.append((module, offset, new, kind))
        else:
            left.append(f"{module}+0x{offset:X}")
    if left:
        log(f"{len(left)} functions shorter than the jump to them or entered inside it, left to the game: "
            f"{' '.join(left[:8])}" + (" ..." if len(left) > 8 else ""))
    return kept, kept_modules


def needs(object_file, names):
    """the game's addresses an object uses: functions (odd for Thumb) it
    calls and replaces, and words its PORT_PATCHes change; and the names of
    what it uses in modules, which are not carried over"""
    functions, words, modules = set(), set(), set()
    for line in run([tool("llvm-nm"), "--format=posix", object_file]).splitlines():
        parts = line.split()
        name, kind = parts[0], parts[1]
        if kind == "U":
            if re.fullmatch(r"__slot_\w+", name) or re.fullmatch(r"[ft]_[A-Za-z]\w*_[0-9A-Fa-f]{8}", name):
                modules.add(name)
                continue
            m = re.fullmatch(r"([ft])_([0-9A-Fa-f]{8})", name)
            if m:
                functions.add(int(m.group(2), 16) | (m.group(1) == "t"))
            elif name in names:
                functions.add(names[name])
            continue
        m = re.fullmatch(r"r(t?)_([0-9A-Fa-f]{8})", name)
        if m and kind in "TtWw":
            functions.add(int(m.group(2), 16) | (1 if m.group(1) else 0))
        elif re.fullmatch(r"rt?_\w+_[0-9A-Fa-f]{8}", name) and kind in "TtWw":
            modules.add(name)
        m = re.fullmatch(r"port_patch_(?:0x)?([0-9A-Fa-f]{8})", name)
        if m:
            words.add(int(m.group(1), 16))
    return functions, words, modules


def carry_over(objects, names, reference, game, out, log):
    """the objects whose game addresses are all found in game, and
    {reference address: game address}; addresses.txt written in out"""
    sys.path.insert(0, SDK)
    import addresses  # noqa: E402

    reference, target = addresses.Program(reference), addresses.Program(game)
    used = {o: needs(o, names) for o in objects}
    functions = set().union(*(f for f, _, _ in used.values()))
    words = set().union(*(w for _, w, _ in used.values()))
    mapping = addresses.map_addresses(reference, target, sorted(functions), sorted(words))
    kept = []
    for o in objects:
        f, w, m = used[o]
        missing = [f"0x{a:08X}" for a in sorted(f | w) if mapping[a] is None] + sorted(m)
        if missing:
            log(f"{os.path.basename(o)}: off for this game, not found there: {' '.join(missing[:6])}"
                + (" ..." if len(missing) > 6 else ""))
        else:
            kept.append(o)
    with open(os.path.join(out, "addresses.txt"), "w", encoding="utf-8") as f:
        f.write("# the code's game addresses (from the reference) and where they are in this game\n")
        for a in sorted(mapping):
            f.write(f"0x{a:08X} " + (f"0x{mapping[a]:08X}" if mapping[a] is not None else "none") + "\n")
    moved = sum(1 for a, b in mapping.items() if b is not None and a != b)
    log(f"{len(mapping)} game addresses, {moved} elsewhere in this game, "
        f"{sum(b is None for b in mapping.values())} not found")
    return kept, {a: b for a, b in mapping.items() if b is not None}


def build(out, sources, extra=(), includes=(), symbol_files=(), starts=None, program=None, base=BASE,
          game=None, reference=None, root=None, host_symbols=("port_",), log=print):
    """builds sources (files and folders: the port's) and extra (files and
    folders whose functions lose to the port's) into out; the numbers of
    files and replaced functions. Raises BuildError."""
    os.makedirs(out, exist_ok=True)
    port_sources = sources_in(sources)
    all_sources = sources_in(extra) + port_sources
    if not all_sources:
        raise BuildError("no .c or .cpp to build")
    root = root or os.path.commonpath(all_sources)
    if os.path.isfile(root):
        root = os.path.dirname(root)
    flags = [*CFLAGS, "-I", INCLUDE, *[a for i in includes for a in ("-I", i)]]

    def compile(source, object_file):
        language = ["-x", "c++", "-std=c++17"] if source.endswith(".cpp") else ["-x", "c", "-std=gnu11"]
        run([tool("clang"), *flags, *language, "-c", source, "-o", object_file])

    ordered = sorted(all_sources)
    objects = [object_of(s, out, root) for s in ordered]
    port_objects = {object_of(s, out, root) for s in port_sources}
    with ThreadPoolExecutor(max_workers=os.cpu_count() or 4) as pool:
        for result in [pool.submit(compile, s, o) for s, o in zip(ordered, objects)]:
            result.result()

    # the port's functions win: the others' of the same name weakened
    ported = set()
    for object_file in port_objects:
        for line in run([tool("llvm-nm"), "--defined-only", "--format=posix", object_file]).splitlines():
            name, kind = line.split()[:2]
            if kind in "Tt" and re.fullmatch(r"rt?_(?:\w+_)?[0-9A-Fa-f]{8}", name):
                ported.add(name)
    if ported:
        for object_file in objects:
            if object_file in port_objects:
                continue
            defined = {line.split()[0] for line in
                       run([tool("llvm-nm"), "--defined-only", "--format=posix", object_file]).splitlines()}
            if defined & ported:
                run([tool("llvm-objcopy"), *[f"--weaken-symbol={name}" for name in sorted(defined & ported)],
                     object_file])

    names = known(symbol_files)
    # another game: the addresses carried over, a file needing one not
    # found left out
    mapping = None
    if game:
        if not reference:
            raise BuildError("building for another game needs the reference its addresses are from")
        objects, mapping = carry_over(objects, names, reference, game, out, log)
        ordered = [s for s in ordered if object_of(s, out, root) in objects]
        if not objects:
            raise BuildError("nothing left to build for this game")

    # the run-time helpers, when the code needs one no symbol file gives
    helpers = os.path.join(out, "_sdk_aeabi.o")
    run([tool("clang"), *flags, "-x", "c", "-std=gnu11", "-c", HELPERS, "-o", helpers])
    wanted = set(undefined(objects)) & nm([helpers], "--defined-only")
    if any(name not in names for name in wanted):
        objects = objects + [helpers]

    # the objects in a response file: a big build is past a command line's length
    response = os.path.join(out, "objects.rsp")
    with open(response, "w", encoding="utf-8") as f:
        f.write("\n".join('"' + o.replace("\\", "/") + '"' for o in objects) + "\n")
    symbols_source = os.path.join(out, "game_symbols.s")
    game_symbols(objects, names, mapping, symbols_source)
    symbols_object = symbols_source[:-2] + ".o"
    run([tool("clang"), "--target=armv6k-none-eabi", "-marm", "-c", symbols_source, "-o", symbols_object])

    elf = os.path.join(out, "decomp.elf")
    run([tool("ld.lld"), "-o", elf, f"-Ttext=0x{base:08X}", "--no-rosegment", "-e", "0",
         "@" + response, symbols_object])
    # .bss is part of the file, the segment is mapped whole
    run([tool("llvm-objcopy"), "-O", "binary", "--set-section-flags", ".bss=alloc,load,contents", elf,
         os.path.join(out, "decomp.bin")])

    returns = {}
    for source in ordered:
        text = open(source, encoding="utf-8").read()
        for m in re.finditer(r'^([\w \t*]+?)\s*\b\w+\s*\([^;{]*\)\s*__asm__\("(rt?_(?:\w+_)?[0-9A-Fa-f]{8})"\)',
                             text, re.M):
            returns[m.group(2)] = kind_of(m.group(1))

    hooks, entries, module_hooks, imports, symbols, patches, options = [], [], [], [], [], [], []
    flat = open(os.path.join(out, "decomp.bin"), "rb").read()
    for line in run([tool("llvm-nm"), "--defined-only", "--format=posix", elf]).splitlines():
        name, kind, address = line.split()[:3]
        if kind in "TtWw":
            entries.append(int(address, 16))
        # an option of the port's: its text, "bool <default> <label>|<help>"
        # or "int <default> <min> <max> <label>|<help>"
        m = re.fullmatch(r"port_option_(\w+)", name)
        if m:
            at = int(address, 16) - base
            text = flat[at:flat.index(b"\0", at)].decode("utf-8")
            options.append((int(address, 16), f"port_{m.group(1)}", " ".join(text.split("\n"))))
            continue
        # a word of the game's the port changes, to the constant's value
        m = re.fullmatch(r"port_patch_(?:0x)?([0-9A-Fa-f]{8})", name)
        if m:
            patches.append((int(m.group(1), 16), struct.unpack_from("<I", flat, int(address, 16) - base)[0]))
            continue
        # variables the host reads or writes (the widescreen's
        # port_top_aspect, a port's UI's)
        if kind in "DdBb" and name.startswith(tuple(host_symbols)):
            symbols.append((name, int(address, 16)))
        m = re.fullmatch(r"__slot_([ft]|at?|s\d+)_(\w+)_([0-9A-Fa-f]{8})", name)
        if m:
            kind = m.group(1)
            segment = f" seg{kind[1:]}" if kind.startswith("s") else ""
            thumb = 1 if kind in ("t", "at") else 0
            imports.append((m.group(2), int(m.group(3), 16) | thumb, int(address, 16), segment))
            continue
        m = re.fullmatch(r"r(t?)_(\w+)_([0-9A-Fa-f]{8})", name)
        if m:
            module_hooks.append((m.group(2), int(m.group(3), 16) | (1 if m.group(1) else 0), int(address, 16),
                                 returns.get(name, "int")))
        m = re.fullmatch(r"r(t?)_([0-9A-Fa-f]{8})", name)
        if m:
            hooks.append((int(m.group(2), 16) | (1 if m.group(1) else 0), int(address, 16), returns.get(name, "int")))
    hooks, module_hooks = fits(hooks, module_hooks, starts, program, log)
    if mapping is not None:
        # checked where the addresses are from (the code is the same there),
        # written for the game built for
        hooks = [(mapping[old], new, kind) for old, new, kind in hooks]
        patches = [(mapping[at], mapping.get(value, value)) for at, value in patches]
    with open(os.path.join(out, "decomp.txt"), "w", encoding="utf-8") as f:
        f.write(f"base 0x{base:08X}\n")
        f.write("".join(f"entry 0x{address:08X}\n" for address in sorted(set(entries))))
        f.write("".join(f"hook 0x{old:08X} 0x{new:08X} {kind}\n" for old, new, kind in sorted(hooks)))
        f.write("".join(f"patch 0x{at:08X} 0x{value:08X}\n" for at, value in sorted(patches)))
        f.write("".join(f"module_hook {module}.cro 0x{offset:08X} 0x{new:08X} {kind}\n"
                        for module, offset, new, kind in sorted(module_hooks)))
        f.write("".join(f"module_import {module}.cro 0x{offset:08X} 0x{slot:08X}{segment}\n"
                        for module, offset, slot, segment in sorted(imports)))
        f.write("".join(f"symbol {name} 0x{address:08X}\n" for name, address in sorted(symbols)))
        # in the order the code has them, which the menus keep
        f.write("".join(f"option {name} {text}\n" for _, name, text in sorted(options)))
    size = os.path.getsize(os.path.join(out, "decomp.bin"))
    log(f"{len(ordered)} files, {size} bytes at 0x{base:08X}, "
        f"{len(hooks) + len(module_hooks)} replaced functions, {len(patches)} patches, {len(options)} options -> {out}")
    return len(ordered), len(hooks) + len(module_hooks)


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("sources", nargs="+", help="the port's sources: folders or files")
    parser.add_argument("--out", required=True)
    parser.add_argument("--extra", action="append", default=[], help="sources that lose to the port's")
    parser.add_argument("-I", dest="includes", action="append", default=[], help="an include folder")
    parser.add_argument("--symbols", action="append", default=[], help="a symbol file (name address kind)")
    parser.add_argument("--starts", help="`3dsrecomp symbols` output of the addresses' game")
    parser.add_argument("--program", help="the addresses' game's update or program, unpacked")
    parser.add_argument("--base", default=f"0x{BASE:08X}", help="where the code is loaded")
    parser.add_argument("--game", help="another game's update, the addresses carried to it by signature")
    parser.add_argument("--reference", help="the update the code's addresses are from (with --game)")
    args = parser.parse_args()
    try:
        build(args.out, args.sources, args.extra, args.includes, args.symbols, args.starts, args.program,
              int(args.base, 0), args.game, args.reference)
    except BuildError as e:
        sys.exit(str(e))


if __name__ == "__main__":
    main()
