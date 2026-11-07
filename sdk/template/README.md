# @NAME@

A native PC port of **@GAMES@**, made by recompiling the game's own code
instead of emulating the console, on
[Bergamota](https://github.com/ShiromiyaG/Bergamota) (the `external/runtime`
submodule).

**No game code or data is included, and none is distributed.** You need your
own copy of the game (and its update), decrypted; @NAME@ recompiles it on
your computer.

| Game | Title ID |
| --- | --- |
@GAME_ROWS@

## Developing

Bergamota's `docs/porting.md` is the guide. With its toolchain (CMake,
Ninja, LLVM/clang, the Vulkan SDK) and Python 3:

```sh
git submodule update --init
cmake -S external/runtime -B external/runtime/build -G Ninja -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++
cmake --build external/runtime/build
external/runtime/build/launcher --port port.toml   # set the game up once
python -I tools/port.py check                       # what is set up
python -I tools/port.py build                       # native/ and the game, into build/
python -I tools/port.py play                        # play it (saves in build/user)
python -I tools/port.py package                     # the package, into dist/
```

## What is in this repository

| Path | What it is |
| --- | --- |
| `port.toml` | the port: its games, its native code, its package |
| `native/` | C for the game's own CPU that changes it |
| `single-screen.toml` | which layout each part of the game uses in single-screen mode |
| `packaging/README.md` | the README shipped to players |
| `tools/port.py` | Bergamota's port tool, for this port |
