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

State (2026-10-27): 1-3 done, 4 under way (fragment lighting, fog and the
logic op done). USUM
draws right from the intro to the field, lit; the player runs it at about
56 fps in the field. In the field, its frames are within a mean of 1.4 (of
255) of Zakuro's software rasterizer, 1.6% of pixels off by more than 16;
17.6 and 52% before the lighting.

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
   draws used them in the intro, the title, the house, the first route and
   the starter scene (`boot` counts them); fog was in 56% of them. Left for
   when a game's draws do (battles still to be counted); Zakuro, the
   reference, has no gas and no shadow mode either.
5. **Performance and scale.** Surfaces at a resolution scale, the swapchain
   presenting surfaces directly, the pipeline cache on disk, the CPU's reads
   of surfaces through the guards.

Zakuro's renderer (`zakuro-dev/crates/zakuro-gpu`) is the reference for the
hardware's behaviour; the code here is written anew.
