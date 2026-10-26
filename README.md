# runtime

A lean 3DS system for the recompiled Pokémon ports (Solstice, Duality,
Tidefall): the kernel calls and system services these games ask for, made
to measure, in place of a whole emulator. Written in C++ (C++20) with a
plain C interface (`include/runtime/runtime.h`), so that the games' C (the
decomp's, else 3dsrecomp's) calls it directly. Zakuro stays the emulator and
the decomp's checker.

What each game uses and why the lists are what they are:
`Solstice/docs/lean-runtime.md` (measured with Zakuro's `ZAKURO_USAGE` and
read from the code with `decomp-tools/ida/ipc_survey.py`).

## The port around it

C++ throughout: SDL3 for the window, input, controllers and audio; Vulkan
for drawing; Dear ImGui for the settings overlay. Until the native
renderer for the engine's graphics exists, the port can draw through
Zakuro's renderer, linked through a C interface.

## Status

Skeleton: the lists of syscalls and services. Next: the kernel and the
native services, translated from Zakuro's (Rust, MIT) and checked against
it on the same session.

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
