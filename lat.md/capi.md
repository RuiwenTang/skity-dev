# C API

The C API module (`module/capi/`) exposes skity to C and other languages with a Vulkan-style ABI: opaque handles, descriptor structs, result codes. Design doc: `module/capi/C_API_DESIGN.md`.

## Goals and stages

Why a C API exists alongside the C++ one: a stable C ABI is the only interface Lynx (and Swift/Rust/Go/C# bindings) can consume across compiler versions, and it lets the final shipped library export nothing but C symbols.

Adoption is staged (see the design doc): Stage 1 is a separate `libskity-capi.so` wrapping the C++ library (current state); Stage 2 uses `module/capi/include/skity_hpp/skity_bridge.hpp` — a temporary, non-owning C++-side bridge for callers mid-migration; Stage 3 targets the main library exporting only C symbols plus a header-only RAII layer. Android measurements put the end-state gain around ~90–150 KB.

## Handle model

Every C-side object is an opaque pointer to a wrapper struct with a common header (`module/capi/src/handle.hpp`):

- `skity_object_header { uint32_t type; uint32_t flags; }` is the **first member** of every wrapper, enabling runtime type-tag validation in `resolve<T>()` — the typed-handle refactor. The tag enum `skity_object_type` covers ~29 object kinds.
- `SKITY_C_DEFINE_HANDLE(name)` declares the `typedef struct name##_s* name` pattern; consumers never see the definition.
- Ownership is explicit via `SKITY_HANDLE_OWNING`: owning handles hold a default-deleter `shared_ptr` to the impl; borrowed handles (e.g. the canvas from `skity_surface_lock_canvas`) hold a no-op deleter so destroying them only frees the wrapper.
- Allocation uses nothrow `new`, matching the no-exceptions build ([[architecture#Cross-cutting decisions]]).

The header/type-tag stays internal — it is not part of the public C ABI, which remains pure opaque pointers.

## Header organization

`module/capi/include/skity_c/skity.h` aggregates ~25 domain headers (canvas, paint, path, surface, text, recorder, ...). Two families are deliberately kept out of the aggregate:

- **Vulkan headers** (`skity_context_vk.h`, `skity_surface_vk.h`, `skity_semaphore_vk.h`, `skity_texture_vk.h`, `skity_native_window_vk.h`) — including them would force `<vulkan/vulkan.h>` on non-Vulkan consumers. The matching implementations (`semaphore_vk.cpp`, `native_window_vk.cpp`) compile only under `SKITY_VK_BACKEND`.
- Foundation headers: `skity_base.h` defines `SKITY_C_API` export macros, the handle macro, and `skity_result` codes (SKITY_SUCCESS, INVALID_HANDLE, INVALID_ARGUMENT, INITIALIZATION_FAILED, OUT_OF_HOST_MEMORY, NOT_SUPPORTED, NEED_RECREATE).

Implementations follow one-file-per-domain under `module/capi/src/` (`canvas_c.cpp`, `paint_c.cpp`, `recorder_c.cpp`, ...).

## Type mirroring

`skity_types.h` mirrors the C++ value types as C PODs, laid out member-for-member so values cross the boundary by `reinterpret` (the build relies on `-fno-strict-aliasing`).

Mirrored types: `skity_vec2/3/4`, `skity_point`, `skity_rect`, column-major `skity_matrix[16]`, `skity_color`, `skity_blend_mode`. Two known fidelity points when wrapping: `Paint` exposes more than the C mirror currently carries, and `Typeface` is an abstract class in C++, so its C functions must resolve through a concrete port ([[text#Typeface and Font]]).

Known migration debt tracked in the two-phase plan: `SKITY_DLL` visibility leakage from the C++ side is the root cause blocking full symbol isolation, plus a list of not-yet-referenced entry points on 32-bit x86 audits.

## Coverage discipline

The C API face is grown by reverse lookup, not by mirroring the C++ surface: only entry points with proven callers (audited in the animax + clay repos) become ABI commitments.

Shaping findings that shaped the current face: `Rect`/`Matrix` methods have only C++ callers (solution is inline-ization in the header-only layer, not C functions); `TextRun`'s proven need is the build side — callers run their own shaping ([[text#Text Stack#Shaping model]]) and inject glyph ids + positions via `skity_text_blob_create_from_glyphs`; `Pixmap`'s is zero-copy buffer wrapping (`skity_data_make_with_proc` → `skity_pixmap_create` → `skity_bitmap_create_from_pixmap`), while `WritableAddr8/16`, `GetID`, and pixel-change listeners have zero caller demand and stay out of the ABI.
