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
| `RUNTIME_DECOMP=folder` | the decomp (`decomp.bin`, `decomp.txt`, from decomp-tools' `build.py`) in place of the functions it replaces; the library has to be built with it, see below |

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

## `texdump` and HD textures

`texdump <game .3ds> [update .cia] <out folder>` writes a game's textures as
PNG without running it: its RomFS (the update's files over the game's),
through GARC, LZ11, Game Freak's two-letter packs and darc, to the textures in
BCH, CLIM/FLIM, CTPK and Sun and Moon's GFTexture. Each is named
`tex_<w>x<h>_<hash>_<format>.png`: the hash is `hash_bytes` (`src/vk/impl.h`)
over the texture's first level as memory has it, the image upright. Two
games of a pair go into one folder (the same texture, the same name). What
only the running game makes (the font, what it draws into) is not in its
files: the player's `video.dump_textures` writes every texture the game uses
to `texture-dump/` beside the settings, named the same way.

The player draws the PNGs in `textures/` beside the settings (any folders in
it, any size, the 16 hex digits of the hash in the name) in place of the
textures they name (`video.hd_textures`, on by default), read the first time
the game uses each (`VulkanRenderer::textures_from`, `src/vk/textures.cpp`),
or all of them from the start on threads of their own with
`video.preload_textures` (no stutter, but every one kept in memory).

`tools/make_upscale_chain.py <textures> <out> <chain.chn> [model]` writes a chaiNNer
(0.26) chain that upscales a folder of them with a PyTorch model and saves
each under its own name and folder, ready for `textures/`: as DDS in BC7 (a
byte a pixel on disk and on the GPU, kept compressed; turned upside down
first, since the runtime takes a DDS's rows bottom first, as the PICA keeps
them), or with `--png` as PNG. The player reads DDSs of BC1, BC3 and BC7.

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
