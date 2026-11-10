"""A port's package, the folder and the zip players download, from its
port.toml (port.py package; docs/porting.md, "The package"):

    dist/<Name>/
        <Name>.exe            the launcher, which port.toml beside it makes the port's
        port.toml
        README.md             packaging/README.md, for players
        3dsrecomp/            the recompiler the launcher runs on the player's computer
        game/<player>.exe     the player, with its shaders and the single screen's scenes
        <decomp>/             each game's native code (decomp.bin, decomp.txt), built here
        hot/                  the lists of each game's hottest functions, if the port has them
        LICENSES/             the runtime's and its parts', and the port's
    dist/<Name>-<version>-<platform>.zip

The runtime is configured and built for it first (in --runtime-build, its
build/ by default), with the port's own UI (port.toml's [package] ui) built
into the player. Nothing of a game goes in: the native code is the port's
own, and the hot lists are addresses and counts.
"""

import os
import platform
import shutil
import subprocess
import sys

import native
import portfile
from portfile import PortError

SDK = os.path.dirname(os.path.abspath(__file__))
RUNTIME = os.path.dirname(SDK)
EXE = ".exe" if os.name == "nt" else ""

# the Visual C++ runtime the player and the launcher are built against
# (shaderc from the Vulkan SDK links the DLLs); Microsoft lets the redist
# folder's DLLs be shipped beside a program
VC_DLLS = ("msvcp140.dll", "msvcp140_atomic_wait.dll", "vcruntime140.dll", "vcruntime140_1.dll")


def copy(source, target):
    os.makedirs(os.path.dirname(target), exist_ok=True)
    if os.path.isdir(source):
        shutil.copytree(source, target, dirs_exist_ok=True)
    else:
        shutil.copy2(source, target)


def vc_redist():
    vswhere = os.path.join(os.environ.get("ProgramFiles(x86)", r"C:\Program Files (x86)"),
                           "Microsoft Visual Studio", "Installer", "vswhere.exe")
    if not os.path.exists(vswhere):
        raise PortError("no vswhere.exe: the Visual C++ redistributable comes from Visual Studio (Build Tools)")
    vs = subprocess.run([vswhere, "-latest", "-products", "*", "-property", "installationPath"],
                        capture_output=True, text=True).stdout.strip()
    redist = os.path.join(vs, "VC", "Redist", "MSVC")
    found = []
    if os.path.isdir(redist):
        for version in os.listdir(redist):
            x64 = os.path.join(redist, version, "x64")
            if os.path.isdir(x64):
                found += [os.path.join(x64, d) for d in os.listdir(x64) if d.startswith("Microsoft.VC14")
                          and d.endswith(".CRT")]
    if not found:
        raise PortError(f"there is no Visual C++ redistributable under {vs}")
    return sorted(found)[-1]


def third_party(port, runtime_build):
    """LICENSES/THIRD_PARTY.md: the runtime's parts, then the port's own"""
    launcher, player = f"{port.name}{EXE}", f"game\\{port.player}{EXE}" if os.name == "nt" else f"game/{port.player}"
    text = f"""# Third-party software

- **Bergamota** (the player, `{player}`, the launcher, `{launcher}`, and
  the recompiler, `3dsrecomp`), MIT, (c) ShiromiyaG. `Bergamota.txt`. Parts
  of it are translated from **Zakuro**, MIT, (c) fearkov (`Zakuro.txt`), and
  its recompiler from **3dsrecomp**, MIT, (c) fearkov (`3dsrecomp.txt`).
- Built into the player and the launcher: **SDL3** (zlib), **Dear ImGui**
  (MIT), **toml++** (MIT); into the player also **shaderc** from the Vulkan
  SDK (Apache-2.0), and the **Fraunhofer FDK AAC Codec Library for Android**
  (https://github.com/mstorsjo/fdk-aac, v2.0.3), which decodes the audio
  firmware's AAC. `fdk-aac.txt`.
- **Nunito** (the launcher's font, built into `{launcher}`, and the menu's
  of a port that takes it, built into the player), SIL Open Font License
  1.1. `Nunito-OFL.txt`.
"""
    if os.name == "nt":
        text += """- **Microsoft Visual C++ runtime** (`msvcp140*.dll`, `vcruntime140*.dll`,
  beside the launcher, in `3dsrecomp\\` and in `game\\`), redistributable
  under the Visual Studio license terms.
- **Zig**, downloaded only when there is no C compiler, is MIT, (c) Zig
  Software Foundation; it is fetched from ziglang.org and its mirrors, not
  shipped here.
"""
    if os.path.exists(port.third_party):
        text += f"\n## {port.name}'s own\n\n" + open(port.third_party, encoding="utf-8").read().strip() + "\n"
    return text


