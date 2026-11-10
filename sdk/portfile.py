"""A port's port.toml, as the SDK's tools read it (docs/porting.md has every
key). The launcher reads the same file for what it needs (launcher/port.h):
the keys only the tools use are left alone by it.

TOML through tomllib (Python 3.11+), else a reader of the part of TOML these
files use: tables, arrays of tables, strings, numbers, booleans, arrays and
inline tables.
"""

import os
import re

SDK_VERSION = 2


class PortError(Exception):
    pass


# --- TOML ---------------------------------------------------------------

def load_toml(path):
    text = open(path, encoding="utf-8").read()
    try:
        import tomllib
    except ImportError:
        tomllib = None
    try:
        return tomllib.loads(text) if tomllib else _Reader(text).document()
    except Exception as e:  # tomllib.TOMLDecodeError, or the reader's ValueError
        raise PortError(f"{path}: {e}")


class _Reader:
    """enough TOML for port.toml, single-screen.toml and the launcher's config"""

    def __init__(self, text):
        self.text, self.at, self.line = text, 0, 1

    def fail(self, why):
        raise ValueError(f"line {self.text.count(chr(10), 0, self.at) + 1}: {why}")

    def skip(self, newlines=False):
        while self.at < len(self.text):
            c = self.text[self.at]
            if c in " \t\r" or (newlines and c == "\n"):
                self.at += 1
            elif c == "#":
                while self.at < len(self.text) and self.text[self.at] != "\n":
                    self.at += 1
            else:
                break

    def peek(self):
        return self.text[self.at] if self.at < len(self.text) else ""

    def expect(self, c):
        if self.peek() != c:
            self.fail(f"{c!r} expected")
        self.at += 1

    def document(self):
        root, table = {}, None
        table = root
        while True:
            self.skip(True)
            if self.at >= len(self.text):
                return root
            if self.text.startswith("[[", self.at):
                self.at += 2
                keys = self.keys("]")
                self.expect("]")
                self.expect("]")
                parent = self.descend(root, keys[:-1])
                items = parent.setdefault(keys[-1], [])
                if not isinstance(items, list):
                    self.fail(f"{keys[-1]} is not an array of tables")
                table = {}
                items.append(table)
            elif self.peek() == "[":
                self.at += 1
                keys = self.keys("]")
                self.expect("]")
                table = self.descend(root, keys)
            else:
                keys = self.keys("=")
                self.expect("=")
                self.skip()
                target = self.descend(table, keys[:-1])
                if keys[-1] in target:
                    self.fail(f"{keys[-1]} given twice")
                target[keys[-1]] = self.value()
            self.skip()
            if self.peek() not in ("\n", ""):
                self.fail("end of line expected")

    def descend(self, table, keys):
        for key in keys:
            table = table.setdefault(key, {})
            if isinstance(table, list):
                table = table[-1]
            if not isinstance(table, dict):
                self.fail(f"{key} is not a table")
        return table

    def keys(self, end):
        keys = []
        while True:
            self.skip()
            c = self.peek()
            if c in "\"'":
                keys.append(self.string())
            else:
                m = re.compile(r"[A-Za-z0-9_-]+").match(self.text, self.at)
                if not m:
                    self.fail("a key expected")
                keys.append(m.group())
                self.at = m.end()
            self.skip()
            if self.peek() != ".":
                return keys
            self.at += 1

    def string(self):
        quote = self.peek()
        if self.text.startswith(quote * 3, self.at):
            self.fail("multi-line strings are not read here (Python 3.11+ reads them)")
        self.at += 1
        out = []
        while True:
            c = self.peek()
            if c in ("", "\n"):
                self.fail("unterminated string")
            self.at += 1
            if c == quote:
                return "".join(out)
            if c == "\\" and quote == '"':
                e = self.peek()
                self.at += 1
                simple = {"n": "\n", "t": "\t", "r": "\r", '"': '"', "\\": "\\", "b": "\b", "f": "\f"}
                if e in simple:
                    out.append(simple[e])
                elif e in "uU":
                    n = 4 if e == "u" else 8
                    out.append(chr(int(self.text[self.at:self.at + n], 16)))
                    self.at += n
                else:
                    self.fail(f"unknown escape \\{e}")
            else:
                out.append(c)

    def value(self):
        c = self.peek()
        if c in "\"'":
            return self.string()
        if c == "[":
            self.at += 1
            items = []
            while True:
                self.skip(True)
                if self.peek() == "]":
                    self.at += 1
                    return items
                items.append(self.value())
                self.skip(True)
                if self.peek() == ",":
                    self.at += 1
                elif self.peek() != "]":
                    self.fail("',' or ']' expected")
        if c == "{":
            self.at += 1
            table = {}
            self.skip()
            if self.peek() == "}":
                self.at += 1
                return table
            while True:
                keys = self.keys("=")
                self.expect("=")
                self.skip()
                self.descend(table, keys[:-1])[keys[-1]] = self.value()
                self.skip()
                if self.peek() == "}":
                    self.at += 1
                    return table
                self.expect(",")
        m = re.compile(r"true|false|[+-]?(0x[0-9A-Fa-f_]+|0o[0-7_]+|0b[01_]+|[0-9_]+(\.[0-9_]+)?([eE][+-]?[0-9_]+)?)")\
            .match(self.text, self.at)
        if not m:
            self.fail("a value expected")
        self.at = m.end()
        word = m.group().replace("_", "")
        if word in ("true", "false"):
            return word == "true"
        if m.group(2) or m.group(3):
            return float(word)
        return int(word, 0)


