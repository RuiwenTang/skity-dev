# Render Pipeline

The render layer (`src/render/`) turns `Canvas` calls into GPU commands (`hw/`) or CPU spans (`sw/`), and dispatches glyphs (`text/`). The overall draw-call journey is [[architecture#Draw-call data flow]]; this page goes deeper per subsystem.

## Canvas backends

Three concrete canvases implement the `Canvas` hooks:

- `HWCanvas` (`src/render/hw/hw_canvas.hpp`) — all GPU backends; created and owned by `GPUSurfaceImpl`, which calls `BeginNewFrame(HWRootLayer*)`. AA mode selection (`SelectAnalyticalAA`, `AnalyticalAAMode {kNone, kContour, kCoverage}`) happens here.
- `SWCanvas` (`src/render/sw/sw_canvas.hpp`) — software; created via `Canvas::MakeSoftwareCanvas(Bitmap*)`.
- `RecordingCanvas` — [[recorder#Recording flow]].

## Frame lifecycle

From recorded draw objects to a submitted frame, inside `HWCanvas::OnFlush` and friends:

1. **Draw objects** — created during recording from the frame arena: `HWDynamicDraw` base with `HWDynamicPathDraw`, `HWDynamicRRectDraw`, `HWDynamicCoveragePathDraw`, `HWDynamicTextDraw`/`HWDynamicSdfTextDraw`, plus `HWDynamicPathClip`. Each attaches to the current layer (`HWLayer::AddDraw` sets scissor/clip/bounds; dst-read copies split a new `HWDrawPass`; `TryMerge` coalesces with up to 5 recent non-overlapping draws — coverage draws are not mergeable yet).
2. **Layers** — `HWDraw` forms a tree: `HWRootLayer` (per-backend subclasses in `gl/ mtl/ vk/ web/`), `HWSubLayer` (saveLayer offscreen), `HWFilterLayer` (filter DAG execution). `NeesOffScreenLayer` gates layer creation: only image/mask filters qualify — color filters compile into WGSL instead.
3. **Prepare** — `HWDynamicDraw::OnPrepare` runs subclass `OnGenerateDrawStep` producing 1–2 `HWDrawStep`s and aggregating stencil/depth bits into `HWDrawState`.
4. **Commands** — each step resolves a pipeline from `HWPipelineLib` by `HWPipelineKey`, then its geometry fills vertex/uniform/bind-group data into a `Command` ([[gpu#Render passes and commands]]).
5. **Upload & submit** — `HWStageBuffer::Flush` + `HWStaticBuffer::Flush` upload meshes; root layer opens `GPURenderPass` per `HWDrawPass`, replays clip steps, appends draw commands; `GPUCommandBuffer::Submit` finishes.

## Frame boundary and save/restore state

`Canvas::Save()`/`Restore()` state is split across two stores with different lifetimes; whether an unbalanced `Save()` at flush time corrupts later frames depends on how the caller manages canvas lifetime.

Canvas lifetime has two modes. Unlike Skia (where a `SkCanvas` is typically long-lived), skity canvases are often one-shot: any integration that recreates the `GPUSurface` per frame (texture-mode rendering, golden/bench harnesses) gets a brand-new `HWCanvas` each `LockCanvas`. The other mode holds one `GPUSurface` across frames (example windows, on-screen surfaces); there `GPUSurfaceImpl` caches its `canvas_` member and reuses the same `HWCanvas` indefinitely.

- `Canvas` base (`canvas.hpp`): `save_count_`, the matrix stack (`CanvasState`), and `global_clip_bounds_stack_` live on the canvas object — they survive flush only in the reused-canvas mode.
- `HWCanvas` side: `HWLayerState::clip_stack_` — the source of scissor box, clip bounds and clip depth — lives on the root layer, which is rebuilt every frame in both modes (`LockCanvas` → `OnBeginNextFrame` arena allocation → `BeginNewFrame`; `GPUSurfaceImpl::Flush()` resets the arena).

Verified consequence (offline Metal reproduction, 2026-09): with a reused canvas, a frame ending with `Save()` unbalanced makes `HWCanvas::OnFlush` discard the root layer together with its extra clip-stack entry, while the base-class side keeps the debt (`save_count_` still elevated). The next frame's `Restore()` lands on the fresh root layer; `HWLayerState::PopClipStack` has no lower-bound guard (unlike `LayerState::CanRestore`) and pops the constructor-time entry, leaving `clip_stack_` empty. `HWLayer::AddDraw` then reads `CurrentClipBounds()` as `back()` on an empty vector — out-of-bounds memory — and every draw of that frame, including a full-screen `DrawColor`, is silently culled. Each frame repeats the pattern, so nothing ever renders. With a one-shot canvas the same code renders fine: the new canvas starts at `save_count_ == 1`, so the stray `Restore()` is a no-op and the debt dies with the old object. Same source, two behaviors — the common one-shot mode is what hides this bug. Rule of thumb: balance `Save`/`Restore` within a frame, or restore to a known save count before flushing.

Keeping canvas state across a flush is therefore not supported today, structurally. The matrix stack and `save_count_` do survive on a reused canvas, but the GPU-effective half — `HWLayerState::clip_stack_`/`clip_history_` hold `HWDraw*` into the frame arena (`ArenaAllocator::Reset()` at flush dangling them all), clip effect lives in the stencil attachment which is rebuilt per pass, and `draw_depth_` numbering is per-frame — dies with the frame. The recorder path is no escape either: `DisplayList` scopes a `Save` by back-filling its restore offset, and `PictureRecorder::FinishRecording` auto-closes any unbalanced save. Supporting it for real would mean keeping a draw-pointer-free semantic save/clip stack on the canvas (matrix + Path/Rect per level) and replaying it into each new root layer at `BeginNewFrame` (saveLayer contents still cannot cross a flush); the cheap alternative is auto-restoring to a known count at flush, which unifies both canvas modes.

## Draw steps, geometry and fragments

The composable shader unit system under `src/render/hw/draw/`:

- `HWDrawStep` = one draw pass over a shape = stencil state + one `HWWGSLGeometry` + one `HWWGSLFragment`. Step flavors: `StencilStep`, `ColorStep` (+`ColorAAStep`), `ClipStep`.
- `HWWGSLGeometry` implementations (`draw/geometry/`): `WGSLPathGeometry` (+AA variant), `WGSLTessPathFillGeometry`/`WGSLTessPathStrokeGeometry`, `WGSLRRectGeometry`, `WGSLClipGeometry`, `WGSLFilterGeometry`, `WGSLCoverageAATileGeometry`, text variants.
- `HWWGSLFragment` implementations (`draw/fragment/`): solid color / vertex color, gradient, texture, stencil, text (color/emoji/gradient/SDF), blur and image filters.
- `HWWGSLShaderWriter` assembles VS/FS WGSL from those parts; the `Flags` system (`kSnippet`/`kAffectsVertex`/`kAffectsFragment`) lets a class act either as a full shader or as an injectable snippet — historical, and the reason varying naming conventions (`v_`/`f_` prefixes) matter here.

Pipeline caching sits one level up: `HWPipelineLib`/`HWPipeline` keyed by `HWPipelineKey`/`HWFunctionKey` (`hw_pipeline_lib.hpp`, `hw_pipeline_key.hpp`). `HWBufferLayoutMap` is the singleton registry of vertex layouts.

## Path rasterization strategies

Multiple strategies coexist; selection happens per draw in `HWCanvas::DrawPathInternal`:

1. **Stencil-then-cover** (default fill) — `StencilStep` with winding increments/decrements (front face `kIncrementWrap`, back `kDecrementWrap`; even-odd uses a 0x01 read mask), then `ColorStep` draws where stencil ≠ 0 and clears it. Convex no-AA fills collapse to a single color pass. Geometry comes from `HWPathFillRaster` fan triangulation (front/back triangles bucketed by winding).
2. **GPU tessellation** — `WGSLTessPathFillGeometry` instanced curve segments (`kMaxNumSegmentsPerInstance=16`, 48-byte instance payload with `fan_center`); stroke counterpart inflates segments on GPU. Static meshes come from `HWStaticBuffer`.
3. **Simple-shape fast path** — `WGSLRRectGeometry` + `HWDynamicRRectDraw` computes coverage analytically in the fragment shader (fragment mask), entered via `Path::IsAType`; `NeedsFallbackToPathDraw` guards complexity.
4. **Coverage AA** — see [[render#Anti-aliasing modes]].
5. **Software** — `SWRaster::RastePath` scanline with active edge table (`SWEdge`), spans to `SWSpanBrush` subclasses; see [[render#Software renderer]].

Strokes: CPU path is `HWPathStrokeRaster` — Wang's-formula subdivision, each segment expanded to a rectangle (`ExpandLine`), miter/bevel joins and round/square caps as forward triangles. Analytical-AA strokes are first converted to fill outlines via `Stroke::StrokePath`/`QuadPath` ([[geometry#Stroking]]).

## Anti-aliasing modes

`AnalyticalAAMode` plus MSAA interact as follows:

- **MSAA** — surface `sample_count > 1`; when active, analytical AA is skipped (resolve handled via `GLMSAAResolveRenderPass` on GL).
- **Contour AA** (`kContour`) — `HWPathAAOutline` builds 0.5px fringe geometry, shaded by `ColorAAStep`.
- **Coverage AA** (`kCoverage`) — tiled analytic AA in `src/render/hw/coverage/`: `CoverageAAPathTiler` cuts paths into 16x16 tiles at 1/256 subpixel resolution, `EncodeCoverageAALines` packs segments as 8.8 fixed-point into an RGBA16Uint texture, winding enters as a per-tile backdrop. `CoverageAARenderer::PrepareFrame` batches per-frame data. `CoverageAAMode` (`gpu_surface.hpp`) adds `kConflationCorrection`, which costs extra fragment work and does not fix every artifact — known trade-off, documented in code.

## Blend plans and dst read

`ResolveHWBlendPlan` produces an `HWBlendPlan` per draw from blend mode + fragment properties + `GPUCaps`.

Plan flavors: plain pipeline blending, programmable blending (`WGXProgrammableBlending`, dst read in-shader), texture-copy dst read (`DstReadStrategy`, MSAA emulated via `EmulatedLoadInfo` resolve replay), or native advanced blend (`GL_KHR_blend_equation_advanced`). Capabilities come from [[gpu#Capability negotiation]].

## Filter DAG

Offscreen filtering lives in `src/render/hw/filters/`.

`HWFilters::ConvertPaintToHWFilter` lowers paint filters into an `HWFilter` DAG — `HWBlurFilter`, `HWColorFilter`, `HWMatrixFilter`, `HWMergeFilter`, `HWDownSamplerFilter` — each with `Prepare`/`Filter` phases, executed by `HWFilterLayer` over offscreen textures. What the filter effects mean semantically is in [[effect#Image and mask filters]].

## Caches and buffers

Memory and caching infrastructure under `src/render/hw/`:

- `HWStageBuffer` — per-frame vertex/index/uniform staging, flushed once per frame.
- `HWStaticBuffer` — persistent static meshes (tess tables, RRect grid, unit quad).
- `HWResourceCache` / `HWResourceAllocator` — byte-budgeted LRU for GPU resources; `GPUContext::SetResourceCacheLimit` actually lands here.
- `HWRenderTargetCache` — render-target reuse, **disabled on GL** (see [[gpu#Known backend-specific decisions]]).

## Text rendering integration

`GlyphRun::Make` (`src/render/text/glyph_run.cc`) splits runs by glyph format, then `TextRenderControl` picks the backend.

Backends: `DirectGlyphRun` (atlas), `SDFGlyphRun` (`sdf::SdfGen`), `PathGlyphRun` (large text), `TransformedMaskGlyphRun` (bitmap emoji). The choosing logic and the atlas machinery are documented in [[text#Glyph rendering dispatch]].

## Software renderer

`src/render/sw/` is the CPU fallback (`SKITY_SW_RENDERER`, `SKITY_CPU` macro).

`SWCanvas` handles spans/clips/layers recursively; `SWRaster` + `SWEdge` + `WalkEdges` do scanline coverage; `SWSpanBrush` subclasses (solid/gradient/pixmap) shade spans; `sw_stack_blur` implements stack blur; clip is span set algebra. A8 drawables and subpixel positioning have dedicated helpers.

## Frame presentation

On-screen flow: `surface->Flush()` → present. Only Vulkan implements `GPUPresenter` and `GPUSemaphore` (see [[gpu#Backends]]).

Field-tested constraint worth remembering: on Mali (mt6983), leaking acquired swapchain images drives `dequeueBuffer` into `-38` and a driver crash — acquired-image bookkeeping and a pending `DiscardSurface` follow-up live here.
