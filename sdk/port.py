"""Bergamota's port tool: everything a port does with its port.toml, from the
first look at a new game to the package players download (docs/porting.md).

    python -I <runtime>/sdk/port.py <command> [--port <folder>] [--game <game>] ...

The port is the folder with port.toml, the current one or above it, or
--port. A game is picked by its title id (or its end), its key, or a word
of its name; the port's first by default.

  new <folder> --name <Name> --game <title id>[:<name>]...
                 a new port from the template (sdk/template)
  check          what port.toml says, and what is set up here for each game
  symbols        `3dsrecomp symbols` for the game, into build/symbols-<title>:
                 the names the analysis found, which the native code can
                 call and which tell the builder where functions end
  native         the port's code for the game's ARM11 (native/) built into
                 build/native-<title> (decomp.bin, decomp.txt)
  build          native, then the game recompiled with it into
                 build/lib-<title>
  play           the game on the player, with what build made, saves in
                 build/user (never the launcher's)
  boot           the game without a window (the runtime's boot): how far it
                 gets and what it asks of the system that nothing answers yet
  package        the package players download, into dist/

The game's files come from the port's launcher: set the game up once with
<runtime>/build/launcher --port <port>/port.toml (it keeps the dump and the
update, and unpacks the update into %APPDATA%\\<Name>\\<title id>\\update).
An update unpacked by hand into the port's extracted/ (any folder name; its
exheader says whose it is) is found too. --rom, --update (the update's .cia)
and --unpacked (the update unpacked) give them for the game picked.
"""

import argparse
import os
import shutil
import subprocess
import sys

SDK = os.path.dirname(os.path.abspath(__file__))
RUNTIME = os.path.dirname(SDK)
sys.path.insert(0, SDK)
import native  # noqa: E402
import portfile  # noqa: E402
from portfile import PortError  # noqa: E402

EXE = ".exe" if os.name == "nt" else ""
LLVM = native.LLVM


def say(text):
    print(text, flush=True)


# --- where things are ---------------------------------------------------

def runtime_build(args):
    return os.path.abspath(getattr(args, "runtime_build", None) or os.environ.get("RUNTIME_BUILD")
                           or os.path.join(RUNTIME, "build"))


def program(args, name):
    path = os.path.join(runtime_build(args), name + EXE)
    if name == "3dsrecomp" and os.environ.get("RECOMP"):
        path = os.environ["RECOMP"]
    if not os.path.exists(path):
        raise PortError(f"no {path}: build the runtime first (cmake --build {os.path.relpath(runtime_build(args))})")
    return path


def data_dir(port):
    """the launcher's folder for the port (launcher/library.cpp, data_dir)"""
    if os.environ.get("APPDATA"):
        return os.path.join(os.environ["APPDATA"], port.name)
    if sys.platform == "darwin":
        return os.path.expanduser(f"~/Library/Application Support/{port.name}")
    return os.path.join(os.environ.get("XDG_DATA_HOME") or os.path.expanduser("~/.local/share"), port.name)


class Files:
    """a game's files: the dump, the update's .cia, the update unpacked"""

    def __init__(self, port, game, args=None):
        self.rom = self.update = self.unpacked = ""
        config = os.path.join(data_dir(port), "config.toml")
        if os.path.exists(config):
            t = portfile.load_toml(config).get("games", {}).get(game.title, {})
            self.rom, self.update = t.get("game", ""), t.get("update", "")
        unpacked = os.path.join(data_dir(port), game.title, "update")
        if os.path.exists(os.path.join(unpacked, "exheader.bin")):
            self.unpacked = unpacked
        else:
            self.unpacked = extracted(port, game)
        # the command line's, for the game picked
        if args is not None and getattr(args, "picked", None) is game:
            self.rom = args.rom or self.rom
            self.update = args.update or self.update
            self.unpacked = args.unpacked or self.unpacked
        self.game, self.port = game, port

    def need(self, *what):
        missing = [w for w in what if not getattr(self, w) or not os.path.exists(getattr(self, w))]
        if missing:
            names = {"rom": "the game's dump (--rom)", "update": "the update's .cia (--update)",
                     "unpacked": "the update unpacked (--unpacked)"}
            raise PortError(f"{self.game.label()}: no {', '.join(names[m] for m in missing)}. Set the game up "
                            f"once in the port's launcher: {os.path.relpath(os.path.join(RUNTIME, 'build', 'launcher' + EXE))} "
                            f"--port {os.path.relpath(self.port.file)}")
        return self


