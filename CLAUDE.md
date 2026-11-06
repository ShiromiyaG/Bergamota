# Bergamota (tools/runtime) — instructions for Claude

A lean 3DS system for the ports (Solstice, Duality, Tidefall): only the syscalls and services
these games use. Private repository `ShiromiyaG/Bergamota` (`recomp-runtime` until 2026-11-05,
`Kumquat` until 2026-11-06); routine local commits allowed, push only when the owner asks.
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
  through `frontend/port_ui.h` (built in with `RUNTIME_PORT_UI=<port>/frontend`; Solstice's
  battle UI).