# --- port.toml ----------------------------------------------------------

class Game:
    def __init__(self, port, t):
        self.title = str(t.get("title", "")).upper()
        if not re.fullmatch(r"[0-9A-F]{16}", self.title):
            raise PortError(f"port.toml: a [[game]] without a title id of 16 hex digits ({t.get('title')!r})")
        self.name = t.get("name", self.title)
        self.key = t.get("key", "")
        self.update = t.get("update")
        self.update_name = t.get("update_name", "")
        # where the package puts the port's code for this game, beside the
        # launcher (decomp.bin, decomp.txt); the launcher hands it on
        self.decomp = t.get("decomp", "")
        self.color = t.get("color", "")
        # the game native/'s addresses are from: carried to this one by
        # signature (sdk/addresses.py)
        self.native_from = str(t.get("native_from", "")).upper()

    def label(self):
        return f"{self.name} ({self.title})"


class Port:
    def __init__(self, root, t):
        self.root = os.path.abspath(root)
        self.file = os.path.join(self.root, "port.toml")
        self.name = t.get("name", "")
        if not self.name or not re.fullmatch(r"[A-Za-z0-9 _.-]+", self.name):
            raise PortError("port.toml: name is needed, letters, digits, spaces, '.', '_' or '-' "
                            "(the launcher's folder in %APPDATA% and the package are named after it)")
        self.player = t.get("player", "")
        if not self.player:
            raise PortError("port.toml: player is needed: the player's name in the package (game/<player>.exe)")
        self.about = t.get("about", "")
        self.sdk = t.get("sdk", SDK_VERSION)
        if not isinstance(self.sdk, int) or self.sdk > SDK_VERSION:
            raise PortError(f"port.toml: sdk = {self.sdk}, and this Bergamota's SDK is version {SDK_VERSION}: "
                            "update the runtime (external/runtime)")
        self.games = [Game(self, g) for g in t.get("game", [])]
        if not self.games:
            raise PortError("port.toml: no [[game]]")
        for g in self.games:
            if g.native_from and not self.game(g.native_from):
                raise PortError(f"port.toml: {g.label()} takes its native code from {g.native_from}, "
                                "which is not one of the port's games")
        native = t.get("native", {})
        self.sources = [self.path(p) for p in native.get("sources", ["native"])]
        self.includes = [self.path(p) for p in native.get("include", [])]
        self.symbols = [self.path(p) for p in native.get("symbols", [])]
        self.base = int(native.get("base", 0x07800000))
        package = t.get("package", {})
        self.ui = self.path(package["ui"]) if package.get("ui") else ""
        self.files = []
        for item in package.get("files", []):
            if isinstance(item, dict):
                self.files.append((self.path(item["from"]), item["to"]))
            else:
                self.files.append((self.path(item[0]), item[1]))
        self.readme = self.path(package.get("readme", "packaging/README.md"))
        self.third_party = self.path(package.get("third_party", "packaging/THIRD_PARTY.md"))

    def path(self, p):
        return os.path.normpath(os.path.join(self.root, p))

    def game(self, which):
        """a game by title id (or its end), key, or a word of its name"""
        if not which:
            return self.games[0]
        w = which.lower()
        for test in (lambda g: g.title.lower() == w, lambda g: g.key.lower() == w,
                     lambda g: g.title.lower().endswith(w), lambda g: w in g.name.lower()):
            found = [g for g in self.games if test(g)]
            if len(found) == 1:
                return found[0]
        return None

    def native_sources(self):
        files = []
        for folder in self.sources:
            if os.path.isdir(folder):
                for f, _, names in os.walk(folder):
                    files += [os.path.join(f, n) for n in names if n.endswith((".c", ".cpp"))]
        return sorted(files)


def find_root(start):
    """the folder with port.toml, start or above it"""
    folder = os.path.abspath(start)
    while True:
        if os.path.exists(os.path.join(folder, "port.toml")):
            return folder
        parent = os.path.dirname(folder)
        if parent == folder:
            return None
        folder = parent


def read(root):
    if not os.path.exists(os.path.join(root, "port.toml")):
        raise PortError(f"no port.toml in {root}")
    return Port(root, load_toml(os.path.join(root, "port.toml")))