def extracted(port, game):
    """an update unpacked by hand into the port's extracted/ (as ctrtool
    lays it), found by the program id its exheader holds (at 0x200)"""
    folder = os.path.join(port.root, "extracted")
    if not os.path.isdir(folder):
        return ""
    for name in sorted(os.listdir(folder)):
        exheader = os.path.join(folder, name, "exheader.bin")
        try:
            with open(exheader, "rb") as f:
                f.seek(0x200)
                program_id = int.from_bytes(f.read(8), "little")
        except OSError:
            continue
        if f"{program_id:016X}" == game.title:
            return os.path.join(folder, name)
    return ""


def build_dir(port, *parts):
    return os.path.join(port.root, "build", *parts)


def compiler_env():
    env = dict(os.environ)
    env.setdefault("CC", "clang")
    env.setdefault("AR", "llvm-ar")
    if LLVM and os.path.isdir(LLVM):
        env["PATH"] = LLVM + os.pathsep + env.get("PATH", "")
    return env


def short(path, base):
    """path from base when it can be (3dsrecomp links every object on one
    command line, which long paths take past Windows' length)"""
    try:
        return os.path.relpath(path, base)
    except ValueError:
        return path


def run(command, **kw):
    say("> " + " ".join(f'"{c}"' if " " in c else c for c in command))
    result = subprocess.run(command, **kw)
    if result.returncode:
        raise PortError(f"{os.path.basename(command[0])} failed ({result.returncode})")


# --- commands -----------------------------------------------------------

def cmd_check(port, args):
    say(f"{port.name} (sdk {port.sdk}; this runtime's SDK is {portfile.SDK_VERSION})")
    say(f"  launcher: {port.name}{EXE}, player: game/{port.player}{EXE}")
    sources = port.native_sources()
    say(f"  native code: {len(sources)} files in {', '.join(short(s, port.root) for s in port.sources)}")
    if port.symbols:
        say(f"  symbol files: {', '.join(short(s, port.root) for s in port.symbols)}")
    layouts = os.path.join(port.root, "single-screen.toml")
    if os.path.exists(layouts):
        scenes = portfile.load_toml(layouts).get("scene", [])
        bad = [s.get("name", "?") for s in scenes if s.get("layout") not in ("panel", "stacked", "top")]
        say(f"  single screen: {len(scenes)} scenes" + (f"; a layout not panel, stacked or top in: {', '.join(bad)}"
                                                       if bad else ""))
    problems = 0
    for game in port.games:
        files = Files(port, game, args)
        say(f"\n  {game.label()}" + (f", key {game.key}" if game.key else ""))
        say(f"    update tested: " + (f"{game.update_name or '?'} (version {game.update})" if game.update else "none"))
        if sources:
            if not game.decomp:
                say("    ! no decomp folder: the package leaves the native code out for this game")
                problems += 1
            else:
                say(f"    native code: {game.decomp}/ in the package"
                    + (f", its addresses carried from {port.game(game.native_from).name}" if game.native_from else ""))
        for what, path in (("dump", files.rom), ("update", files.update), ("unpacked", files.unpacked)):
            say(f"    {what}: " + (path if path and os.path.exists(path) else "-"))
        for what, path in (("symbols", build_dir(port, "symbols-" + game.title)),
                           ("native", build_dir(port, "native-" + game.title)),
                           ("library", build_dir(port, "lib-" + game.title))):
            if os.path.exists(path):
                say(f"    {what}: {short(path, port.root)}")
    if problems:
        say(f"\n{problems} to look at")


