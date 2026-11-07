"""port.py new: a new port's folder from sdk/template, its port.toml written
for its games."""

import os
import re
import shutil

from portfile import SDK_VERSION, PortError

SDK = os.path.dirname(os.path.abspath(__file__))
TEMPLATE = os.path.join(SDK, "template")
# a launcher's colors for a port's games, in turn
COLORS = ["#2F6FE4", "#E2384A", "#F08A24", "#7B5CE0", "#2BA35F"]


def names_of(games):
    names = [name for _, name in games]
    return names[0] if len(names) == 1 else ", ".join(names[:-1]) + " and " + names[-1]


def port_toml(name, about, games):
    lines = [f"# {name}'s port (Bergamota's docs/porting.md): its games for the launcher,",
             "# its native code and its package.",
             f'name = "{name}"',
             f'player = "{name}Game"           # the player as the package names it (game/<player>.exe)',
             f'about = "{about}"',
             f"sdk = {SDK_VERSION}                          # the SDK version the port is written for",
             "",
             "[native]",
             'sources = ["native"]               # the C for the game\'s ARM11',
             "symbols = []                       # files of names the code calls the game's functions by",
             ""]
    for i, (title, game) in enumerate(games):
        lines += ["[[game]]",
                  f'title = "{title}"',
                  f'name = "{game}"',
                  f'color = "{COLORS[i % len(COLORS)]}"',
                  '# key = "x"                        # a short name for port.py --game',
                  "# update = 1024                    # the update it is tested with (the TMD's version)",
                  '# update_name = "v1.1"             # and as players know it',
                  (f'decomp = "native-{i + 1}"' if len(games) > 1 else 'decomp = "native"') +
                  "  # native/ built for it: a folder beside the launcher in the package"]
        if i:
            # native/'s addresses are one game's: every other game's taken
            # from it by signature, never used as they are
            lines.append(f'native_from = "{games[0][0]}"  # native/\'s addresses are that game\'s: carried here')
        lines.append("")
    return "\n".join(lines)


def new_port(args, say):
    name = args.name
    if not re.fullmatch(r"[A-Za-z0-9][A-Za-z0-9 _.-]*", name):
        raise PortError("--name: letters, digits, spaces, '.', '_' or '-'")
    games = []
    for text in args.game:
        title, _, game = text.partition(":")
        title = title.strip().upper()
        if not re.fullmatch(r"[0-9A-F]{16}", title):
            raise PortError(f"--game {text}: a title id is 16 hex digits (0004000000055D00)")
        games.append((title, game.strip() or title))
    if not games:
        raise PortError("--game <title id>[:<name>], once for each of the port's games")
    folder = os.path.abspath(args.folder)
    if os.path.exists(folder) and os.listdir(folder):
        raise PortError(f"{folder} is there and not empty")
    about = args.about or f"{names_of(games)}, recompiled for this computer from your own copy."
    rows = "\n".join(f"| {game} | `{title}` |" for title, game in games)
    values = {"@NAME@": name, "@PLAYER@": name + "Game", "@GAMES@": names_of(games), "@GAME_ROWS@": rows}

    for base, _, files in os.walk(TEMPLATE):
        for f in files:
            source = os.path.join(base, f)
            target = os.path.join(folder, os.path.relpath(source, TEMPLATE))
            os.makedirs(os.path.dirname(target), exist_ok=True)
            text = open(source, encoding="utf-8").read()
            for key, value in values.items():
                text = text.replace(key, value)
            with open(target, "w", encoding="utf-8", newline="\n") as out:
                out.write(text)
    with open(os.path.join(folder, "port.toml"), "w", encoding="utf-8", newline="\n") as out:
        out.write(port_toml(name, about, games))
    say(f"{name} in {folder}. Next:")
    say(f"  cd {folder}")
    say("  git init && git submodule add https://github.com/ShiromiyaG/Bergamota.git external/runtime")
    say("  (or, in a workspace with Bergamota in tools/runtime, nothing: tools/port.py finds it)")
    say("  <runtime>/build/launcher --port port.toml    # set the game up once")
    say("  python -I tools/port.py check")
    say("  python -I tools/port.py build && python -I tools/port.py play")
