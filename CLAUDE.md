# Bergamota (tools/runtime) — instructions for Claude

A lean 3DS system for the ports (Solstice, Duality, Tidefall): only the syscalls and services
these games use. Public repository `ShiromiyaG/Bergamota` (since 2026-11-09; `recomp-runtime` until
2026-11-05, `Kumquat` until 2026-11-06); routine local commits allowed, push only when the owner
asks. Public: nothing of the games, no keys or tokens, nothing private of the owner's in it.
Docs in English.

- C++20, plain C interface (`include/runtime/runtime.h`); CMake + Ninja + clang (decided
  2026-10-26: the ports are C/C++, with SDL3, Vulkan and Dear ImGui).
- Lists and reasons: `ports/Solstice/docs/lean-runtime.md`.
- The game's threads are cooperative, each on a fiber (interface 6), on a host thread of their
  own; the GPU runs on its own (`AsyncGpu`), and the player shows the frames on the main thread.
- Code translated from Zakuro (Rust, MIT): copyright notice at the top of each translated file;
  check it against Zakuro in the same session.
- Nothing derived from the games goes in here.
- Nothing made for one game either (decided 2026-11-06): it goes in its port, as data the
  runtime reads (`single-screen.toml`, `port.toml`, `decomp.txt`'s options) or as host code
  through `frontend/port_ui.h` (built in with `RUNTIME_PORT_UI=<port>/frontend`; each
  port's look for the F1 menu, `frontend/menu.cpp`).
- An SDK for ports (decided 2026-11-06): a port is data and `native/`, with no change to the
  runtime. `sdk/` holds the port tool (`port.py`: new, check, symbols, native, build, play,
  boot, package, driven by `port.toml`), `include/port.h`, the native code's builder
  (`native.py`, which decomp-tools' `build.py` also goes through) and the template; the
  contract is `docs/porting.md`. A change that breaks a port's files raises `SDK_VERSION`
  (`sdk/portfile.py`) and says in `docs/porting.md` what to change.
- Windows, Linux and Android (2026-11-09; `docs/porting.md`, "Android"): fibers switched by our
  own code on x86-64 and AArch64 (`src/fiber.cpp`), Vulkan through volk with 1.1 devices on
  extensions, glslang where there is no shaderc, the game built on the phone by 3dsrecomp and
  Zig without a C library (`port.py package --android`). Linux is tested in WSL2 (Ubuntu 24.04,
  lavapipe); Android only builds here (no emulator: the owner tests on a phone).
- `.github/workflows/release.yml` builds the launcher, player, 3dsrecomp, the Android program
  and the SDK on a version tag (`v*`). Bump versions or push tags only when the owner asks.
