# The recompiler

`recomp/` is 3dsrecomp, the static recompiler that turns a game's ARM11
code into C, translated from its Rust (fearkov's, MIT,
[`LICENSE-3dsrecomp`](../LICENSE-3dsrecomp)) into C++ and built with the
rest of the runtime: `cmake --build build --target 3dsrecomp`. It needs
nothing but a C++20 compiler (no SDL, no Vulkan); the C it writes is
compiled by whatever C compiler `CC` names (else gcc, clang or cc).

Its C is the Rust's byte for byte, checked on Ultra Sun, Ultra Moon, X, Y,
Omega Ruby and Alpha Sapphire (discovery, the front end and every
generated file), with and without the update, a decomp, hot lists, a fast
compile and overrides. The Rust in `tools/3dsrecomp` stays as the
reference until interface 6 changes the code it writes.

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

`recomp.h`, the interface the C runs against (interface 5, described in
`tools/3dsrecomp/docs/interface.md`), is `third_party/recomp.h`, built into
the recompiler and written beside the C; the runtime's loader reads the
same file. The bundled hints are `recomp/hints.txt`.

Tests: `tests/recomp_*_test.cpp`, the Rust's tests translated.
