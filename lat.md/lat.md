# Skity Overview

Skity is an open-source C++ 2D graphics library focused on GPU rendering, developed by the Lynx family. It targets OpenGL/ES, Metal, experimental Vulkan, and WebGPU under Emscripten, plus a software rasterizer fallback. Apache-2.0 licensed.

This directory is the knowledge graph of skity's architecture. Each file documents one subsystem — what it does and why it is designed that way. Start from [[architecture]] for the layering, then dive into the subsystem pages.

## Subsystem map

Each subsystem has a dedicated page:

- [[architecture]] — layering, draw-call data flow, cross-cutting design decisions
- [[graphic]] — public drawing API: Canvas, Paint, Path, Image, Bitmap
- [[geometry]] — 3D-ready math primitives, curves, stroking
- [[render]] — rendering pipeline: HW (GPU) renderer, SW rasterizer, path rasterization strategies
- [[text]] — font/typeface stack, glyph caching, glyph rendering dispatch
- [[effect]] — shaders, color/image/mask filters, path effects
- [[recorder]] — Picture recording, display list format, partial redraw
- [[gpu]] — GPU hardware abstraction (context/device/resources/render passes) and per-backend notes
- [[wgx]] — the in-tree WGSL cross compiler (WGSL → GLSL/MSL/SPIR-V)
- [[capi]] — the C API module: handle model, ABI strategy
- [[build]] — CMake/GN/habitat build system, platform packaging
- [[tests]] — unit/golden/bench/font-harness test organization

## Repository layout

The tree is organized so that public API, domain logic, renderers, and GPU backends live in separate layers:

| Path | Contents |
| --- | --- |
| `include/skity/` | Public C++ API headers (graphic, geometry, text, effect, render, recorder, gpu, io, utils) |
| `src/` | Core library implementation (base, io, utils, geometry, graphic, effect, render, text, recorder, gpu) |
| `module/` | Satellite modules: `capi` (C API), `codec` (image codecs), `io` (streams/picture serialization), `wgx` (WGSL cross compiler) |
| `platform/` | Packaging: Android prefab AAR (`android/`), Apple xcframework (`darwin/`) |
| `test/` | Unit tests (`ut/`), golden pixel tests (`golden/`), benchmarks (`bench/`), font resources (`fonts/`) |
| `harness/` | Font metrics probe harness (`skity-font` CLI) |
| `third_party/` | Pinned third-party dependencies (patched via `patches/`, managed by habitat) |
| `cmake/`, `hab/`, `tools/` | Build config, dependency manifests, scripts (incl. `tools/test-runner.py`) |

## Product constraints

Two constraints shape most decisions in this codebase:

1. **Binary size is a first-class KPI.** The library ships inside the Lynx runtime, so features are judged against the size they add. This motivates the single-shader-source strategy ([[wgx]]), `-fno-exceptions -fno-rtti -fvisibility=hidden`, LTO/`--gc-sections` on mobile targets, and the "CMake as source of truth, GN generated" rule ([[build]]).
2. **No external shaping/translation dependencies.** Text shaping is deliberately left to the host via `TypefaceDelegate` ([[text]]), and shader translation is done in-tree by wgx instead of shaderc/glslang/SPIRV-Cross ([[wgx]]).
