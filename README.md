# runtime

A lean 3DS system for the recompiled Pokémon ports (Solstice, Duality,
Tidefall): the kernel calls, system services, sound and graphics these games
use, made to measure, in place of a whole emulator. C++20 with a plain C
interface (`include/runtime/runtime.h`), so the games' C (a decomp's, else
[3dsrecomp](https://github.com/fearkov/3dsrecomp)'s) calls it directly.

No game is included or needed to build it. To run anything you need your own
copy of a game, its update, and a library of its code recompiled locally.

## What works

Pokémon Ultra Sun runs on it, in a window, from the intro movie through the
title and the save to the field, with sound, on the runtime's own Vulkan
renderer, at full speed on a desktop GPU.

| Part | Where | State |
| --- | --- | --- |
| Kernel: threads, objects, waits, the address arbiter, 32 syscalls | `kernel.h`, `src/kernel.cpp`, `src/svc.cpp` | done; tests in `tests/` |
| Memory: FCRAM regions, VRAM, page tables, the memory syscalls, guards over what the GPU drew | `memory.h`, `src/memory.cpp` | done |
| The run loop: cooperative threads on one host thread, a virtual clock for reproducible runs | `system.h`, `src/system.cpp` | done |
| ARM/Thumb/VFPv2 interpreter, for code the C does not have | `interpreter.h` | done |
| Services: srv, APT, cfg, fs (RomFS from the .3ds and update .cia, saves as folders), hid, ldr:ro with the CRO loader, y2r, gsp, offline answers for the online services | `services.h`, `src/services/`, `fs.h`, `cro.h` | what USUM uses |
| Sound: the DSP's 24 voices (PCM8/16, ADPCM, filters) and mixer | `audio.h`, `src/audio.cpp` | done; AAC (X/Y's music) not yet |
| Graphics: the PICA200 front end, the vertex stage on the CPU, a Vulkan back end | `pica.h`, `src/pica/`, `vulkan_renderer.h`, `src/vk/` | first pixels to the field; lighting next ([plan](docs/vulkan-renderer.md)) |
| The player: SDL3 window, keyboard, gamepad, mouse as the stylus, `config.toml`, a launcher and an ImGui overlay (F1) | `frontend/` | done |

## Building

Needs CMake 3.25+, Ninja, clang (or MSVC), the [Vulkan SDK](https://vulkan.lunarg.com/)
(for `glslc` and the headers) and, for Zakuro's renderer as a fallback, Rust.
SDL3, Dear ImGui and toml++ are fetched by CMake at configure time.

```sh
cmake -S . -B build -G Ninja -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++
cmake --build build
ctest --test-dir build
```

Builds are optimized unless a build type is given (a Debug runtime runs the
games several times slower). Options: `RUNTIME_VULKAN` (the runtime's
renderer), `RUNTIME_ZAKURO_GPU` (Zakuro's, through `bridge/`, needs cargo and
a [Zakuro](https://github.com/fearkov/zakuro) checkout beside this one),
`RUNTIME_PLAYER` (the window).

A port adds it with `add_subdirectory(../runtime runtime)` and links
`recomp_runtime`.

## Running

```sh
# the window: asks for the game's files the first time (or with --setup)
build/player
build/player <exheader.bin> <code.bin> <recompiled library> <game .3ds> <update .cia> <saves folder>
```

Settings live in `%APPDATA%\runtime\config.toml` (or the platform's own
place); F1 opens the overlay, F11 switches fullscreen. `RUNTIME_STATS=1`
prints where each second went.

`build/boot` runs a game without a window, for measuring and checking:
`docs/tools.md` lists what it takes, and `tools/compare.py`, which runs a
stretch of a game here and on Zakuro with the same recorded input and reports
where they part (screens, service calls, saves).

## Documents

- [`docs/vulkan-renderer.md`](docs/vulkan-renderer.md): the renderer's design and milestones.
- [`docs/tools.md`](docs/tools.md): `boot`, `compare.py`, the environment variables.
- What each game asks of the system, and why the lists are what they are:
  `docs/lean-runtime.md` in the Solstice repository.

## License

MIT (`LICENSE`). Parts are translated from Zakuro (MIT, Copyright (c) 2026
fearkov, `LICENSE-zakuro`); each such file says so at its top. Nothing
derived from any game is in this repository.
