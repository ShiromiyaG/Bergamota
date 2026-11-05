# Kumquat

Kumquat runs Nintendo 3DS games natively on the PC, from the games' own code
recompiled to C. It is not an emulator: instead of a whole console, it gives a
game only what the game uses (a 3DS kernel, the system services, sound and a
Vulkan renderer). It comes with the recompiler, a player and a launcher.

**No game is included, and none is needed to build Kumquat.** To play, you
need your own copy of a game and its update, which are recompiled on your
computer.

Kumquat was made for three Pokémon ports, each in a repository of its own:
**Solstice** (Ultra Sun / Ultra Moon), **Duality** (X / Y) and **Tidefall**
(Omega Ruby / Alpha Sapphire).

## How it works

1. **The recompiler** (`3dsrecomp`) reads a game and its update and turns the
   game's ARM code (its executable and its CRO modules) into C.
2. **A C compiler** (clang, or Zig when there is none) builds that C into a
   library.
3. **The player** loads the library and runs it on Kumquat. Each of the game's
   threads runs on a fiber, its system calls go to Kumquat's kernel and its GPU
   commands go to the Vulkan renderer. Code the library does not have (code
   found only at run time, or changed by a mod) runs in a built-in ARM
   interpreter.
4. **A port** adds what belongs to its games: a `port.toml` that tells the
   launcher which games it is for and, when needed, C for the game's own CPU
   (`native/`) that changes the game, such as widescreen.

## Status

- **Pokémon Ultra Sun** plays from the intro movie through the title screen
  and the save to the field, with sound, at full speed on a desktop GPU.
  Ultra Moon's intro plays too. Battles and later scenes still need checking.
- **X, Y, Omega Ruby and Alpha Sapphire** recompile, and Kumquat already has
  what they need beyond Ultra Sun, such as the AAC decoder their music goes
  through. Their ports come after Solstice.

## What is in it

| Part | What it does | Where |
| --- | --- | --- |
| Recompiler | turns ARM, Thumb and VFP code into C, CRO modules included; a C++ translation of [3dsrecomp](https://github.com/fearkov/3dsrecomp) | `recomp/` ([docs](docs/recompiler.md)) |
| Kernel and memory | threads, waits, the address arbiter, the memory regions and the system calls the games make | `src/kernel.cpp`, `src/svc.cpp`, `src/memory.cpp` |
| Interpreter | ARM, Thumb and VFPv2, for code the library does not have | `include/runtime/interpreter.h` |
| Services | srv, APT (with the shared font), cfg, fs, hid, ldr:ro (which loads the CRO modules), y2r, gsp and ptm; offline answers for the online services | `src/services/` |
| Game files | reads `.3ds` and `.cia` dumps (NCSD, NCCH, CIA, ExeFS, RomFS) and unpacks updates | `src/dump.cpp` |
| Sound | the DSP's 24 voices and its mixer; AAC through FDK AAC | `src/audio.cpp`, `src/aac.cpp` |
| Graphics | the PICA200 GPU on Vulkan: its vertex shaders translated for the GPU, a resolution scale, widescreen, extra frames for 60 FPS and above, texture packs in Azahar's format | `src/pica/`, `src/vk/` ([docs](docs/vulkan-renderer.md)) |
| Mods | a port's native code, `code.ips` patches and Gateway cheats (`cheats.txt`), none of which needs the game recompiled | `src/decomp.cpp`, `src/cheats.cpp` |
| Player | an SDL3 window; keyboard, gamepad, and the mouse as the stylus; a settings overlay (F1); single-screen layouts | `frontend/` |
| Launcher | sets up a port's games, recompiles them, and edits the player's graphics and controls | `launcher/` |

The C interface that the games' code calls is `include/runtime/runtime.h`.

## Building

You need CMake 3.25+, Ninja, clang (or MSVC) and the
[Vulkan SDK](https://vulkan.lunarg.com/) (for `glslc` and the headers).
CMake downloads SDL3, Dear ImGui, toml++ and FDK AAC by itself.

```sh
cmake -S . -B build -G Ninja -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++
cmake --build build
ctest --test-dir build
```

| Program | What it is |
| --- | --- |
| `build/player` | runs a game in a window |
| `build/launcher` | the ports' launcher; each port's package renames it (`Solstice.exe`, ...) |
| `build/3dsrecomp` | the recompiler |
| `build/boot` | runs a game without a window, for measuring and testing |

Builds are optimized unless you pick another build type; a Debug build runs
games several times slower. CMake options: `RUNTIME_VULKAN` (the Vulkan
renderer, on when the SDK is found), `RUNTIME_PLAYER` (the player and the
launcher), `RUNTIME_AAC` (the AAC decoder).

The ports include Kumquat as a submodule (`external/runtime`), and their
package scripts build these programs from it. To link the library into a
program of your own, `add_subdirectory` this folder and link `recomp_runtime`.

## Running

The usual way is through a port: its launcher asks for your game and its
update, recompiles them and opens the player.

The player also runs on its own:

```sh
build/player                  # asks for the game's files the first time
build/player --setup          # asks for them again
build/player --config <file>  # uses another settings file
```

Its settings are in `%APPDATA%\runtime\config.toml` (on other systems, the
platform's usual place). While playing, **F1** opens the settings and
**F11** switches fullscreen.

To recompile a game by hand (all the options are in
[`docs/recompiler.md`](docs/recompiler.md)):

```sh
build/3dsrecomp build <game .3ds> <output folder> --mods <update, unpacked>
```

## Tools for development

- `build/boot` runs a game without a window and reports how far it got and
  where the time went. It can run on a virtual clock (the same input gives
  the same run), replay recorded input and save screenshots.
- `tools/compare.py` runs a stretch of a game here and on Zakuro with the same
  input and reports where they differ (screens, service calls, saves).
- `RUNTIME_STATS=1` makes the player print where each second went.

All of them, with the environment variables: [`docs/tools.md`](docs/tools.md).

## Documents

| File | About |
| --- | --- |
| [`docs/recompiler.md`](docs/recompiler.md) | the recompiler and the interface its C runs against |
| [`docs/vulkan-renderer.md`](docs/vulkan-renderer.md) | the renderer's design and milestones |
| [`docs/tools.md`](docs/tools.md) | `boot`, `compare.py`, the environment variables, mods and texture packs |
| [`docs/problems.md`](docs/problems.md) | known problems, and how the solved ones were found |
| [`docs/organization.md`](docs/organization.md) | the plan for organizing the code |

What each game asks of the system, and so why Kumquat has what it has, is in
Solstice's `docs/lean-runtime.md`.

## License

MIT (`LICENSE`), written by ShiromiyaG with Claude (Anthropic). Parts are
translated from Zakuro (MIT, © 2026 fearkov, `LICENSE-zakuro`) and the
recompiler from 3dsrecomp (MIT, by the same author, `LICENSE-3dsrecomp`); each
translated file says so at its top. Nothing derived from any game is in this
repository. Nintendo 3DS is a trademark of Nintendo; Kumquat is not affiliated
with Nintendo.
