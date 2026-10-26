# runtime

A lean 3DS system for the recompiled Pokémon ports (Solstice, Duality,
Tidefall): the kernel calls and system services these games ask for, made
to measure, in place of a whole emulator. Written in C++ (C++20) with a
plain C interface (`include/runtime/runtime.h`), so that the games' C (the
decomp's, else 3dsrecomp's) calls it directly. Zakuro stays the emulator and
the decomp's checker.

What each game uses and why the lists are what they are:
`ports/Solstice/docs/lean-runtime.md` (measured with Zakuro's `ZAKURO_USAGE` and
read from the code with `tools/decomp-tools/ida/ipc_survey.py`).

## The port around it

C++ throughout: SDL3 for the window, input, controllers and audio; Vulkan
for drawing; Dear ImGui for the settings overlay. Until the native
renderer for the engine's graphics exists, the port can draw through
Zakuro's renderer, linked through a C interface.

## Status

- **Kernel** (`include/runtime/kernel.h`, `src/kernel.cpp`, `src/svc.cpp`):
  threads, kernel objects and handles, waits (events, mutexes, the address
  arbiter, sleeps and timeouts), and the 32 syscalls, translated from
  Zakuro's. A guest thread is a `Context` (3dsrecomp's, `third_party/recomp.h`):
  the games' code gives control back on every syscall, so switching
  threads is picking another context, with no fibers. Tests:
  `tests/kernel_test.cpp`.
- **Memory** (`memory.h`, `src/memory.cpp`): the 3DS's physical memories
  and FCRAM's regions, the page tables the code reads through, mappings,
  the configuration and shared pages, and the memory syscalls.
- **The loop** (`system.h`, `src/system.cpp`): runs each thread's code from
  its r15 through the port's lookup, handles the syscall it stopped at, and
  sleeps the host when every thread waits. No interpreter: everything has
  to be C. Tests: `tests/system_test.cpp` (two threads, written by hand as
  3dsrecomp writes code, meet on the address arbiter).
- Next: the services (`srv`, `APT`, `cfg`, `fs`, `hid`, `gsp`, `dsp`,
  `y2r`, `ldr:ro`) and loading the game's code and modules, then the first
  run of the real game against Zakuro.

## Building

```sh
cmake -S . -B build -G Ninja -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++
cmake --build build
```

A port adds it with `add_subdirectory(../runtime runtime)` and links
`recomp_runtime`.

## License

MIT. Parts are translated from Zakuro (MIT, Copyright (c) 2026 fearkov,
`LICENSE-zakuro`); each such file says so at its top.

Nothing derived from the games is ever committed here.
