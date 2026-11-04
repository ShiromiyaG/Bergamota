# Tools for measuring and checking

## `boot`

Runs a game without a window and reports how far it got: the modules it
loaded, where the time went (the game's code, the interpreter, syscalls by
number, services by command, the renderer), the service commands nothing
answers yet, and where each thread stands at the end.

```sh
build/boot <exheader.bin> <code.bin> <recompiled library> <seconds> <game .3ds> <update .cia> <saves folder>
```

On the host's clock it runs for `seconds`, pressing A for a tenth of each
second to get past the title. With `RUNTIME_FRAMES` it runs on a virtual
clock instead (a tick an instruction, as Zakuro counts them): the same input
gives the same run, and it never sleeps, so it runs as fast as the host can.

| Variable | What it does |
| --- | --- |
| `RUNTIME_FRAMES=n` | run n frames on the virtual clock |
| `RUNTIME_REPLAY=file` | the input of a Zakuro recording (`ZAKURO_RECORD`), frame by frame; its clock becomes the console's |
| `RUNTIME_SHOTS=folder` | both screens as `<frame>-top.ppm` and `<frame>-bottom.ppm` |
| `RUNTIME_SHOT_EVERY=n` | every n frames (100) |
| `RUNTIME_USAGE=file` | the service commands asked for, as Zakuro's `ZAKURO_USAGE` writes them |
| `RUNTIME_RENDERER=native` | the runtime's Vulkan renderer; `sketch` draws wireframes of the vertex stage on the CPU instead; unset, Zakuro's |
| `RUNTIME_DECOMP=folder` | the decomp (`decomp.bin`, `decomp.txt`, from decomp-tools' `build.py`) in place of the functions it replaces; a library built without it, or with an older one, runs what differs in the interpreter, see below |
| `RUNTIME_TRACE_UNWIND=1` | interface 6: where the code unwound to its thread's loop, and the host's runs that came back with the stack elsewhere (the first few) |
| `RUNTIME_MODS=folder` | the player's own changes, as the player's `mods` in `[game]`: `code.ips` laid over the program, the cheats of `cheats.txt` run each blank (the ones it marks on; `RUNTIME_CHEATS=all` runs every one), see below |

Always point it at a copy of the saves: the game writes to them.

## The decomp

The decomp's C runs in place of the game's functions it replaces, and
3dsrecomp's C does the rest. Both come from the same two files:

```sh
python -I decomp-tools/decomp/build.py              # -> <project>/build/decomp
3dsrecomp build <game .3ds> <dir> --mods <update unpacked> --decomp <project>/build/decomp
RUNTIME_DECOMP=<project>/build/decomp build/boot <exheader.bin> <code.bin> <dir>/<program id>.dll ...
```

`3dsrecomp --decomp` writes a jump over each replaced function of the
executable before recompiling it, and recompiles the decomp as a module of
its own, `decomp`. The runtime writes the same jumps when it loads the code
and maps the decomp at its base. A module's functions are hooked as the
module loads: the decomp's addresses in it are written to their slots, the
jumps over the functions it replaces, and then the module's code is checked
against what was recompiled (`recomp_origins`). The hooked functions no
longer match, so the code skips them, the interpreter runs the jump, and
the decomp's recompiled code takes over. The player reads the folder from
`decomp` in `[game]`.

The same check finds anything else that changed since the library was made
(a mod, another update): what changed runs in the interpreter, and `boot`
says how many functions did.

So the decomp does not need the game recompiled to run. With a library
built without it, or with an older one, the functions it hooks no longer
match and go to the interpreter, which takes their jumps, and the decomp's
code the library lacks (new, or changed since) runs in the interpreter too,
until the next build takes it in. Changing a port's `native/` code is
`build.py` and starting the game; only speed waits for the build (the
interpreter is slower than the C: fine for wrappers called a few times a
frame, not for a loop over every vertex). The port's launcher plays a game whose code
changed at once and builds it again in the background.

## The port's options

A port's code declares what the player may turn on or off
(`PORT_OPTION(name, default, label, help)` in Solstice's `native/port.h`,
`PORT_OPTION_RANGE` for a number): build.py writes each as an `option`
line of `decomp.txt`, and the player lists them in its menu's Port tab, and
the launcher on its Graphics page, with nothing in either named after the
option. The value is kept in the settings' `[port]` table by name and
written into the code's `port_<name>` word, live.

## The player's own changes

The `mods` folder of `[game]` (the launcher's `%APPDATA%\<port>\<title>\mods`)
takes two files, the ones Luma and Azahar take:

- `code.ips`: an IPS patch of `code.bin`, laid over it as it loads.
- `cheats.txt`: Gateway (Action Replay) codes, as Azahar keeps them
  (`[name]`, `*citra_enabled` for the ones on, two words a line), run once a
  frame between two of the game's slices; the menu's Cheats tab turns each on
  or off, kept in `[cheats]`. The code types are Azahar's: writes, conditions
  on a word or a masked halfword, the offset and the data register, a loop,
  the buttons, a run of bytes (`src/cheats.cpp`).

Neither needs the game recompiled: what they change in the code no longer
matches the library, and runs in the interpreter (a cheat that writes code
has the library check it again then).

## `tools/compare.py`

Runs the same stretch of a game on the runtime and on Zakuro, with the same
recorded input, and reports where they part: the screens every so many
frames (a mean difference above a threshold), the service commands each was
asked (one side only, or counts far apart), and the save files at the end.
Zakuro already runs the games, so a difference points at the runtime.

```sh
python -I tools/compare.py <exheader.bin> <code.bin> <library> <game .3ds> <update .cia> \
    <saves folder to copy> <recording> <out folder> [--frames 3000] [--every 300]
```

Needs Pillow. The two clocks count alike but not the same, so timed scenes
(the intro movie) can differ by a few frames without anything being wrong.

## Texture packs (HD textures) and `texdump`

The player takes texture packs made for Azahar (and Citra) as they are,
and writes its dumps the way Azahar does, so a pack made with either works
in both. Written from a description of how Azahar behaves (its folders,
names, options and hashes), none of its code; the hash is Google's
CityHash64 v1.1, `src/texture_pack.cpp`.

**Where.** A game's packs are `load/textures/<title id>/` beside the
player's settings (the title id as 16 upper-case hex digits, the game's
own, never the update's: `00040000001B5000` for Ultra Sun), then the same
folder in Azahar's own user folder (`%APPDATA%\Azahar`, `video.azahar_textures`,
on by default), then `textures/` beside the settings (the runtime's packs
from before). Every folder in them is searched; the first file found for a
texture keeps it (folders in that order, files in name order).

**A pack's files.** `tex1_<w>x<h>_<hash>_<format>...png` (or `.dds`, `.ktx`):
only the hash counts, the size, format and `_mipN` in the name are left
alone, and the file may be any size. A `pack.json` at the pack's top says
how its hashes were taken and how its files lie:

| `options` | | |
| --- | --- | --- |
| `use_new_hash` | true | the hash of the texture's first level as memory has it (8x8 tiles, the PICA's own encoding) |
| | false | the legacy hash: the same decoded, its rows from the last in memory to the first, formats 0-4 their texels' own bytes, 5-13 R, G, B, A |
| `flip_png_files` | true | a PNG's first row is the texture's first in memory, as dumps are |
| `skip_mipmap` | true | DDS files are passed over (Azahar makes the mips from the base, which a DDS cannot have) |

