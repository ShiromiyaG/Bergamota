# Known problems and their causes (2026-10-31)

What the player showed wrong, found by reading the code. Each entry: the
symptom, the cause with where it is, how sure we are, and the fix. The order
at the end is the order they were fixed in; "Status" says what is done.

## Status (2026-10-31)

Fixed: flashes 1-6, performance 1, 2 and 5, sound 1 and 2. `boot` runs
AsyncGpu by default. Checked with `boot` on a bot recording (1710 frames,
"60 FPS" on): frames between and the game's alternate exactly, none of the
frames shown stands out from both neighbors, and 324 of 329 moving frames
fall between theirs (the rest are texture animations, which a frame between
does not move); the player shows 60 frames a second with the game's thread
idle 90% of the time.

- Threads: `Kernel::live_` holds the threads not dead, which is all the
  scheduler looks at; an ended thread's TLS goes back (`Hooks::free_tls`).
- Sound: `Services::tick` makes one DSP frame a call and keeps the debt
  (up to 0.1 s); the player keeps SDL's queue near 60 ms by playing up to
  0.5% faster or slower.
- Frames: each vertical blank is an item in the GPU's queue
  (`Gpu::vblank`). On the GPU's thread, in order with the command lists,
  `VulkanRenderer::vblank` copies what the screens show (the frame between
  for the top one when it is due) and hands on the copy of the blank
  before as a numbered `Frame` (`Gpu::next_frame`). Frames between are cut
  at the display transfer into a top screen buffer (`frame_end`), so they
  are whole; they are drawn only when the frame came two blanks or more
  after the last one; draws match on vertex count, textures and combiners;
  a vertex that jumps more than a quarter of the screen is drawn where it
  lands; depth and stencil are seeded; a buffer read as a texture is read
  from its between image; partial fills in a replay are skipped; images
  let go mid-frame live until the frame is replayed. A frame between shows
  on the blank the screen moves to the new buffer (the game swaps a blank
  after its transfer), the game's own on the next.
- Presenting: the player runs `System::run` on a thread of its own; the
  main thread reads the input (handed over under a lock) and shows the
  frames in order, two waiting at most.
- Small costs: staging buffers reused (`staging_buffer`, `give_back`),
  `RUNTIME_VK_DEBUG` read once, the per-draw timers and the shape counter
  gone.

Found after, by looking at what the player showed (2026-11-01):

- The intro's movie went back and forth between a new picture and one
  frames old, "60 FPS" on or off. The game is CPU-bound there (the game's
  thread never idle), and when it falls behind it skips drawing a frame but
  still flips to its other buffer. `VulkanRenderer::vblank` keeps the
  newest picture when a screen flips to a buffer whose last transfer or
  write is older than the one shown (`picture_in`).
- Black cracks between pieces of the ground in frames between, when moving.
  Repeated pieces (a curb's, a row of tiles) sample the same part of their
  texture, and the first one alike was often the neighbor's; and the jump
  test counted vertices near or behind the eye and far off the screen,
  where a small move projects to a huge one, so a piece was drawn where it
  is now while its neighbors moved halfway. A draw now pairs with the one
  alike in its first texture coordinates that moved the least (`match`),
  and moves are measured on the vertices in front of the eye and on or
  near the screen (`moved`).

Where the game's CPU time goes (2026-11-01, `boot` with `RUNTIME_SAMPLE`,
on the host's clock):

- The intro's movie: `Library::lookup` was 31% of the game's thread, a
  binary search on every indirect jump; cached, 4%. What is left is the
  game's own code, nearly all on one thread (priority 0x31): the Mobiclip
  decoder (`MovieLib`) and the movie player polling its counters
  (`mwmomobiclipDecoderGetNbPushedFrame`, `DemuxerStreamAt`, the critical
  sections), which is its wait between frames. It decodes at the movie's
  30 frames a second.
- The field, walking: the game's thread idle 82% of the time (the game
  draws at about 30 frames a second, as USUM does); its code does not
  reach the top of the samples. The GPU's thread is the busy one: 21.8 s
  of 92 in the vertex stage (performance 4).

