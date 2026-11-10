# Porting a game (the SDK)

Bergamota is an SDK for native ports of 3DS games: a port is a small
repository of its own that holds **only what is the port's** (data and C),
and Bergamota (the `external/runtime` submodule) does the rest: it
recompiles the player's own copy of the game, runs it on its lean 3DS, and
gives the port a launcher, a player and a package.

The test of the SDK: a new port is data and `native/`, and needs no change
to Bergamota. What a game needs from the system that Bergamota lacks (a
service, a syscall, a GPU feature) goes into Bergamota, written for every
game; what is one game's goes into its port.

## What a port is

| Path | What it is | Read by |
| --- | --- | --- |
| `port.toml` | the port: its name, its games, its native code, its package | the launcher, `port.py` |
| `native/` | C for the game's own CPU (its ARM11) that changes the game | `port.py native` |
| `single-screen.toml` | which layout each part of the game is shown in, in single-screen mode | the player |
| `hot/<title id>.txt` | each game's hottest functions, for the fast compile (optional) | the launcher |
| `frontend/` | host code for a UI of the port's own over the game (optional) | the player, built in |
| `packaging/README.md` | the README players get | `port.py package` |
| `packaging/THIRD_PARTY.md` | the port's own third-party notices (optional) | `port.py package` |
| `packaging/package.ps1`, `tools/port.py` | thin wrappers over Bergamota's `sdk/port.py` | you |
| `extracted/`, `build/`, `dist/` | made from the game or for it: never committed | |

Nothing derived from a game goes into a port's repository: no code, no
assets, no bytes. Addresses, names and the port's own C are fine.

## Starting a port

```sh
python -I <runtime>/sdk/port.py new MyPort --name MyPort \
    --game 0004000000055D00:"Pokémon X" --game 0004000000055E00:"Pokémon Y"
cd MyPort
git init && git submodule add https://github.com/ShiromiyaG/Bergamota.git external/runtime
```

`new` copies `sdk/template/` and writes `port.toml` for the games. Build
Bergamota once (its README: CMake, Ninja, clang, the Vulkan SDK), then set
each game up in the port's own launcher, which keeps the dump and the
update, unpacks the update, and makes a first build:

```sh
external/runtime/build/launcher --port port.toml
```

From then on `tools/port.py` finds the game's files by itself (the
launcher's `%APPDATA%\<Name>\config.toml`, and the update unpacked in
`%APPDATA%\<Name>\<title id>\update`). An update unpacked by hand into the
port's `extracted/` (any folder name: its `exheader.bin` says whose it is)
is found too, and `--rom`, `--update`, `--unpacked` name them outright.

## The port tool

`python -I tools/port.py <command>` (or `<runtime>/sdk/port.py <command>
--port <folder>`), for the port's first game or the one `--game` names (its
title id or its end, its `key`, or a word of its name):

| Command | What it does |
| --- | --- |
| `check` | what `port.toml` says, and what is set up here for each game |
| `symbols` | `3dsrecomp symbols` into `build/symbols-<title>` (seconds; done by `native` when missing) |
| `native` | `native/` built for the game into `build/native-<title>` |
| `build` | `native`, then the game recompiled with it into `build/lib-<title>`; `--native-only` for `native/` alone (the runtime interprets what the library lacks), `--fast` for the hot list |
| `play` | the game in the player with what `build` made; saves in `build/user`, never the launcher's |
| `boot` | the game without a window: how far it got, where the time went, the service commands nothing answers |
| `package` | the package players download, into `dist/` |

`build` and `native` take `--extra <folder>` (and `-I <folder>`) for more
sources whose functions lose to the port's: a decomp's, under the port.

## A new game, step by step

1. **Does it boot?** `port.py boot` (or `play`). The runtime prints each
   service command nothing answers (`runtime: unimplemented ...`) and, at
   the end, where the game's threads are. A gap there is Bergamota's to
   fill, for every game: a service in `src/services/`, a syscall in
   `src/svc.cpp`, a GPU feature in `src/pica/` or `src/vk/`. `tools/compare.py`
   runs a stretch of the game here and on Zakuro and says where they part.
2. **Find the engine's seams.** `port.py symbols` lists what the game
   exports (its `static.crs` and CROs) and every function the analysis
   found, which Ghidra or IDA can import; the names you give go in a symbol
   file of the port's (`[native] symbols`, below).
3. **Change the game in `native/`** (below): a widescreen hook, a frame
   rate, an option. Prefer wrapping to rewriting.