def cmd_symbols(port, args, game=None):
    game = game or args.picked
    files = Files(port, game, args).need("rom")
    out = build_dir(port, "symbols-" + game.title)
    command = [program(args, "3dsrecomp"), "symbols", files.rom, out]
    if files.unpacked:
        command += ["--mods", files.unpacked]
    run(command)
    return out


def cmd_native(port, args, game=None):
    """the port's code built for the game; its folder, None without code"""
    game = game or args.picked
    if not port.native_sources() and not getattr(args, "extra", None):
        say(f"{port.name} has no native code ({', '.join(short(s, port.root) for s in port.sources)})")
        return None
    origin = port.game(game.native_from) if game.native_from else game
    origin_files = Files(port, origin, args)
    starts = build_dir(port, "symbols-" + origin.title)
    if not os.path.exists(os.path.join(starts, "executable.txt")) and origin_files.rom \
            and os.path.exists(origin_files.rom) and not getattr(args, "no_symbols", False):
        say(f"{origin.name}'s symbols, once (port.py symbols):")
        cmd_symbols(port, args, origin)
    if not os.path.exists(starts):
        starts = None
    carry = {}
    if game.native_from:
        carry = {"game": Files(port, game, args).need("unpacked").unpacked,
                 "reference": origin_files.need("unpacked").unpacked}
    out = os.path.abspath(args.out if getattr(args, "out", None) and args.command == "native"
                          else build_dir(port, "native-" + game.title))
    # what 3dsrecomp found first, the port's own names over them
    symbol_files = ([os.path.join(starts, "executable.txt")] if starts else []) + port.symbols
    native.build(out, port.sources, extra=getattr(args, "extra", None) or (),
                 includes=port.includes + (getattr(args, "includes", None) or []), symbol_files=symbol_files,
                 starts=starts, program=origin_files.unpacked or None, base=port.base, root=port.root, **carry)
    return out


def cmd_build(port, args):
    game = args.picked
    decomp = cmd_native(port, args)
    if args.native_only:
        if decomp:
            say(f"native code in {short(decomp, port.root)}; the library left as it is: what changed runs in "
                "the interpreter until the next build")
        return
    files = Files(port, game, args).need("rom")
    out = os.path.abspath(args.out or build_dir(port, "lib-" + game.title))
    command = [program(args, "3dsrecomp"), "build", short(files.rom, port.root), short(out, port.root)]
    if files.unpacked:
        command += ["--mods", short(files.unpacked, port.root)]
    if decomp:
        command += ["--decomp", short(decomp, port.root)]
    hot = args.hot or (os.path.join(port.root, "hot", game.title + ".txt") if args.fast else "")
    if hot:
        if not os.path.exists(hot):
            raise PortError(f"no hot list {hot}")
        command += ["--hot", short(hot, port.root)]
        if args.fast:
            command.append("--cold-fast")
    run(command, env=compiler_env(), cwd=port.root)
    say(f"{game.name}: {short(out, port.root)}" + (f", with {short(decomp, port.root)}" if decomp else ""))


def library_in(folder):
    if os.path.isdir(folder):
        for name in sorted(os.listdir(folder)):
            if name.endswith((".dll", ".so", ".dylib")):
                return os.path.join(folder, name)
    return ""


