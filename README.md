# Bergamota

Bergamota runs Nintendo 3DS games natively on Windows, Linux and Android, from
the games' own code recompiled to C. It is not an emulator: instead of a whole
console, it gives a game only what the game uses (a 3DS kernel, the system
services, sound and a Vulkan renderer). It comes with the recompiler, a player
and a launcher, and it is an SDK for ports: a port of a game is a small
repository of its own, with only what is the port's, and Bergamota does the
rest ([`docs/porting.md`](docs/porting.md)).

**No game is included, and none is needed to build Bergamota.** To play, you
need your own copy of a game and its update, which are recompiled on your
computer.

Bergamota was made for three Pokémon ports, each in a repository of its own:
**Solstice** (Ultra Sun / Ultra Moon), **Duality** (X / Y) and **Tidefall**
(Omega Ruby / Alpha Sapphire).

## Platforms

| Platform | What it needs | How far it is tested |
| --- | --- | --- |
| Windows 10/11, x64 | a GPU with Vulkan 1.1 or later | the main one: everything is developed and measured here |
| Linux, x86-64 and AArch64 | the same, X11 or Wayland | built and run in WSL2 (Ubuntu 24.04, software Vulkan): Ultra Sun plays as on Windows |
| Android, arm64 | Android 10 or later, Vulkan 1.1 or later | one phone so far (a Mali-G52, an entry-level chip): Ultra Sun builds on the phone and plays its intro with the right picture, below full speed in 3D scenes |

On every platform the game is recompiled on the device that plays it, from
the player's own copy: nothing made from a game is ever shipped.

## How it works

1. **The recompiler** (`3dsrecomp`) reads a game and its update and turns the
   game's ARM code (its executable and its CRO modules) into C.
2. **A C compiler** (clang, or Zig when there is none; on Android, the Zig
   inside the app) builds that C into a library. The code a game spends
   nearly all its time in is built first, so the game plays after about a
   minute on a desktop; the rest is built in the background while it is
   played. On a phone only that first part is built by default, and what the
   game is seen to run besides is added on the next start.
3. **The player** loads the library and runs it on Bergamota. Each of the game's
   threads runs on a fiber, its system calls go to Bergamota's kernel and its
   GPU commands go to the Vulkan renderer. Code the library does not have (code
   found only at run time, or changed by a mod) runs in a built-in ARM
   interpreter.
4. **A port** adds what belongs to its games: a `port.toml` that tells the
   launcher which games it is for and, when needed, C for the game's own CPU
   (`native/`) that changes the game, such as widescreen. `sdk/port.py`
   builds, plays and packages it ([`docs/porting.md`](docs/porting.md)).

## Status

- **Pokémon Ultra Sun** plays from the intro movie through the title screen
  and the save to the field, with sound, at full speed on a desktop GPU.
  Ultra Moon's intro plays too. Battles and later scenes still need checking.
- **X, Y, Omega Ruby and Alpha Sapphire** recompile, and Bergamota already has
  what they need beyond Ultra Sun, such as the AAC decoder their music goes
  through. Their ports come after Solstice.
- **Android** is new (2026-11): a port packages as an APK, with the 3DS's
  controls on the screen until a gamepad is connected. On the one phone
  tested, 3D scenes run at 16 to 20 frames a second where the game wants 30;
  the renderer's speed on phone GPUs is the open work there.

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
| Player | an SDL3 window; keyboard, gamepad, and the mouse as the stylus (on a phone, controls on the screen and a finger as the stylus); a settings overlay (F1); single-screen layouts | `frontend/` |
| Launcher | sets up a port's games, recompiles them, and edits the player's graphics and controls | `launcher/` |
| Android app | SDL's activity running the launcher and then the player as one program, with the recompiler and Zig inside the APK | `android/`, `frontend/android_main.cpp`, `sdk/android.py` |
| SDK | the port tool (`port.py`: new, check, symbols, native, build, play, boot, package), the header and builder for a port's native code, the template of a new port | `sdk/` ([docs](docs/porting.md)) |

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

On Linux the same commands work; the packages a build needs (clang, CMake,
Ninja, the Vulkan headers, `glslc` and SDL's X11/Wayland/sound headers) are
the ones `.github/workflows/release.yml` installs. Android is built through
a port's package (`sdk/port.py package --android`, with the Android SDK and
NDK), not by hand: [`docs/porting.md`](docs/porting.md), "Android".

Builds are optimized unless you pick another build type; a Debug build runs
games several times slower. CMake options: `RUNTIME_VULKAN` (the Vulkan
renderer, on when the SDK is found), `RUNTIME_PLAYER` (the player and the
launcher), `RUNTIME_AAC` (the AAC decoder), `RUNTIME_PORT_UI` (a port's
folder with a UI of its own for the player and `boot`, as Solstice's
`frontend/`; `frontend/port_ui.h`).

The ports include Bergamota as a submodule (`external/runtime`), and its
port tool (`sdk/port.py package`) builds these programs from it for their
packages. To link the library into a
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
**F11** switches fullscreen. On Android the round button at the top of the
screen, or the back button, opens the settings.

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
- On Android, what the program prints goes to the system's log
  (`adb logcat -s Bergamota`), and
  `adb shell setprop debug.bergamota.env "RUNTIME_STATS=1"` sets these
  variables for the next start.

All of them, with the environment variables: [`docs/tools.md`](docs/tools.md).

## Documents

| File | About |
| --- | --- |
| [`docs/porting.md`](docs/porting.md) | porting a game: a port's files, the port tool, native code, the package for each platform (Android included), what stays stable |
| [`docs/recompiler.md`](docs/recompiler.md) | the recompiler and the interface its C runs against |
| [`docs/vulkan-renderer.md`](docs/vulkan-renderer.md) | the renderer's design and milestones |
| [`docs/tools.md`](docs/tools.md) | `boot`, `compare.py`, the environment variables, mods and texture packs |
| [`docs/problems.md`](docs/problems.md) | known problems, and how the solved ones were found |
| [`docs/organization.md`](docs/organization.md) | the plan for organizing the code |

What each game asks of the system, and so why Bergamota has what it has, is in
Solstice's `docs/lean-runtime.md`.

## License

MIT (`LICENSE`), written by ShiromiyaG with Claude (Anthropic). Parts are
translated from Zakuro (MIT, © 2026 fearkov, `LICENSE-zakuro`) and the
recompiler from 3dsrecomp (MIT, by the same author, `LICENSE-3dsrecomp`); each
translated file says so at its top. Nothing derived from any game is in this
repository. Nintendo 3DS is a trademark of Nintendo; Bergamota is not affiliated
with Nintendo.
