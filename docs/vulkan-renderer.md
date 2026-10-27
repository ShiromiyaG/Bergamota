# The Vulkan renderer

The runtime's own PICA200 renderer, replacing the bridge to Zakuro's. Written
for the three ports, not as an emulator: lean where Zakuro is general, and
built so that nothing compiles while a game runs.

## Shape

```
command lists ──> Pica (front end)            src/pica/
                    registers, shader uploads, fixed attributes,
                    fill / transfer / copy engines
                      │ draw
                      v
                  vertex stage (CPU)          src/pica/shader.cpp
                    attribute loading, the PICA vertex shader
                    interpreted, primitive assembly
                      │ triangles + a state block
                      v
                  Vulkan back end             src/vk/
                    one pipeline layout, one ubershader pair
                    (fragment: TEV, lighting, fog, alpha test from a
                    state block), pipelines only for blend/depth/stencil
                    variants; surfaces (render targets) and textures
                    cached by address
```

## Hybrid, decided 2026-10-26

The renderer translates what the games send the PICA (its registers, shader
programs and combiner setup) into Vulkan: it works for all three games
without the engines decompiled. Native rendering comes on top of it, where
it pays, as hooks in each engine (Solstice's gfl2, the Gen 6 engine for
Duality and Tidefall; PLAN 6.3): the camera and aspect for widescreen, the
UI laid out for the single screen, effects of our own. Materials stay PICA
data either way, their vertex shaders and combiners come from the models.

## Decisions

- **No shader compilation at run time.** One vertex and one fragment shader,
  GLSL compiled to SPIR-V by `glslc` at build time and embedded. The PICA's
  fixed function (TEV stages, combiners, lighting, fog, procedural textures)
  is data the fragment shader reads, not code generated per state. Zakuro
  generates and compiles shaders per state, which is where its hitches come
  from.
- **Vertex shaders on the CPU first.** The PICA vertex shader is interpreted
  on the host CPU (a frame of USUM is tens of thousands of vertices, a few
  milliseconds). Moving it to the GPU (a vertex-shader interpreter reading the
  program from a buffer) is a later step, if it shows up in profiles.
- **Few pipelines.** Vulkan 1.3 dynamic state covers viewport, scissor, cull
  mode, depth test/write/compare, stencil; the remaining fixed state (blend
  equation and factors, color write mask, logic op) keys a small pipeline
  cache, warmed from disk.
