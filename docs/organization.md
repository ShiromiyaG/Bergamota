# Organizing the code (2026-10-31)

A plan, not done yet. The runtime grew fast (renderer, AsyncGpu, frames
between, the player, the launcher) and some pieces now sit in the wrong
place or in files too big to follow. Each step below is one commit that
changes no behavior, so `boot`'s shots stay the same before and after.

## 1. Split the renderer (`src/vk/renderer.cpp`, 1,543 lines)

Done (2026-11-01): `Impl` is declared in `src/vk/impl.h` and its methods
are in the files below; `renderer.cpp` keeps the public side. `boot`'s
shots were the same before and after, byte for byte, with `RUNTIME_ASYNC=0`
(with the GPU on its own thread a run differs a little from the next, as the
interrupts come back at other points of the game's code).

One `Impl` struct holds everything. By what it does:

| File | What goes there |
| --- | --- |
| `vk/device.cpp` | instance, device, queue; buffers and images; the two slots, `submit`, `retire` |
| `vk/surfaces.cpp` | the surface cache: `surface`, `sync`, `upload`, `write_back`, `fill`, `transfer`, the guards |
| `vk/textures.cpp` | the texture cache, samplers, `depth_as_color` |
| `vk/draw.cpp` | pipelines and `render` |
| `vk/between.cpp` | the frames between: ops, matching, replay |
| `vk/screens.cpp` | screen read-back (later: the swapchain) |

`Impl` becomes a header private to `src/vk/`. While at it:

- the shape counter goes (done);
- the `RUNTIME_VK_*` variables read once at start (done for
  `RUNTIME_VK_DEBUG`; `RUNTIME_VK_NO_PICTURE` is gone);
- the counters and timers (`draws`, `time_vertices`, `part[]`,
  `write_back_causes`...) in one `Stats` struct (done:
  `VulkanRenderer::stats`; still timed always, which costs nothing that
  shows).

## 2. One place for the frames between and for presenting

Done (2026-10-31): `Frame` and `Gpu::next_frame`, fed by
`VulkanRenderer::vblank` on the GPU's thread; the player and `boot` both
take frames, and the picking rules live in the renderer.

The rule for which picture to show is in the player (`show_between`,
`new_frame_before`) and copied into `boot` (`trail_new_before`); the
renderer counts `game_frames`; the player reaches into `pica_gpu->screens`
to find the address to read. All of it moves behind one interface in the
runtime, something like:

```cpp
struct Frame { uint64_t number; Picture top, bottom; bool between; };
bool next_frame(Frame &out); // the next picture to show, in order
```

fed by the GPU thread at the end of each game frame (`docs/problems.md`,
flashes 1 and 2). The player and `boot` both just take frames.

## 3. A session shared by the player and `boot`

Done (2026-11-01): `runtime::Session` (`include/runtime/session.h`) opens
the game for both; the globals went into it. The GPU stays each one's
(the player's presents, `boot`'s takes shots).

Both open a game the same way: the program (exheader, code), the title,
the library, `Services`, `Fs` and the romfs, the decomp, `System`, `load`,
`link_code`, the GPU (Pica, Vulkan, AsyncGpu). It is written twice, and
`boot` already differs (AsyncGpu off). A `runtime::Session` in the library
does it once from the `[game]` settings; the player keeps the window,
input, sound and overlay; `boot` keeps the recording and the shots. The
globals the player needs today (`library`, `services_ptr`, because
`System` takes plain function pointers) go into it.

## 4. The player's `main`

Done (2026-11-01): `main` reads the settings, opens the session, the
renderer and the presenter, starts the game's thread and loops; the parts
are functions of their own (`read_settings`, `start_overlay`,
`feed_sound`, `read_input`, `Dump`, `Tally` for each second's numbers).

`frontend/player.cpp`'s `main` is one function with a 150-line lambda run
on the game's thread. Once the game has its own thread
(`docs/problems.md`, performance 1): `main` opens the session and starts
the game thread, then loops on events, input, sound and presenting, each a
function of its own.

## 5. Zakuro's renderer out

Done (2026-11-01): `bridge/`, `ZakuroGpu`, the `RUNTIME_ZAKURO_GPU` option
and `boot`'s `gpu` struct of functions are gone.

R3 made the runtime's renderer the only one. Left over from the bridge:
`bridge/` (a Rust crate and its `Cargo.lock`), `ZakuroGpu` in
`include/runtime/gpu.h`, the `RUNTIME_ZAKURO_GPU` option and its `#ifdef`s
in the player and `boot`, the `video.renderer` setting and its combo, the
`gpu` struct of `std::function`s in the player. All of it can go; Zakuro
stays a separate program that `tools/compare.py` runs to compare.

## 6. Services one file each

Done (2026-11-01), but for the one list: `cfg`, `dsp`, `ldr_ro`, `gsp`,
`hid` and `y2r` each have a file in `src/services/`; `services.cpp` keeps
the dispatch, srv, APT, `tick` and the online ones; `src/services.cpp` is
`src/service_list.cpp`. The two lists of what is native or offline are
still two (they answer different questions: the C interface's, and which
services report no network).

`src/services/services.cpp` (1,169 lines) holds srv, APT, cfg, dsp,
ldr:ro, gsp, hid, y2r and the online ones; `fs.cpp` is already apart.
One file per service, as Zakuro has them (`services/gsp.cpp`,
`services/dsp.cpp`...), `services.cpp` keeping `request` and `tick`.

There are also two lists of the same thing: `src/services.cpp` (the C
interface's `runtime_service_answer`: native or offline per service) and
`network()` in `services/services.cpp`. One list, used by both. The two
files called `services.cpp` (one in `src/`, one in `src/services/`) stop
sharing a name.

## 7. The kernel's threads

Enough as it is (2026-11-01): scheduling goes over the live threads only
(`live_`) and TLS slots are reused; `boot` from the title to the field
spends 0.1 s scheduling in 1710 frames, so a list of the ready threads
apart would save nothing that shows.

With dead threads reused (`docs/problems.md`, performance 2), `Kernel`
keeps a free list of thread slots and of TLS slots, and the ready threads
apart, so scheduling does not scan every thread.

## 8. The repository

Done (2026-11-01): `boot`'s shots go to `build/shots/`, `*.ppm` was already
ignored, the files are LF in the repository (`.gitattributes` says so),
`CLAUDE.md` knows the GPU's thread.

- `shot-*.ppm` in the root: `boot` writes into the current folder by
  default. Default to `build/shots/`, and ignore `*.ppm`.
- Line endings: `fs.h` and `fs.cpp` are LF where the rest is CRLF; a
  `.gitattributes` settles it.
- `CLAUDE.md` says "cooperative threads on one host thread for now";
  the GPU has its own thread since AsyncGpu.
- `docs/vulkan-renderer.md` and `README.md` to follow the split.

## Order

1 (renderer split) and 8 first, since the fixes in `docs/problems.md` land
in the renderer; then 3 and 4 together with the game's own thread; 2 with
the frames between fix; 5, 6 and 7 whenever, each alone.
