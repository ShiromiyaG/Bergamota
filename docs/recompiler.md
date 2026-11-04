# The recompiler

`recomp/` is 3dsrecomp, the static recompiler that turns a game's ARM11
code into C, translated from its Rust (fearkov's, MIT,
[`LICENSE-3dsrecomp`](../LICENSE-3dsrecomp)) into C++ and built with the
rest of the runtime: `cmake --build build --target 3dsrecomp`. It needs
nothing but a C++20 compiler (no SDL, no Vulkan); the C it writes is
compiled by whatever C compiler `CC` names (else gcc, clang or cc).

Translated, its C was the Rust's byte for byte, checked on Ultra Sun, Ultra
Moon, X, Y, Omega Ruby and Alpha Sapphire (discovery, the front end and
every generated file), with and without the update, a decomp, hot lists, a
fast compile and overrides (runtime commit `d74dbaf`). Since then it writes
interface 6, below; the Rust in `tools/3dsrecomp` writes interface 5, which
the runtime still runs.

```sh
build/3dsrecomp analyze <game .3ds>
build/3dsrecomp build <game .3ds> [<dir>] [--mods <update unpacked>] [--decomp <dir>]
                      [--hot <heat> [--cold-fast]] [--overrides <dir>] [--c-only]
build/3dsrecomp hints <game .3ds> [<hints file>]
build/3dsrecomp symbols <game .3ds> <dir>
```

`--c-only` writes the C and compiles nothing (for comparing what two
builds write). `port` and `verify` were Zakuro's and stayed behind.

| Part | Where | From the Rust's |
| --- | --- | --- |
| The dumps: NCSD, NCCH, CIA, ExeFS, RomFS, the code decompressed | `rom.h` | `rom.rs` |
| The executable's segments, the CRO modules, the decomp, the programs to analyze | `image.h`, `cro.h`, `decomp.h`, `programs.h` | `image.rs`, `cro.rs`, `decomp.rs`, `lib.rs` |
| Finding the functions | `decode.h`, `discover.h` | `arm.rs`, `thumb.rs`, `discover.rs` |
| The hot build, the overrides | `hot.h`, `overrides.h` | `hot.rs`, `overrides.rs` |
| The C | `codegen/` | `codegen/` |
| Compiling it (a compiler per core while the memory holds them, the object cache), the build, the command | `compile.h`, `subprocess.h`, `build.h`, `main.cpp` | `compile.rs`, `build.rs`, `main.rs` |
| What Rust's library gave the translation (its whitespace, `lines`, number parsing) | `rust.h` | |

`recomp.h`, the interface the C runs against, is `third_party/recomp.h`,
built into the recompiler and written beside the C; the runtime's loader
reads the same file. The bundled hints are `recomp/hints.txt`.

## Interface 6

Interface 5 (`tools/3dsrecomp/docs/interface.md`) lets the host stop a
function anywhere and come back in later: at a syscall, the end of the
time slice, a jump the code cannot follow. So every block, every return
from a call and every svc is an entry, each function starts with a switch
to all its labels, and every block checks the budget and can leave. That
is most of what makes the C big and slow to compile.

In interface 6 the code never leaves partway. Each of the game's threads
runs on a stack of its own (`runtime/fiber.h`), and what used to make the
code return to the host is a call that comes back where it was, the thread
having waited inside it when it had to:

| | interface 5 | interface 6 |
| --- | --- | --- |
| svc | returns `EXIT_SVC`, the host goes on at the entry after it | `host->svc(ctx, n)`, then on |
| the budget | every block checks it and returns `EXIT_BUDGET` | every block counts it (`BLOCK_COUNT`); where loops start and where the function does, `BLOCK` lets the other threads run there (`host->yield`) |
| code without C, a stale function | returns `EXIT_UNWIND`, the host goes on | `host->call(ctx, stop, sp)`: run until it returns to stop with the stack at sp, then on |
| a jump the code cannot follow | through its switch, else to the host | the switch has only the labels a jump table leads to and the functions it holds; anything else `JUMP_AWAY`, the host running the rest until the function would have returned |
| a tail call | `CALL`, then `RETURN` | `TAIL_CALL`, a C tail call (`musttail`): no frame, no depth |
| a return elsewhere than after the call | through its switch | to where the function itself returns, its stack as it came in (a shared epilogue ending the caller's frame too): it returns as well; anywhere else `EXIT_UNWIND` |
| entries | every label | where functions start |

An unwind is the slow way out: the C frames go and the thread goes on in
the interpreter until the callers above return, which a game's main loop
never does. `boot` says how many there were (`unwound`); with
`RUNTIME_TRACE_UNWIND=1` the player and `boot` print where. Two kinds
showed up on Ultra Sun and are handled as the table says: calls counted
deep through tail calls (a loop of them, which interface 5's syscalls used
to reset), and a callee returning for its caller.

Measured on Ultra Sun v1.2 with Solstice's `native/` (clang 23, the same
machine as `ports/Solstice/docs/build-speed.md`):

| | interface 5 | interface 6 |
| --- | ---: | ---: |
| C | 834 MiB | 765 MiB |
| `entries.c` | 30 MB | 14 MB |
| a cold build, all of it at `-O2` | 441 s | 298 s |
| the library | 284 MB | 222 MB |
| the bot over 12,000 frames (`boot`, field and the intro): the game's code | 9.0 s | 5.0 s |
| the same run, in all | 57 s | 40 s |
| interpreted, unwinds | 507, none | 9,645, none |

The same modules load in the same order; the pictures are the same until
the threads' turns part (a cutscene a few frames apart). The runtime runs
both: a library of interface 5 as before, one of 6 with `Config::fibers`
(`System::run_fibers`), which the session sets from the library.

Tests: `tests/recomp_*_test.cpp`, the Rust's tests translated.
