# GPU Abstraction Layer

The GPU layer under `src/gpu/` is skity's hardware abstraction: backend-neutral interfaces with WebGPU-style naming, plus four symmetric backend implementations — GL, Vulkan, Metal, and Web (Emscripten WebGPU).

## Context and device

Two-level abstraction splits "public entry point" from "backend resource factory".

- `GPUContext` (`include/skity/gpu/gpu_context.hpp`) — the public pure-virtual entry: creates surfaces, presenters, textures, render targets, semaphores; exposes resource-cache limits and AA/tessellation toggles. One instance per render thread; texture upload must stay on the creating thread.
- `GPUContextImpl` (`src/gpu/gpu_context_impl.hpp`) — template-method base. Its `Init()` assembles the shared machinery (`GPUDevice` + `TextureManager` + `HWRenderTargetCache` + `HWPipelineLib` + atlas manager); backends only override `CreateGPUDevice()`, `OnWrapTexture()`, `OnCreateRenderTarget()`, `OnReadPixels()`.
- `GPUDevice` (`src/gpu/gpu_device.hpp`) — pure-virtual backend resource factory: create buffer/shader function/render pipeline/command buffer/sampler/texture, `CanUseMSAA()`, alignment and max-texture queries, and holds `GPUCaps`.

Backend context creation entry points (public free functions):

| Backend | Entry | Header |
| --- | --- | --- |
| OpenGL / ES / WebGL2 | `GLContextCreate(proc_loader)` | `include/skity/gpu/gpu_context_gl.hpp` |
| Vulkan | `CreateGPUContextVK(GPUContextInfoVK*)` | `include/skity/gpu/gpu_context_vk.hpp` |
| Metal | `MTLContextCreate(device, queue)` | `include/skity/gpu/gpu_context_mtl.h` |
| WebGPU | `WebContextCreate(WGPUDevice, WGPUQueue)` | `include/skity/gpu/gpu_context_web.hpp` |

When the Vulkan backend is compiled out, `src/gpu/vk/gpu_context_vk_stub.cc` still provides the symbols returning nullptr, so consumers link and load fine either way.

## Backends

One directory per backend under `src/gpu/`; all four mirror the same interface family.

Shared per-backend file family: `gpu_context_impl_*`, `gpu_device_*`, `gpu_command_buffer_*`, `gpu_render_pass_*`, `gpu_render_pipeline_*`, `gpu_buffer_*`, `gpu_texture_*`, `gpu_sampler_*`, `gpu_blit_pass_*`, `gpu_surface_*`.

- `src/gpu/gl/` — GL functions go through a global `GLInterface` function table (loaded via glad, header retains a Skia BSD credit); `Versions` parses GL/ES variants; `GLDriverWorkarounds` (e.g. `use_draw_for_clear`) is a first-class concept. Surfaces pick one of four present strategies (`GLSurfaceMode`: kAuto/kDirect/kBlit/kDrawTexture → four `GPUSurfaceGL` subclasses). Texture polymorphism: placeholder / external / renderbuffer variants in `gpu_texture_gl.hpp`.
- `src/gpu/vk/` — the largest backend. All state lives in `VulkanContextState` (instance/device/queues/VMA allocator/`VkPipelineCache`/proc table/extension queries). Also home to the only `GPUPresenter` implementation (`GPUPresenterVK`), `GPUSemaphoreVK`, `GPUExternalTextureAHB` (Android AHardwareBuffer), `VulkanRenderPassCache` (VkRenderPass compatibility cache) and `VulkanPendingSubmission` (retains objects until the GPU fence completes).
- `src/gpu/mtl/` — Objective-C++ (`.mm`). Surfaces split into `MTLTextureSurface` and `MTLLayerSurface`.
- `src/gpu/web/` — Emscripten `<webgpu/webgpu.h>` (not Dawn). WGSL passes through natively; the caller must supply device and queue — skity never requests an adapter. Note the inconsistent suffixes here: `GPURenderPipelineWeb` (lowercase "eb") vs `GPUSurfaceImplWEB`.

`GPUBackendType` (`include/skity/gpu/gpu_backend_type.hpp`) enumerates kNone/kOpenGL/kVulkan/kWebGL2/kWebGPU/kMetal; `IsGPUBackendSupported()` answers at compile time (kWebGPU only under `__EMSCRIPTEN__`). WebGL2 is not a separate backend — it rides the GL implementation behind `SKITY_OPENGL`.

## Resources

How textures, buffers and samplers are abstracted and cached.