def cmd_play(port, args):
    game = args.picked
    files = Files(port, game, args).need("rom")
    library = library_in(args.library or build_dir(port, "lib-" + game.title))
    if not library:
        raise PortError(f"{game.label()}: no library in build/lib-{game.title}: port.py build first")
    decomp = build_dir(port, "native-" + game.title)
    user = os.path.abspath(args.user or build_dir(port, "user"))
    os.makedirs(user, exist_ok=True)
    # the launcher's player settings (keys, video) when there are any, the
    # game's own files over them
    settings = os.path.join(data_dir(port), "player.toml")
    t = portfile.load_toml(settings) if os.path.exists(settings) else {}
    g = t.setdefault("game", {})
    for key in ("game", "library", "user", "update", "decomp", "exheader", "code", "mods", "layouts"):
        g.pop(key, None)
    g.update({"game": files.rom, "library": library, "user": user})
    if files.update:
        g["update"] = files.update
    if os.path.exists(os.path.join(decomp, "decomp.txt")) and not args.no_native:
        g["decomp"] = decomp
    if os.path.exists(os.path.join(port.root, "single-screen.toml")):
        g["layouts"] = os.path.join(port.root, "single-screen.toml")
    config = build_dir(port, f"player-{game.title}.toml")
    with open(config, "w", encoding="utf-8") as f:
        f.write(f"# written by port.py play for {game.name}; saves in {user}\n")
        f.write(dump_toml(t))
    command = [program(args, "player"), "--config", config]
    if args.config_only:
        say(f"{config}; to play: " + " ".join(f'"{c}"' if " " in c else c for c in command))
        return
    run(command)


def cmd_boot(port, args):
    game = args.picked
    files = Files(port, game, args).need("rom")
    if not files.unpacked:
        raise PortError(f"{game.label()}: boot needs the update unpacked (exheader.bin, code.bin)")
    library = library_in(args.library or build_dir(port, "lib-" + game.title))
    if not library:
        raise PortError(f"{game.label()}: no library in build/lib-{game.title}: port.py build first")
    user = os.path.abspath(args.user or build_dir(port, "user"))
    os.makedirs(user, exist_ok=True)
    code = os.path.join(files.unpacked, "code.bin")
    if not os.path.exists(code):
        code = os.path.join(files.unpacked, "exefs", "code.bin")
    env = dict(os.environ)
    decomp = build_dir(port, "native-" + game.title)
    if os.path.exists(os.path.join(decomp, "decomp.txt")) and not args.no_native:
        env.setdefault("RUNTIME_DECOMP", decomp)
    if os.path.exists(os.path.join(port.root, "single-screen.toml")):
        env.setdefault("RUNTIME_LAYOUTS", os.path.join(port.root, "single-screen.toml"))
    if args.frames:
        env["RUNTIME_FRAMES"] = str(args.frames)
    run([program(args, "boot"), os.path.join(files.unpacked, "exheader.bin"), code, library, str(args.seconds),
         files.rom, files.update or "", user], env=env)


def cmd_package(port, args):
    from package import package
    package(port, args, say, cmd_native)


def cmd_new(args):
    from template import new_port
    new_port(args, say)


# --- TOML out, for the player's settings ----------------------------------

def _value(v):
    if isinstance(v, bool):
        return "true" if v else "false"
    if isinstance(v, (int, float)):
        return repr(v)
    if isinstance(v, list):
        return "[" + ", ".join(_value(x) for x in v) + "]"
    if isinstance(v, dict):
        return "{ " + ", ".join(f"{_key(k)} = {_value(x)}" for k, x in v.items()) + " }"
    s = str(v).replace("\\", "\\\\").replace('"', '\\"').replace("\n", "\\n")
    return f'"{s}"'


def _key(k):
    return k if k and all(c.isalnum() or c in "_-" for c in k) else _value(k)


def dump_toml(t, prefix=""):
    lines = [f"{_key(k)} = {_value(v)}" for k, v in t.items() if not isinstance(v, dict)]
    out = "\n".join(lines) + ("\n" if lines else "")
    for k, v in t.items():
        if isinstance(v, dict):
            name = f"{prefix}.{_key(k)}" if prefix else _key(k)
            out += f"\n[{name}]\n" + dump_toml(v, name)
    return out


# --- the command line ---------------------------------------------------

