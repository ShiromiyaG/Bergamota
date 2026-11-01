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

Left: performance 3 (presenting through the renderer's own Vulkan device)
and 4 (the vertex stage on the GPU). Both are now off the game's thread:
3 costs a frame of latency and a little CPU on the main thread, 4 keeps
the GPU's thread busy (about 45% of each second at the title screen), and
the game waits for that thread when it reads back what it drew.

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