- `GPUTexture` / `GPUTextureProxy` (`src/gpu/gpu_texture.hpp`) — backend texture base; proxies support lazy initialization via `InitializeTextureProc`.
- `TextureManager` (`src/gpu/texture_manager.hpp`) — bridges public `Texture` objects (`include/skity/gpu/texture.hpp`) and `GPUTexture`s: `TextureKey` dedup, UniqueID→GPUTexture map, and a GPU-thread release queue (`ClearGPUTextures`); guarded by `SharedMutex`.
- `GPUBuffer` / `GPUBufferView` (`src/gpu/gpu_buffer.hpp`) — buffer abstraction with HostVisible/Private storage modes.
- `GPUSampler` (`src/gpu/gpu_sampler.hpp`) — sampler abstraction; the GL device additionally caches them in a `GPUSamplerMap`.
- Render-target and generic resource caches live one layer up in the renderer: `HWRenderTargetCache` and `HWResourceCache` (see [[render#Caches and buffers]]).

## Render passes and commands

The command model is record-then-encode, mirroring modern APIs.

- `GPUCommandBuffer` (`src/gpu/gpu_command_buffer.hpp`) — `BeginRenderPass(desc)`, `BeginBlitPass()`, `HoldResource()`, `Submit(GPUSubmitInfo*)`. Vulkan adds `VulkanPendingSubmission` to defer destruction until fence completion.
- `GPURenderPass` (`src/gpu/gpu_render_pass.hpp`) — two-phase: `AddCommand(Command*)` records into (arena-allocated) command lists, then `EncodeCommands(viewport, scissor)` does the backend-specific encoding. A `Command` carries pipeline + vertex/index/instance buffer views + up to 4 uniform/texture/sampler bindings + stencil reference/mask + scissor. Descriptors (`GPURenderPassDescriptor`, color/stencil/depth attachments with load/store ops) are WebGPU-shaped.
- `GPUBlitPass` (`src/gpu/gpu_blit_pass.hpp`) — texture/buffer uploads, mip generation, texture-to-texture copy.
- `GPURenderPipeline` + `GPURenderPipelineDescriptor` (`src/gpu/gpu_render_pipeline.hpp`) — WebGPU-style descriptor (vertex/fragment function, vertex layouts, blend state, depth/stencil, multisample). The GL implementation (`GPURenderPipelineGL`, owning a `GLProgram`) emulates render passes with FBO state tracking, `BlitFramebuffer`, and MSAA resolve (`GLMSAAResolveRenderPass`).
- Presentation: `GPUPresenter` (experimental, Vulkan-only) and `GPUSemaphore` for cross-GPU synchronization — see [[render#Frame presentation]] for the QueuePresent crash context on Mali.

## Shader plumbing

All built-in shaders are authored once in WGSL and translated at runtime by [[wgx]] — there is no external cross compiler (no shaderc/glslang/SPIRV-Cross).

- `GPUShaderModule` (`src/gpu/gpu_shader_module.hpp`) only parses (holds the `wgx::Program` AST); translation is deferred.
- `GPUShaderFunction` (`src/gpu/gpu_shader_function.hpp`) has `GPUShaderSourceType {kRaw, kWGX}`; the WGX variant carries module+entry+`CompilerContext` (bind-group reflection shared across VS→FS so binding slots stay consistent).
- Translation points per backend: GL `WriteToGlsl` (desktop vs ES, injects `GL_KHR_blend_equation_advanced`), Metal `WriteToMsl` (MSL 2.0), Vulkan `WriteToSpirv` (requires `WGX_SPIRV_BACKEND`, mandatory with `SKITY_VK_BACKEND`), Web passes WGSL through natively plus `GetWGSLBindGroups`.

The renderer side that generates WGSL geometry/fragment snippets and caches pipelines is described in [[render#Draw steps, geometry and fragments]].

## Capability negotiation

`GPUCaps` (`src/gpu/gpu_caps.hpp`) declares backend abilities — framebuffer fetch, native advanced blend, dual-source blending — that mirror one-to-one into `GPUShaderFeature` requests; how a backend satisfies a request is an implementation detail.

One deliberate coupling: the native-blend shader variant only consumes a pipeline-cache slot on GL, because Vulkan/Metal express it via pipeline blend state.

## Known backend-specific decisions

Rationale worth remembering before touching these areas.

- **`GPUObjectVK` holds a weak `VulkanContextState` reference** (`src/gpu/vk/gpu_object_vk.hpp`, comment at the top): the chain `state → pending submission → cleanup action → GPU object → state` would form a reference cycle, and after device loss handle destruction must be skipped anyway.
- **GL render-target cache is force-disabled** (`GPUContext::IsRenderTargetCacheEnabled()` returns false, comment in `gpu_context.hpp`): cached GL FBO depth/stencil attachments can be rewritten by other passes, which would break stencil-based path rendering. See also the RT-cache stale-entry follow-up tracked for `prepare` error paths.
- **GL framebuffer Y flip** is corrected in `GPUContextImpl::MakeSnapshot` via a canvas transform rather than in the blit itself.
