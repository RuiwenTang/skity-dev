# Architecture

Skity is layered strictly top-down: public API → domain objects → renderer → GPU abstraction → backend. A draw call never skips a layer, and each layer only knows the one below it.

## Layering

The layers and their homes in the tree:

| Layer | Location | Knows about |
| --- | --- | --- |
| Public API | `include/skity/` (graphic, geometry, text, effect, render, recorder, gpu, io, utils) | — |
| Domain / geometry | `src/graphic/`, `src/geometry/`, `src/effect/`, `src/text/` | public API |
| Renderers | `src/render/` (`hw/`, `sw/`, `text/`) | domain + GPU layer |
| GPU abstraction | `src/gpu/` (see [[gpu]]) | backend APIs |
| Backends | `src/gpu/{gl,vk,mtl,web}/` | raw graphics APIs |
| Satellite modules | `module/` — [[capi]], codec, io (streams/SKP), [[wgx]] | public API |

Deliberate inversion points: `HWPipelineDescriptor` exists so the draw layer never sees `GPUShaderFunction` details; `GPUSurfaceImpl` owns an `HWCanvas` (not the other way round), creating a backend root layer per frame.

## Draw-call data flow

Life of a draw call on the GPU path, end to end:

1. `Canvas::DrawPath` (template method in `src/render/canvas.cc`) applies state then calls the virtual `OnDrawPath`.
2. `HWCanvas::OnDrawPath` (`src/render/hw/hw_canvas.cc`) may promote simple shapes to the RRect fast path, then `DrawShape` runs early-outs (unready texture, empty path, quick-reject).
3. `HWCanvas::DrawPathInternal` picks the AA mode, applies path effects, splits fill/stroke via `DrawFillStrokeInPaintOrder`, and allocates a draw object (`HWDynamicPathDraw`, `HWDynamicRRectDraw`, `HWDynamicCoveragePathDraw`) from the frame arena.
4. `HWLayer::AddDraw` records it into the current layer (offscreen layers come from `NeesOffScreenLayer` — only image/mask filters; color filters are folded into generated WGSL), attempting merge with recent draws.
5. On `Flush`: `Prepare` walks the layer tree, each draw emits `HWDrawStep`s (geometry + fragment + stencil state); pipelines resolve via `HWPipelineLib`; meshes upload through `HWStageBuffer`/`HWStaticBuffer`; each `HWDrawPass` opens a `GPURenderPass` and replays its `Command`s.
6. `GPUCommandBuffer::Submit` hands everything to the backend.

Details per stage live in [[render#Frame lifecycle]] and [[gpu#Render passes and commands]].

## Cross-cutting decisions

Rules that apply everywhere and explain many local choices.

- **Zero exceptions / zero RTTI / hidden visibility** across the core and C API (CMake enforces `-fno-exceptions -fno-rtti -fvisibility=hidden`; tests keep no-RTTI). The C API uses nothrow `new`. This keeps the library embeddable and shrinks the binary — size is a core KPI.
- **WGSL as the single shader source**, translated in-tree by wgx (see [[wgx]]) instead of shipping shaderc/glslang/SPIRV-Cross per platform.
- **Frame-scoped arena + vector recycling**: draw objects, steps and commands come from `ArenaAllocator` (`src/utils/arena_allocator.hpp`) reset each frame; vertex/index vectors recycle through `VectorCache`.
- **Skia-lineage data formats** where they earned their keep (path verb/point/conic-weight storage, flattenable serialization names, `PackedGlyphID` bit layout), but naming follows Google C++ style (`PascalCase` methods).
- **Binary size budget drives feature routing** — e.g. color filters avoid offscreen layers by compiling into generated WGSL instead.

## Threading model

One `GPUContext` per render thread; texture uploads must happen on the creating thread (documented on `GPUContext`).

`TextureManager` keeps a GPU-thread-only release queue (`ClearGPUTextures`) with `SharedMutex` protection on shared paths. Recorded display lists ([[recorder]]) are the intended cross-thread artifact: record anywhere, replay on the render thread.