To look at what the player shows: `RUNTIME_DUMP=<folder>` in the player;
`boot` with `RUNTIME_REPLAY`, `RUNTIME_TRAIL` and `RUNTIME_TRAIL_FROM` on the
host's clock, as the player runs.

The vertex stage on the GPU (performance 4, done 2026-11-01): the CPU
gathers each draw's input registers and its triangles as indices
(`VertexStage::gather`); `src/vk/pica_gpu.vert` runs the game's vertex
shader program for each vertex, as `src/pica/shader.cpp` does, and places
its outputs. `boot`'s shots are the same as with the CPU's stage
(`RUNTIME_CPU_VERTICES=1`), with no validation errors; walking in the
field, the GPU's thread worked 14.4 s where it worked 33.4 s (the vertex
stage 4.8 s where it was 19.4 s); at the title screen the player's GPU
time went from about 450 to 215 ms a second. Frames between: the shader
runs the program twice, with the frame before's inputs and uniforms and
with this one's, and puts the vertex between; a draw pairs with the one
with the same inputs, indices and program whose uniforms differ the least.
The jump test on that path (2026-11-01): when a pair's uniforms differ,
eight of its vertices run through the program on the CPU for both frames
(`sample_positions`, each draw once) and the same test as the CPU's
decides; a pair that jumps is drawn where it is now. `between_test` checks
it with a triangle moved a little and across the screen. Immediate-mode draws and the
geometry stage stay on the CPU.

Each vertex program translated to a shader of its own (2026-11-01,
`src/vk/translate.cpp`): an interpreter on the GPU cost more of the GPU's
time than the CPU's stage did. Each instruction is decoded once and
written into GLSL with its operands, swizzles and mask; the flow keeps the
interpreter's blocks, a switch on the program counter with a case only
where the flow can land. shaderc (from the Vulkan SDK, linked in) compiles
it on a few threads of their own, which make the pipelines too; until
then the interpreter draws. The SPIR-V and the driver's pipeline cache are
kept in a folder (the player's: `shaders` beside its settings; `boot`:
`RUNTIME_SHADER_CACHE`), so a program is translated once. The host GPU's
own time (timestamps, `gpu_seconds`) over 5000 frames walking in the field:

| | without "60 FPS" | with |
| --- | --- | --- |
| vertex stage on the CPU | 6.2 s | 11.8 s |
| interpreted on the GPU | 10.4 s | 29.5 s |
| translated | 5.2 s | 13.5 s |

With "60 FPS" the program ran twice a vertex; since, each draw of the
game's writes its vertices' outputs (pica_gpu.vert, outputs: two halves, a
game frame each in turn) and its frame between takes them, running nothing:
7.0 s with "60 FPS" where it was 13.5 s (the frames between 1.1 s where
they were 8.3 s), the same checks passing. `boot`'s shots are the
same as with the CPU's stage; no validation errors. With an empty cache,
90% of the draws already use the translated shaders.