def package(port, args, say, build_native):
    """build_native(port, args, game): port.py's native, the game's code's folder"""
    runtime_build = os.path.abspath(args.runtime_build or os.environ.get("RUNTIME_BUILD")
                                    or os.path.join(RUNTIME, "build"))
    env = dict(os.environ)
    if native.LLVM and os.path.isdir(native.LLVM):
        env["PATH"] = native.LLVM + os.pathsep + env.get("PATH", "")

    if not args.no_runtime_build:
        # the port's own UI built into the player, none otherwise (a build
        # folder shared with another port's may have had its)
        ui = port.ui.replace("\\", "/") if port.ui else ""
        icon = port.icon.replace("\\", "/") if port.icon else ""
        command = ["cmake", "-S", RUNTIME, "-B", runtime_build, "-G", "Ninja", "-DCMAKE_BUILD_TYPE=Release",
                   "-DCMAKE_C_COMPILER=clang", "-DCMAKE_CXX_COMPILER=clang++", f"-DRUNTIME_PORT_UI={ui}",
                   f"-DRUNTIME_PORT_ICON={icon}"]
        say("> " + " ".join(command))
        if subprocess.run(command, env=env).returncode:
            raise PortError("configuring the runtime failed")
        command = ["cmake", "--build", runtime_build, "--target", "player", "launcher", "3dsrecomp"]
        say("> " + " ".join(command))
        if subprocess.run(command, env=env).returncode:
            raise PortError("building the runtime failed")

    def built(name):
        path = os.path.join(runtime_build, name)
        if not os.path.exists(path):
            raise PortError(f"no {path}: the runtime is not built there")
        return path

    dist = os.path.join(port.root, "dist")
    out = os.path.join(dist, port.name)
    shutil.rmtree(out, ignore_errors=True)
    os.makedirs(os.path.join(out, "LICENSES"))
    copy(built("launcher" + EXE), os.path.join(out, port.name + EXE))
    copy(port.file, os.path.join(out, "port.toml"))
    copy(built("3dsrecomp" + EXE), os.path.join(out, "3dsrecomp", "3dsrecomp" + EXE))
    # the game itself, under a name players know it by
    copy(built("player" + EXE), os.path.join(out, "game", port.player + EXE))
    copy(built("shaders"), os.path.join(out, "game", "shaders"))
    # the single screen's scenes, which the player reads beside itself
    layouts = os.path.join(port.root, "single-screen.toml")
    if os.path.exists(layouts):
        copy(layouts, os.path.join(out, "game", "single-screen.toml"))
    if os.name == "nt":
        crt = vc_redist()
        for dll in VC_DLLS:
            for folder in (out, os.path.join(out, "game"), os.path.join(out, "3dsrecomp")):
                copy(os.path.join(crt, dll), os.path.join(folder, dll))
    if os.path.exists(port.readme):
        copy(port.readme, os.path.join(out, "README.md"))
    else:
        say(f"! no {os.path.relpath(port.readme, port.root)}: the package goes without a README")
    # the lists of each game's hottest functions: addresses and sample
    # counts, no game code
    if os.path.isdir(os.path.join(port.root, "hot")):
        copy(os.path.join(port.root, "hot"), os.path.join(out, "hot"))
    # what the port adds ([package] files)
    for source, target in port.files:
        if not os.path.exists(source):
            raise PortError(f"port.toml's [package] files: no {source}")
        copy(source, os.path.join(out, *target.replace("\\", "/").split("/")))

    # each game's native code, built here: the port's own, no game code
    if port.native_sources():
        import argparse
        for game in port.games:
            if not game.decomp:
                say(f"! {game.label()}: no decomp folder in port.toml, so no native code for it")
                continue
            build = argparse.Namespace(command="package", picked=game, rom=None, update=None, unpacked=None,
                                       extra=[], includes=[], out=None, no_symbols=False,
                                       runtime_build=runtime_build)
            try:
                folder = build_native(port, build, game)
            except PortError as e:
                if game.native_from:
                    # carrying the addresses needs both games' updates here
                    say(f"! {game.label()} goes without the port's code: {e}")
                    continue
                raise
            for name in ("decomp.bin", "decomp.txt"):
                copy(os.path.join(folder, name), os.path.join(out, game.decomp, name))

    copy(os.path.join(RUNTIME, "LICENSE"), os.path.join(out, "LICENSES", "Bergamota.txt"))
    copy(os.path.join(RUNTIME, "LICENSE-zakuro"), os.path.join(out, "LICENSES", "Zakuro.txt"))
    copy(os.path.join(RUNTIME, "LICENSE-3dsrecomp"), os.path.join(out, "LICENSES", "3dsrecomp.txt"))
    copy(os.path.join(RUNTIME, "launcher", "assets", "Nunito-OFL.txt"), os.path.join(out, "LICENSES", "Nunito-OFL.txt"))
    fdk = os.path.join(runtime_build, "_deps", "fdk_aac-src", "NOTICE")
    if os.path.exists(fdk):
        copy(fdk, os.path.join(out, "LICENSES", "fdk-aac.txt"))
    with open(os.path.join(out, "LICENSES", "THIRD_PARTY.md"), "w", encoding="utf-8") as f:
        f.write(third_party(port, runtime_build))

    system = {"win32": "windows", "darwin": "macos"}.get(sys.platform, sys.platform)
    machine = {"amd64": "x64", "x86_64": "x64", "arm64": "arm64", "aarch64": "arm64"}.get(
        platform.machine().lower(), platform.machine().lower())
    zip_base = os.path.join(dist, f"{port.name}-{args.version}-{system}-{machine}")
    if os.path.exists(zip_base + ".zip"):
        os.remove(zip_base + ".zip")
    shutil.make_archive(zip_base, "zip", root_dir=dist, base_dir=port.name)
    say(f"{zip_base}.zip: {os.path.getsize(zip_base + '.zip') / 2**20:.1f} MB")