- **Surfaces stay on the GPU.** A render target is an image keyed by its
  address, format and size. Memory gets its contents only when something
  else reads it: a display transfer the GPU cannot do itself, the CPU (the
  runtime's page guards), a service. The screens are shown from their images
  without a trip through memory.
- **Textures** are decoded on the CPU into RGBA8 and uploaded, keyed by
  address, format, size and a hash of their bytes; a texture that is a
  surface (render to texture) is used in place.
- **One device, one queue, one thread** for now, matching the runtime's
  cooperative scheduling.

## Milestones

State (2026-10-27): 1-5 done, 6 under way. USUM draws right from the intro
to the field and battles, lit, at up to 4 times the 3DS's resolution,
shown straight from the GPU; the player keeps about 58 fps in the field.
In the field, its frames are within a mean of 1.4 (of 255) of Zakuro's
software rasterizer, 1.6% of pixels off by more than 16.

1. **Front end.** (done) Command lists, registers, shader and table uploads, fixed
   attributes and immediate mode, and the fill, transfer and copy engines on
   memory. Draws counted, not drawn. Selectable beside the Zakuro bridge.
2. **Vertex stage.** (done; 16 vertices at a time, across the CPU's cores) Attribute loading, the shader interpreter, the geometry
   stage's absence checked, primitive assembly, clipping left to Vulkan.
3. **First pixels.** (done; fills and display transfers on the GPU, a
   depth buffer read as a texture converted by a compute shader, the screens
   read back a frame behind) Vulkan device, surfaces, textures, the ubershader with
   TEV, blending and depth/stencil; display transfers from surfaces; the
   screens shown. USUM's title screen and field compared with Zakuro's
   (`tools/compare.py`).
4. **The rest of the fixed function.** Fragment lighting (done: the lights
   decoded on the CPU into a block, `pica_lighting.h`, when the registers
   change; the 24 tables in a storage buffer, sent when they change; the
   shading per fragment in `pica.frag`, bump and shadow inputs included),
   fog (done: its table after the lighting's, applied after the alpha
   test, the depth times w in the w buffer, which is the depth map turned
   off), the logic op (done: a pipeline per operation, on devices with the
   feature). Procedural textures, gas and the shadow mode: none of USUM's
   draws used them in the intro, the title, the house, the first route, the
   starter scene, a town from a finished save and a wild battle in a cave
   with a Z-move (`boot` counts them, over 1.3 million draws); fog was in
   about half of them, the logic op in none. Left for when a game's draws
   do; Zakuro, the reference, has no gas and no shadow mode either. The
   battle's frames are within a mean of 0.4-4.8 of Zakuro's.
5. **Performance and scale.** Surfaces at a resolution scale, the swapchain
   presenting surfaces directly, the pipeline cache on disk, the CPU's reads
   of surfaces through the guards. Done:
   - *Pipeline cache*: a `VkPipelineCache` from a file (the player's
     `pipelines.bin` beside `config.toml`, `RUNTIME_PIPELINE_CACHE` for
     `boot`), saved on the way out and every few new pipelines. The
     ubershader keeps them few (8 in a room, 22 in a battle); creating them
     went from 5-8 ms to 1 ms on the second run.
   - *Direct presentation*: the player's window is the renderer's own
     swapchain; `present.vert/.frag` draw each screen from its surface,
     turned upright, smooth or sharp, letterboxed, and Dear ImGui's Vulkan
     backend draws the overlay in the same pass. No read-back and no frame
     of delay; `RUNTIME_NO_PRESENT`, or a window that cannot be shown in,
     goes back to reading the screens and the SDL renderer.
   - *Resolution scale* (`video.scale` 1-4, `RUNTIME_SCALE`): one scale for
     every surface; images, render area, viewport, scissor and display
     transfers at it, memory at the buffers' own size (uploads enlarged,
     write-backs and `picture()` from the middle of each block, the middle
     four averaged at an even scale). At 1× the frames are the same byte for
     byte; at 2×-4× scaled down they are as near Zakuro's as 1×, but for
     the phase of textures not drawn 1:1 and edges the scaling down smooths.
   - *Guards*: the CPU's reads of a surface the GPU drew write it back
     first, as they did already.

6. **The rest of the hardware.** What the PICA200 does that the renderer
   did not, counted first (`boot`'s "draws using" lines), over the three
   recordings (the intro to the field, 845 thousand draws of gameplay, a
   battle):
   - a geometry shader: 2,782 of a battle's 451 thousand draws (none
     elsewhere), which the renderer dropped without a word. Done: run on
     the CPU after the vertex shader (mode 0, as Zakuro; another mode is
     counted as refused), SETEMIT's bit 22 turning a triangle round as
     Citra has it. The battle's are a particle system whose vertex shader
     finds every particle past its life, so the geometry shader emits
     nothing there and the frames stay the same;
   - textures with mipmap levels: 9-34% of the draws, sampled at level 0
     only. Done: the levels after the first in memory, each half the last
     and none smaller than a tile, into the image's levels; the sampler
     from the level register (bias, most and least level) and the filter
     between levels (bit 24). Sampled before the border's branch, which
     goes by the pixel. Zakuro has none: what changed are the textures drawn
     smaller than their size, the edges of alpha-tested leaves and far
     ground (up to 2% of a battle frame's pixels);
   - the scissor test: on in every draw, keeping the screen's 240×400 (or
     240×320) of a 256×512 buffer; the rest of the buffer is drawn into
     but not shown. Done: the inside of the rectangle is the pipeline's
     scissor, the outside a discard in pica.frag, rows turned over as the
     viewport's and at the scale (pica_scissor); USUM's frames stay the
     same;
   - cube maps, shadow textures, projected textures, the scissor keeping the
     outside, procedural textures, the shadow mode and gas: none (the
     procedural texture counted by its own bit, 10 of the texture
     configuration, from part E on: still none).

   What no USUM draw uses is checked by `render_test`: a quad drawn by the
   renderer into a buffer, read back, each pixel against the CPU's own
   reading of the same registers.

   - the procedural texture: done, `proctex()` in pica.frag, its tables
     in the shader's buffer after the fog's. Its twin on the CPU,
     `proctex_sample` (tables.cpp), is Zakuro's proctex.rs translated, and
     gave the same values as Zakuro's to a millionth over four set-ups; the
     GPU's frames match it to the pixel (4 of them, 47-1,467 colors each).

   - unit 0's types: done. A cube map's six faces (+x at the unit's
     address, the rest at 0x086-0x08A over the bits they share) go into a
     cube image turned over, so it is sampled at (u, v, w) as Citra samples
     its own; a projected texture divides u and v by w; a shadow texture,
     flat or in a cube, compares four texels' depth (red, green, blue) with
     w less the bias (0x08B) and mixes their penumbras (alpha), as Citra
     worked it out (Azahar's shader generator read, the code written anew;
     Zakuro has none of them). render_test checks each against the CPU's
     reading of the same: 2D, projected, cube (x and y faces, z faces),
     shadow (orthographic, perspective) and shadow cube, all to the pixel
     but the few where a face or a texel's edge is all but tied.

   - the shadow mode (the output merger's mode 3): done. The color buffer
     holds a depth (24 bits) and a penumbra (8) a pixel; the fragment
     shader writes it through a 32-bit view of the buffer's image, in a
     compare-and-swap loop, in a pass with no color attachment (whose store
     would undo it): nearer with no penumbra of its own puts its depth, with
     one puts it scaled by the bias in 0x130 (two 16-bit floats) when less.
     As Citra worked it out, written anew. A device without fragment
     atomics refuses these draws (counted). render_test fills a buffer,
     draws over it twice (no penumbra, then texture 0's green), and the
     words back match the CPU's reading to the pixel.

   Next: gas, for the other ports.

Zakuro's renderer (`zakuro-dev/crates/zakuro-gpu`) is the reference for the
hardware's behaviour; the code here is written anew.
