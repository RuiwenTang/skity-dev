# wgx

wgx (library target `wgsl-cross`, `module/wgx/`) is skity's in-tree WGSL cross compiler: WGSL in, GLSL / MSL / SPIR-V out.

It exists so skity can author shaders once in WGSL and run on every backend without shipping shaderc, glslang, or SPIRV-Cross — a binary-size and dependency-hygiene decision (see [[architecture#Cross-cutting decisions]]).

## Pipeline stages

Compilation is staged, front to back (`module/wgx/`, public header `module/wgx/include/wgsl_cross.h`, namespace `wgx`):

1. **Frontend** (`wgsl/`) — scanner → parser → `Program` AST (`wgsl/ast/`); entry is `wgx::Program::Parse(source)`.
2. **Semantics** (`semantic/`) — resolver, scopes, symbols.
3. **Lowering** (`lower/`, `ir/`) — lower-to-IR; the IR layer is the growing direction (see [[wgx#Roadmap]]).
4. **Emission** — `WriteToGlsl(entry, GlslOptions, ctx)` (`glsl/glsl_ast_printer`), `WriteToMsl(entry, MslOptions, ctx)` (`msl/msl_ast_printer`, `msl/uniform_capture`), `WriteToSpirv(entry, SpirvOptions, ctx)` (`spirv/` emitter, gated by `WGX_SPIRV_BACKEND`).

Reflection is part of the contract: `GetWGSLBindGroups(entry)` returns `BindGroup`/`BindGroupEntry` structures, and a `CompilerContext` carries UBO/texture/sampler slot assignments across the VS→FS pair so binding layouts stay consistent within a pipeline.

## Deliberate WGSL subset

The frontend intentionally implements a subset of WGSL, documented in `module/wgx/README.md`.

Excluded: compute shaders, type inference, `override` constants, `const_assert`, global directives; diagnostics are terse. The subset matches what skity's generated shaders need — nothing more — which keeps the compiler small.

## Integration points

Who calls wgx, and where:

- `GPUShaderModule` (`src/gpu/gpu_shader_module.hpp`) parses and holds the AST only.
- Each GPU device translates at pipeline-creation time: GL `WriteToGlsl` (desktop vs ES variants, extension injection), Metal `WriteToMsl` (MSL 2.0), Vulkan `WriteToSpirv` (mandatory: `SKITY_VK_BACKEND` requires `WGX_SPIRV_BACKEND`); WebGPU passes WGSL through natively. See [[gpu#Shader plumbing]].
- The renderer generates the WGSL itself from geometry/fragment snippets ([[render#Draw steps, geometry and fragments]]) — wgx never sees hand-written per-backend shader sources.

## Roadmap

`module/wgx/docs/ROADMAP_IR_SPIRV.md` records the direction.

Today's AST-direct printers are lightweight but allow name collisions and duplicated semantics; the migration moves printing behind semantic analysis and a proper IR, enabling multiple emission backends (SPIR-V included) with deterministic codegen.