Presenting through the renderer's own device (performance 3, done
2026-11-01, `src/vk/present.cpp`): at each vertical blank the GPU's thread
copies the screens, on the GPU, into one of eight sets of images
(`Frame::set`); the player's `VulkanPresenter` has a swapchain on the
window and draws both screens from the set, letterboxed, turned and
filtered in a shader, with Dear ImGui's Vulkan backend over them. Two
timeline semaphores order it: the renderer's submissions (the set is
filled) and the presenter's (the set can be filled again); the queue is
shared under a lock. The pictures go to the CPU only when asked
(`cpu_pictures`: `boot`'s shots, `RUNTIME_DUMP`). No validation errors;
`boot`'s shots unchanged. The launcher keeps SDL's renderer.

Nothing of the list is left.

## What the renderer lacks (2026-11-01)

`boot` and the player count the draws that ask for what the renderer does
not do (`VulkanRenderer::stats.lacking`; the player notes the first of
each in a session in `renderer-lacks.txt` beside its config). From the
title to the field a third of the draws used fog, which it then ignored.

Done since, each checked by `render_test` (command lists made by hand,
the buffers read back): fog and procedural textures (from Zakuro's, MIT);
the geometry shader in its three modes (points from Zakuro's; variable
and fixed primitives); the w buffer (a second build of `pica.frag` writes
the depth, chosen by the pipeline's key so the rest keep the early depth
test); texture 0 projected, as a cube map, and as a shadow map (2D and
cube); the shadow output (a third build of `pica.frag`, with no
attachment, keeps the map's word, depth over intensity, through an atomic
compare-and-swap on the color buffer seen as R32_UINT). What Zakuro does
not have was written from the hardware's behavior, Azahar (GPL) read as a
reference only, none of its code taken. None of these showed from the
title to the field.

The counter keeps what is left: gas, which Azahar does not do either, and
a geometry shader mode past the three there are.

## Flashes with "60 FPS" (frames between)

The frames between work in `boot` and flash in the player. `boot` runs the
GPU on the game's thread (`RUNTIME_ASYNC` off by default) and cuts frames at
each vertical blank; the player runs `AsyncGpu` and cuts them at its own
presentation tick. Every cause below comes from that difference.

1. **Frames cut at the presentation tick, not at the game's frame** (sure).
   The ops (draws, fills, transfers) are cut when the player calls
   `prepare_between` (`frontend/player.cpp`, `take`), every 16.7 ms of wall
   clock. `locked()` drains the GPU queue first, so a tick that lands while
   the GPU thread is inside a command list sees that frame's draws but not
   its display transfer. Then `frame_between(false)`
   (`src/vk/renderer.cpp`, `frame_between`) moves the half frame into
   `draws_before` and clears the ops; the next tick replays only the rest,
   on top of a copy of the finished frame (ghosts), and `draws_before[target]`
   is replaced by that rest, not appended, so the next frame's draws are
   matched against the wrong list: vertices lerp toward unrelated draws.
   With the vertex stage taking ~8 ms of each 30 Hz frame on the GPU thread,
   a good share of ticks land inside a command list.
   Fix: cut frames inside the renderer, on the GPU thread, at the top
   screen's display transfer (the end of a frame), and keep whole frames;
   the presentation side only picks finished ones.

2. **Two clocks** (likely). The player ticks at least 16.667 ms apart
   (`next_frame = now + 16667 us`, so it drifts later), the game's vertical
   blank comes every 16.714 ms (`Services::tick`, 59.83 Hz), and the shown
   buffer swaps at the vertical blank. The rule "between on the tick a new
   frame comes, the game's on the next" (`show_between`) only holds while
   the two stay in the same phase; as they drift, a tick sees two blanks or
   none, and the picture steps back half a frame or repeats. Screens are
   also read back a call behind (`picture`, `ScreenCopy`), which the rule
   counts on. Fix: present once per emulated vertical blank (or tag each
   picture with its game frame and pick by number), not on a clock of its
   own.

3. **Weak matching** (sure it happens, not how often). A draw is matched to
   the next one of the frame before with the same target and the same
   vertex count, within 48 (`draw_between`). Most 2D quads have 6 vertices:
   when one appears or goes, every quad after it is paired with the wrong
   one and drawn halfway across the screen for a frame. Fix: match on the
   textures and the shader too (and the vertex count), and draw a pair that
   moves too far (a share of the viewport) at the new position instead.

4. **The between depth buffers are not seeded** (sure). `draw_between`
   copies only color images into `between`; a depth image keeps what the
   last replay left. A replay without its depth clear (see 1) tests against
   an old frame's depth, and parts vanish. Fix: copy depth too, or only
   replay whole frames (1) and assert the clear is there.

5. **A stale picture after a gap** (sure). `picture_between` returns false
   while any surface lacks a `between` image (a surface made since the last
   replay: scene changes), without advancing its `ScreenCopy`. The next
   success returns the copy kept from before the gap, an old frame, for a
   tick. The same happens when "60 FPS" is switched on. Fix: drop the kept
   copy when a call fails (`serial = 0`), or give each picture its frame
   number (2).

6. **Replays touch the game's state** (minor). A partial fill during a
   replay skips the write-back (`write_back` returns while `replaying`) but
   still marks the surface stale, so the next `surface()` uploads older
   memory into the between image.

## Performance

1. **The game's thread does the presentation** (sure). `System::run` calls
   the player's callback between slices, on the only thread that runs the
   game. Every 16.7 ms it drains the GPU, replays the frame between, reads
   both screens back (fence waits), turns them by hand, uploads them to
   SDL's renderer, draws ImGui and presents (blocking up to a refresh with
   vsync on). All of it is time the game is stopped, and it is also what
   breaks the sound (below). Fix: the game on a thread of its own; the main
   thread presents what the GPU finished, through a mailbox.

2. **Threads are never freed** (sure). `Kernel::start` appends to
   `threads_` and nothing removes a dead thread; `schedule`, `pick_next`
   and `next_wakeup` scan all of them on every slice. Each thread also gets
   a TLS slot (`System::allocate_tls`, 0x200 bytes, a Base page per 8) that
   is never given back, so after ~2000 threads the TLS addresses run 1 MB
   past `TLS_VADDR`. The scheduling cost grows with play time. This, not
   `nwm::UDS`, is the "2000 threads" problem: the runtime answers UDS
   exactly as Zakuro does (`UDS_WIRELESS_OFF`), so the game makes those
   threads under Zakuro too, which reuses them. Fix: reuse dead threads'
   slots and TLS; keep the ready threads in a list of their own.

3. **Screens go GPU → CPU → GPU** (sure). Each tick both screens are copied
   to host memory, rotated per pixel on the CPU and uploaded into SDL's
   renderer (another device, D3D on Windows). Fix: present from the
   renderer's own Vulkan device (a swapchain on the SDL window, ImGui on
   Vulkan), as PLAN 6.6 already says. It also removes a frame of latency.