4. **Lay out the single screen** (`single-screen.toml`): which scenes are
   made for both screens.
5. **Test the update**: once the port works with one, give it in
   `port.toml` (`update`, `update_name`); the launcher warns about others.
6. **Package** it.

## Native code

C (or C++) for the game's ARM11, in `native/`. `port.py native` compiles it
with clang for armv6k with VFPv2, floats passed the softfp way as the
game's own calls do, freestanding, with `sdk/include/port.h` on the include
path; links it at `0x07800000` (`[native] base`) against the game's
functions by address; and writes `decomp.bin` (the code) and `decomp.txt`
(what it changes). 3dsrecomp recompiles it with the game (`--decomp`), and
the runtime loads it into the game's memory, so it runs as the game's own
code: it calls the game's functions directly, including ones that wait.

```c
#include "port.h"

/* the game's function, called by its address (t_ for Thumb) */
extern void Bloom_Ghosting(u32 self, u32 a, u32 b, u32 c) __asm__("f_0042F334");

/* an option in the player's menu and the launcher, written live */
PORT_OPTION(no_ghosting, 1, "No bloom ghosting", "Leaves out the trail the bloom draws behind what moves.");

static void ghosting(u32 self, u32 a, u32 b, u32 c) {
    if (!port_no_ghosting) Bloom_Ghosting(self, a, b, c);
}

/* the vtable's entry that pointed at the game's function points here */
PORT_PATCH(0x0064E570, ghosting);
```

### Names

What the builder reads, as assembler names (8 hex digits, no `0x`):

| Name | Meaning |
| --- | --- |
| `f_XXXXXXXX`, `t_XXXXXXXX` | a function of the game's executable, ARM or Thumb: call it |
| `f_<Module>_XXXXXXXX` | a function of a module (a CRO), by its offset from where the module loads; the runtime fills a word with its address as it loads |
| `r_XXXXXXXX`, `rt_XXXXXXXX` | this function replaces the game's at that address (ARM, Thumb): a jump is written over its start |
| `r_<Module>_XXXXXXXX` | replaces a module's, as the module loads |
| any name in a symbol file | a function by name (`[native] symbols` and `build/symbols-<title>/executable.txt`) |

`port.h` gives the rest:

- `PORT_PATCH(0xADDRESS, value)`: a word of the executable (a vtable's
  entry, a pointer in a table) set to `value` as the game loads. The way
  in when the port only wraps the game: its function is called as it is.
- `PORT_OPTION(name, default, label, help)` and
  `PORT_OPTION_RANGE(name, default, min, max, label, help)`: the variable
  `u32 port_<name>`, shown in the player's menu (F1, Port) and the
  launcher's Graphics page, saved in the player's `[port]` settings and
  written into the game's memory as it changes.
- any global named `port_<something>` is visible to the host by name: the
  player writes `float port_top_aspect` (the top screen's shape as shown,
  16:9 or 21:9, 0 for the 3DS's own) when the port defines it, and a port's
  own UI reads what the port's code leaves in its variables.
- `MODULE_ADDRESS`, `MODULE_THUMB`, `MODULE_DATA`: addresses in a module,
  known once it loads.
- `FIELD(type, base, offset)` and the game's integer types.

A function shorter than the jump that replaces it, or one the game enters
past its start, is left to the game (the builder says which): it reads
`build/symbols-<title>` and the unpacked update for that.

Run-time helpers clang calls (division, `memcpy`) are the game's own when a
symbol file names them, else Bergamota's (`sdk/lib/aeabi.c`).

### Other versions of a game

`native/`'s addresses are one game's. Each other game the port is for
(the other version of a pair, another region) takes them by signature:

```toml
[[game]]
title = "0004000000055E00"
native_from = "0004000000055D00"   # native/'s addresses are X's
decomp = "decomp-y"
```

Each address is looked up in this game's code by the bytes around it in the
first game's (`sdk/addresses.py`, from the player's own copies; no bytes are
kept). A file that uses an address not found there is left out of that
game's build, with a line saying which: that feature is off for that game.
Both games' updates must be set up (or unpacked in `extracted/`).

### What `decomp.txt` holds

One item a line, numbers in hex with `0x`:

| Line | Meaning |
| --- | --- |
| `base <address>` | where `decomp.bin` loads (a page) |
| `entry <address>` | a function of the code, for 3dsrecomp |
| `hook <old> <new> <returns>` | the executable's function at old jumps to new (old odd: Thumb); returns is `void`, `int`, `long`, `float` or `double` |
| `patch <address> <value>` | a word of the executable set as the game loads |
| `module_hook <Module>.cro <offset> <new> <returns>` | a module's function replaced as it loads |
| `module_import <Module>.cro <offset> <slot> [seg<n>]` | the word at slot filled with an address of the module once it loads |
| `symbol <name> <address>` | a variable the host reads or writes |
| `option <symbol> bool <default> <label>\|<help>` | a toggle |
| `option <symbol> int <default> <min> <max> <label>\|<help>` | a number |

The runtime ignores lines it does not know (a newer builder's).

## `port.toml`

```toml
name = "Solstice"            # the launcher's window and %APPDATA% folder, the package
player = "SolsticeGame"      # the player in the package: game/<player>.exe
about = "..."                # a line on the port, in the launcher
sdk = 2                      # the SDK version the port is written for

[native]                     # port.py's
sources = ["native"]         # folders of C, the port's
include = []                 # more include folders
symbols = ["tools/ghidra/names-us-v1.2.txt"]   # name address kind (f, t, l) a line
base = 0x07800000            # where the code loads

[package]                    # port.py package's
ui = "frontend"              # host code built into the player (frontend/port_ui.h)
icon = "packaging/icon.ico"  # the port's icon, in the launcher's and the player's .exe
files = [ { from = "ui/art", to = "game/ui/art" } ]   # more files, into the package
readme = "packaging/README.md"
third_party = "packaging/THIRD_PARTY.md"

[[game]]                     # once per game
title = "00040000001B5000"   # its title id
name = "Pokémon Ultra Sun"
key = "us"                   # a short name for --game
color = "#F08A24"            # its color in the launcher
update = 2048                # the update it is tested with (the TMD's version)
update_name = "v1.2"         # and as players know it
decomp = "decomp"            # its native code in the package, a folder beside the launcher
native_from = "..."          # another game whose addresses native/ uses
```

The launcher reads `name`, `player`, `about` and each game's `title`,
`name`, `update`, `update_name`, `decomp` and `color`; the rest is the
tools'.

## `single-screen.toml`

```toml
[[scene]]
name = "movie"
modules = ["MovieLib"]   # all loaded (by the CRO's name); none: any time
layout = "stacked"       # "panel", "stacked" or "top"
lift = 46                # "panel": the panel that many of the top screen's 240 rows up
```

The first scene whose modules are all loaded picks the layout; with none,
"panel" (`frontend/scenes.h`). `boot` composes shots as the player shows
them with `RUNTIME_LAYOUT=single` and `RUNTIME_LAYOUTS=<file>`.

## The port's host code: its menu's look and its own UI

What the port adds on the host's side is C++ in the port's `frontend/`,
whose `CMakeLists.txt` makes a `port_ui` library defining both of
`frontend/port_ui.h`'s functions, each returning null for what the port has
not:

- `make_port_menu(program_id)`: the look of the player's menu (F1) for each
  of the port's games, a `PortMenu`. `start(scale)` adds its fonts to Dear
  ImGui's atlas (making one `io.FontDefault`) and sets `ImGui::GetStyle()`,
  its sizes times the display's scale; `header(title)` draws across the top
  of the menu's window, which then has no title bar, leaves the cursor under
  it, and returns false when its close button is pressed. The runtime offers
  the port Nunito, the launcher's, as `nunito_semibold.inc` and
  `nunito_black.inc` (arrays `NUNITO_SEMIBOLD`, `NUNITO_BLACK`) on the
  library's include path. Solstice, Duality and Tidefall each have their own
  (`frontend/menu.cpp`).
- `make_port_ui(decomp)`: something drawn over the game, a `PortUi`: it
  reads what the port's native code leaves in the game's memory each
  vertical blank, draws with Dear ImGui over the top screen, and can touch
  the bottom screen. No port has one now.

`[package] ui` builds it into the package's player
(`-DRUNTIME_PORT_UI=<folder>`); for `play`, build the runtime with the same
option.

## Icons

`[package] icon`, a `.ico` of the port's own, goes into the launcher's and
the player's `.exe` (`-DRUNTIME_PORT_ICON=<file>`): Explorer shows it, and
their windows take it. Once the game starts, the player's window takes the
game's own icon instead, read from the player's dump (its SMDH, the large
and the small one; `runtime::read_icon` in `runtime/dump.h`), so nothing of
the game's is in the port or the package.

## The package

`port.py package` (or `pwsh packaging/package.ps1 [-Runtime <checkout>]`)
builds the runtime in Release (in `--runtime-build`, its `build/` by
default) with the port's UI, then lays out `dist/<Name>/`:

| | |
| --- | --- |
| `<Name>.exe`, `port.toml` | the launcher, made the port's by its port.toml |
| `3dsrecomp/` | the recompiler, run on the player's computer |
| `game/<player>.exe`, `game/shaders/`, `game/single-screen.toml` | the player |
| `<decomp>/` | each game's native code, built here |
| `hot/`, `README.md`, `[package] files` | the port's |
| `LICENSES/` | Bergamota's and its parts', and `THIRD_PARTY.md` with the port's own at its end |

and zips it as `dist/<Name>-<version>-<platform>.zip`. A game whose
addresses are carried (`native_from`) goes without native code, with a
warning, when its update is not set up where the package is built.

On Linux the same command, run there, lays out the same folder without
`.exe` and with the port's `icon.png` and `icon.bmp` beside the launcher
(its shortcut is a `.desktop` entry, its window takes the BMP), as
`dist/<Name>-<version>-linux-<machine>.tar.gz`. The launcher builds the game
with clang, else the system's `cc`, else Zig downloaded for Linux. Needed to
build there: clang (or gcc), CMake, Ninja, the Vulkan headers and `glslc`,
and SDL's X11/Wayland/ALSA/PulseAudio headers (the packages are listed in
`.github/workflows/release.yml`).

