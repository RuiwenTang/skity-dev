# C API

The C API module (`module/capi/`) exposes skity to C and other languages with a Vulkan-style ABI: opaque handles, descriptor structs, result codes. Design doc: `module/capi/C_API_DESIGN.md`.

## Goals and stages

Why a C API exists alongside the C++ one: a stable C ABI is the only interface Lynx (and Swift/Rust/Go/C# bindings) can consume across compiler versions, and it lets the final shipped library export nothing but C symbols.

Adoption is staged (see the design doc): Stage 1 is a separate `libskity-capi.so` wrapping the C++ library (current state); Stage 2 uses `module/capi/include/skity_hpp/skity_bridge.hpp` — a temporary, non-owning C++-side bridge for callers mid-migration; Stage 3 targets the main library exporting only C symbols plus a header-only RAII layer. Android measurements put the end-state gain around ~90–150 KB.

## Terminal include layout

Stage 3 end state for `include/skity/`: only the C headers (moved from `skity_c/`, aggregated as `<skity/skity.h>`) plus the header-only `skity.hpp` wrapper — the `vulkan.h`/`vulkan.hpp` shape. The old C++ public headers leave the public tree.

Migration rules for reaching it:

- Internalize the C++ headers first. 205 files under `src/` today include 58 distinct `<skity/...>` public headers, so the old headers must move into the internal tree before the public face can shrink. This is the same workstream as the `SKITY_DLL` visibility fix (see [[capi#C API#Type mirroring]]) — both are "make the C++ surface an implementation detail".
- The include path switches exactly once, bound to the Stage 3 major bump: `<skity_c/...>` → `<skity/...>`. The header-only wrapper is named `skity.hpp` from day one (staged under `skity_hpp/`) so only the directory prefix changes, never the file name.
- This layout also settles packaging (see [[build#Build System#Platform packaging]]): a single header tree means a source-pod `header_mappings_dir = 'include'` yields `<skity/skity.h>` directly, no umbrella collision (the C header sits in the `skity/` subdir while framework umbrellas stay at the top level), and CMake install, Android prefab, and CocoaPods all agree on one path.

## Header-only RAII layer

`module/capi/include/skity_hpp/skity.hpp` wraps the C handles in RAII types compiled into the consumer's TU; end-to-end demo: `example/case/c_api_hpp`.

One wrapper header per C domain header, same file name with a `.hpp` extension (`skity_c/skity_paint.h` → `skity_hpp/skity_paint.hpp`); the aggregate `skity.hpp` is the consumer entry and also pulls in `<skity_c/skity.h>` so the mixed wrapper + raw-C usage needs one include. Domain headers are self-sufficient individually (tighter compile deps when included directly); enum homes mirror the C side (`ClipOp` lives in `skity_canvas.hpp` because `skity_clip_op` lives in `skity_canvas.h`, not `skity_types.h`).

Owning wrappers (Paint, Path, TextBlob, Font, Typeface, Context, Surface, SoftwareCanvas) derive from a move-only `detail::OwnHandle` calling `skity_*_destroy`; `Canvas` is a non-owning view.

`Rect`/`Matrix` inherit the C PODs (static_assert on sizeof) so they cross the ABI by upcast; their legacy helper methods (MakeXYWH, PreConcat, MapRect, ...) are implemented inline here — the reverse-lookup-approved home for C++-only helpers, never new C functions.

The wrapper lives in **`namespace skity::raii`** (vk::raii precedent), NOT `skity::`, and this is load-bearing: libskity.dylib still exports ~418 legacy `skity::` C++ symbols (the `SKITY_DLL` leak). Wrapper classes intentionally mirror legacy names and signatures, so at -O0 (or whenever a member call is not inlined) a call site mangles to the same symbol as the legacy class method and the dynamic linker binds it to the legacy implementation, which then runs with the wrapper object as `this` and silently corrupts it (verified: `Paint::SetStrokeWidth(2.f)` overwrote the high half of `handle_` with 2.0f). The namespace collapses back to plain `skity::` in the same Stage 3 bump that hides the C++ symbols and moves the headers.

vulkan.hpp needs no such guard because its world already is the Stage 3 end state: libvulkan exports C symbols only, so an uninlined `vk::` call is a weak inline symbol with no exported surface to mis-bind against; and `vk::raii` itself was layered for a different reason — coexisting ownership models (plain handle wrappers, `vk::UniqueHandle`, `vk::raii`), a permanent split we do not have. Full comparison and the borrowed conventions (zero out-of-line symbols, detail namespace): `module/capi/C_API_DESIGN.md` §8.1–8.2.

Dual-API example (`example/case/basic`): the same drawing sources compile against both APIs — `SKITY_EXAMPLE_HPP` selects the backend and `namespace sk =` is the common spelling, so `example.cc` is written once with `sk::Canvas&`. The frame glue (`main.cc`) stays on the legacy C++ API in both modes and lends the canvas through [[capi#C API#Goals and stages]]'s bridge (`skity_canvas_from_native`) when the wrapper is selected; the two APIs coexist in one TU safely precisely because the wrapper lives in `skity::raii`. Compilation failures of the `_hpp` target are the wrapper's coverage checklist (this drove: legacy-shaped enum aliases like `Paint::kFill_Style`, float-RGBA color overloads, `SetShader(const Shader&)` temporaries, `RRect`, Shader/PathEffect wrappers, FontManager/TypefaceDelegate); the few genuine shape differences (shared_ptr `->` vs value `.`, TextBlobBuilder vs factory) are localized `#ifdef`s.

The switch is compile-time only, by decision. A namespace alias binds at compile time, so one-source dual-API cannot be runtime; a runtime switch would need two compiled drawing implementations (double the code size — against the size KPI) and loses the coverage-checklist property. What migration actually needs is coexistence, which is already free: TUs pick their API independently and meet at the bridge. Rollback stays a build-time affair (swap the include back and rebuild); pixel A/B comparison runs the two binaries separately, the golden-test shape.

Known pre-existing lib bugs found while validating (both reproduce in pure C, not wrapper defects): `skity_font_get_metrics` on a default font dereferences a NULL typeface inside `ScalerContextDesc::MakeCanonicalized`, and `skity_context_create_gl` requires a live GL context (glad calls jump to NULL otherwise).

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