def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    commands = parser.add_subparsers(dest="command", required=True)

    def command(name, help, game=True):
        p = commands.add_parser(name, help=help)
        p.add_argument("--port", help="the port's folder (with port.toml); default: here or above")
        p.add_argument("--runtime-build", help="where the runtime is built (default <runtime>/build)")
        if game:
            p.add_argument("--game", help="title id (or its end), key or a word of the name; default: the first")
            p.add_argument("--rom", help="the game's dump, instead of the launcher's")
            p.add_argument("--update", help="the update's .cia, instead of the launcher's")
            p.add_argument("--unpacked", help="the update unpacked (exheader.bin, code.bin), instead of the launcher's")
        return p

    p = commands.add_parser("new", help="a new port from the template")
    p.add_argument("folder")
    p.add_argument("--name", required=True, help="the port's name (its launcher, its folder in %%APPDATA%%)")
    p.add_argument("--game", action="append", default=[], help="<title id>[:<name>], once per game")
    p.add_argument("--about", default="", help="a line on what the port is")
    command("check", "what port.toml says, and what is set up here")
    command("symbols", "3dsrecomp symbols for the game, into build/symbols-<title>")
    for name, help in (("native", "the port's ARM11 code, into build/native-<title>"),
                       ("build", "native, then the game recompiled with it, into build/lib-<title>")):
        p = command(name, help)
        p.add_argument("--extra", action="append", default=[],
                       help="more sources (a decomp's), whose functions lose to the port's")
        p.add_argument("-I", dest="includes", action="append", default=[], help="an include folder for them")
        p.add_argument("--out", help="another folder to build into")
        p.add_argument("--no-symbols", action="store_true", help="no 3dsrecomp symbols run when they are missing")
        if name == "build":
            p.add_argument("--native-only", action="store_true",
                           help="only the ARM11 code; the library as it is, what changed interpreted")
            p.add_argument("--hot", help="a hot list: only the hot functions compiled (3dsrecomp --hot)")
            p.add_argument("--fast", action="store_true",
                           help="the port's hot list (hot/<title>.txt), the rest unoptimized (--cold-fast)")
    for name, help in (("play", "the game on the player, with build's library and native code"),
                       ("boot", "the game without a window: how far it gets, what nothing answers")):
        p = command(name, help)
        p.add_argument("--library", help="the folder of another library than build/lib-<title>")
        p.add_argument("--user", help="the saves' folder (default build/user; never the launcher's)")
        p.add_argument("--no-native", action="store_true", help="without the port's native code")
        if name == "play":
            p.add_argument("--config-only", action="store_true",
                           help="only the player's settings written (build/player-<title>.toml), not played")
        if name == "boot":
            p.add_argument("--seconds", type=int, default=60, help="how long, on the host's clock (60)")
            p.add_argument("--frames", type=int, help="how many frames, on the virtual clock")
    p = command("package", "the package players download, into dist/", game=False)
    p.add_argument("--version", default="0.1.0")
    p.add_argument("--no-runtime-build", action="store_true", help="the runtime as built already")

    args = parser.parse_args()
    if not sys.stdout.isatty() and hasattr(sys.stdout, "reconfigure"):
        sys.stdout.reconfigure(encoding="utf-8", errors="replace")
    try:
        if args.command == "new":
            return cmd_new(args)
        root = portfile.find_root(args.port or os.getcwd())
        if not root:
            raise PortError("no port.toml here or above: --port <folder>")
        port = portfile.read(root)
        if hasattr(args, "game"):
            args.picked = port.game(args.game)
            if not args.picked:
                raise PortError(f"no game {args.game!r} in {port.name}: "
                                + ", ".join(g.key or g.title for g in port.games))
        {"check": cmd_check, "symbols": cmd_symbols, "native": cmd_native, "build": cmd_build,
         "play": cmd_play, "boot": cmd_boot, "package": cmd_package}[args.command](port, args)
    except (PortError, native.BuildError) as e:
        sys.exit(f"port.py: {e}")


if __name__ == "__main__":
    main()