## Android

`port.py package --android [--version x.y.z]` (`sdk/android.py`) makes
`dist/<Name>-<version>-android-arm64.apk`, for arm64 phones with Vulkan 1.1
or later (Adreno, Mali, PowerVR: a device on 1.1 or 1.2 takes the extensions
1.3 took in, `src/vk/device.cpp`; `VK_KHR_push_descriptor` is needed). It
needs the Android SDK (`ANDROID_HOME`, or Android Studio's), an NDK in it
(or `ANDROID_NDK_HOME`), CMake, Ninja and a JDK Gradle 9 runs on.

- The runtime is built with the NDK into `build-android/`: `libmain.so`
  (the launcher, then the player, one program SDL's activity runs;
  `frontend/android_main.cpp`) and `3dsrecomp`, with the port's UI.
- The game is built on the phone, as on a computer: 3dsrecomp
  (`lib3dsrecomp.so`) with Zig for AArch64 (`libzig.so`; the part of its lib
  folder that C for `aarch64-linux-musl` takes is an asset, unpacked on the
  first start), the library linked without a C library (`recomp/compile.cpp`,
  `freestanding()`). Android runs programs only from the APK's library
  folder, so both are there, named as libraries.
- The game is picked with Android's picker; the launcher keeps its
  `content://` and reads it as `/proc/self/fd/N` (`launcher/android.cpp`).
- The port's files (port.toml, the scenes, the hot lists, each game's native
  code, the licenses) are assets under `port/`, unpacked beside the
  launcher's settings when the APK changes.
- `[android] id` in port.toml is the app's id (else `org.bergamota.<name>`);
  its icon is `icon.png` beside `[package] icon`. Signed with Android's debug
  key unless `ANDROID_KEYSTORE`, `ANDROID_KEYSTORE_PASSWORD`,
  `ANDROID_KEY_ALIAS` and `ANDROID_KEY_PASSWORD` name the port's own.
- In the game: the 3DS's controls on the screen (`frontend/touch.h`) until a
  gamepad is connected, a finger elsewhere touching the bottom screen; the
  round button at the top, or Android's back button, opens the menu.

## What stays stable

`sdk` in `port.toml` is the SDK version a port is written for;
`port.py` refuses a port newer than itself. What a port relies on, and what
changing it means:

| Contract | Where |
| --- | --- |
| `port.toml`'s keys | here, `sdk/portfile.py`, `launcher/port.h` |
| the names and macros of native code | `sdk/include/port.h` |
| `decomp.txt`'s lines | here, `src/decomp.cpp` |
| the host symbols the player writes (`port_top_aspect`, options) | here, `frontend/player.cpp` |
| a port's UI | `frontend/port_ui.h` |
| `single-screen.toml` | `frontend/scenes.h` |

A change that breaks one of them raises the SDK's version
(`sdk/portfile.py`, `SDK_VERSION`) and says here what to change:

- 2 (2026-11-09): a port's `frontend/` defines `make_port_menu` too (its
  menu's look; `return nullptr;` for none), beside `make_port_ui`. The
recompiled code's interface (`recomp.h`, interface 6) is not part of it: the
game is recompiled on the player's computer by the 3dsrecomp the package
carries, so it always matches the runtime.