4. **The vertex stage on the CPU** (known). About 14 s of every 60 s in the
   field, now on the GPU thread. Fix: run the PICA's vertex shaders in a
   Vulkan shader that reads the program from a buffer (Zakuro JITs them).

5. **Small costs in hot paths** (sure, small). `std::getenv` on every
   texture lookup (`texture`, `RUNTIME_VK_DEBUG`) and every picture
   (`RUNTIME_VK_NO_PICTURE`), which takes the CRT's lock on Windows; three
   `steady_clock::now()` per draw; a `vkAllocateMemory` for every upload
   (`copy_in`) and every write-back; a copy of every register per draw
   while "60 FPS" is on (`note_draw`). Fix: flags read once, a staging
   ring, timers only when stats are asked for.

## Sound (slow, cutting, crackling)

1. **Audio frames dropped when a tick is late** (sure). `Services::tick`
   makes one DSP frame (160 samples, 4.9 ms) per call when it is due; if
   the call comes more than a frame late, `next_frame` jumps to now and the
   missed frames are never made (`src/services/services.cpp`, the end of
   `tick`). The game then mixes less than real time and SDL's queue runs
   dry: cuts and crackle. The late calls are the presentation work in the
   same callback (performance 1). Fix: make every missed frame (with a cap),
   and get the presentation off the game's thread.

2. **No rate control** (likely). The player queues whatever comes, drops a
   chunk above 0.25 s and does nothing below. Once 1 holds, keep the queue
   near a target (50-80 ms) by resampling a fraction of a percent.

## Order

1. Free dead threads (performance 2): small, alone, and it grows with time.
   Done, as are 2, 4 and 5; 3 is left (`docs/organization.md`, 1).
2. Make every missed DSP frame (sound 1): a few lines.
3. Split `src/vk/renderer.cpp` (`docs/organization.md`) before touching the
   frames between.
4. The game on its own thread, the presentation on the main one
   (performance 1); with it the vertical blank drives presentation
   (flashes 2).
5. Frames between cut at the top transfer on the GPU thread, whole frames
   only, better matching, depth seeded, no stale pictures (flashes 1, 3-6).
6. Presentation through Vulkan (performance 3), then the vertex stage on
   the GPU (performance 4).

`boot` should run as the player does by default (`AsyncGpu` on), or it will
keep missing what the player gets wrong.