`textures` gives hashes to files by name (`"114BFC385ED72F15": "logo.png"`).
A folder without a `pack.json` is a legacy pack: the legacy hash, PNGs as
dumped, DDS passed over.

Rows: Azahar keeps a texture's rows from the last in memory to the first,
the runtime first to last (and samples with v turned over). So the same
files take opposite steps here and come out the same: Azahar turns a PNG
over as it reads it (`flip_png_files`) and the runtime takes it as it is;
Azahar takes a DDS or KTX as it is stored (its first row the texture's last
in memory), and so does the runtime, RGBA8 and blocks alike, nothing of it
moved, sampling it with v the other way (`TEX_FLIP`, `src/vk/draw.cpp`,
`pica.frag`). Read: PNG,
DDS and KTX 1 of RGBA8, BC1, BC3, BC5 and BC7 (their first level). Not:
normal maps (`.norm.`), ASTC, the mips of a pack (the runtime draws the
first level only). The runtime's own files from before, `tex_<...>_<hash>_<...>`
with its own hash and the other orientation, still read, in any folder.

**Reading.** As the game first uses each texture, or all of them from the
start on threads of their own with `video.preload_textures` (no stutter,
but every one kept in memory). `boot` takes `RUNTIME_TEXTURES=folder;folder`.

**Dumping.** `video.dump_textures` writes every texture the game uses to
`dump/textures/<title id>/` beside the settings as Azahar does: named
`tex1_<w>x<h>_<hash>_<format>_mip0.png` by the new hash, the PNG's first row
the first in memory, and a `pack.json` saying so, so the folder is a pack
as it is (`boot`: `RUNTIME_DUMP_TEXTURES=folder`). `texdump <game .3ds>
[update .cia] <out folder>` writes the same without playing: the game's
RomFS (the update's files over the game's), through GARC, LZ11, Game
Freak's two-letter packs and darc, to the textures in BCH, CLIM/FLIM, CTPK
and Sun and Moon's GFTexture. Two games of a pair go into one folder (the
same texture, the same name). What only the running game makes (the font,
what it draws into) is not in its files.

`tools/make_upscale_chain.py <textures> <out> <chain.chn> [model]` writes a
chaiNNer (0.26) chain that upscales a dump with a PyTorch model and saves
each under its own name and folder, with the pack's `pack.json`: as DDS in
BC7 (a byte a pixel on disk and on the GPU, kept compressed; turned upside
down first, as Azahar keeps a DDS), or with `--png` as PNG.

Checked on Ultra Sun's intro: its dump used as a pack draws the same
pictures, as PNG and as an RGBA8 DDS, and a BC3 DDS the right way up.

## The player

| Variable | What it does |
| --- | --- |
| `RUNTIME_STATS=1` | a line a second: frames shown, the longest gap, time in the GPU, idle, reading the screens, sending the frame |

## The renderer

| Variable | What it does |
| --- | --- |
| `RUNTIME_VK_VALIDATION=1` | the Vulkan validation layer (needs the SDK) |
| `RUNTIME_VK_DEBUG=1` | what falls off the fast paths: transfers and textures that go through memory, write-backs and how long they took |
| `RUNTIME_VK_NO_PICTURE=1` | read the screens from memory rather than from the GPU |
